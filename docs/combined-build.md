# The combined build

One exe with both feature lines, plus the overlay plugin and its configurator in the same install.

```bash
bash tools/combined/deploy-combined.sh
```

That builds both repos, installs into the Gencast install, and verifies what it installed. Nothing
is copied between the repos to make this work — see "Why a merge and not a copy" below.

## What it produces

```
<install>\GeneralsOnlineZH_60.exe                                   engine + plugin host
<install>\plugins\OverlayPlugin\OverlayPlugin.goplugin.dll          the overlay plugin
<install>\plugins\OverlayPlugin\configurator\index.html             the studio, double-click it
<install>\plugins\OverlayPlugin\BUILD.txt                           which commits are installed
```

## Why a merge and not a copy

The two features were developed on separate lines from the same base:

| Line | Has | Branch |
|---|---|---|
| gencast client | GO login, direct mode, the relay transport | `build/static-vcpkg-deps` |
| plugin host | `PluginABI.h`, `PluginManager`, the render/gameplay hooks | `pr-upstream-base` |

Neither tree contained the other's code, so "combined" could only be done by copying files between
them — which is how the two versions drift apart. `combined/gencast-overlay` is instead a merge of
the two branches, so each feature stays owned by the branch that develops it and the combined tree
is the only place they meet.

`feature/plugin-capture` looks like the obvious partner but is the wrong one: its `PluginABI.h` is
the older 25 KB header and its `PluginManager` does not implement the host calls the overlay plugin
makes (`isObjectProjectile`, `getObjectContainerId`, `GOObjectInfo`). `pr-upstream-base` carries the
same framework 1073 lines further on, and its header is byte-identical to the one the plugin was
written against - so that is the side that merges, cleanly and without conflicts.

The earlier attempt, against `feature/plugin-capture`, had four conflicts, resolved as:

- **MainMenuUtils.cpp** — the gencast side's removal of the pre-online patch/version check wins.
  This is what keeps Generals Online from prompting for an update on a locally built exe: the check
  compares a CRC of the exe file against a published release, so it answers NEEDS_UPDATE for any
  self-built binary, forever. The plugin side kept the call and forced `bNeedsUpdate = false`; the
  gencast side drops the call and goes straight to the online path.
- **Main/CMakeLists.txt** — keep `GeneralsOnlineZH${RTS_BUILD_OUTPUT_SUFFIX}` so the name stays a
  build flag rather than a literal.
- **GameEngine/CMakeLists.txt** — take the plugin host's header list and `PluginManager.cpp`.
- **StatsExporter.cpp** — keep the gencast side's zlib `Byte` workaround.

One follow-up was needed: the gencast side includes the Steam networking headers as `<steam/...>`,
which assumed a `Vendor/steam` folder it generated locally, while the plugin side carries them at
`Vendor/ValveNetworkingSockets/steam` and includes them by full path. One include directory
satisfies both spellings, so neither branch's source was rewritten.

## Where the plugin's ABI lives

In this repo, once: `GeneralsMD/Code/GameEngine/Include/GameNetwork/GeneralsOnline/Plugins/PluginABI.h`.
The overlay plugin repo no longer carries a copy; its CMake takes `GENCAST_ENGINE_DIR` (default
`../plugin-framework`) and fails at configure time when the header is not there. Its own check,
`tools/no-overlap.sh`, fails when any tracked file exists byte-for-byte in both trees.

## Verifying an install

`install-combined.ps1` checks, and fails the install if any of these is untrue:

- the installed exe is byte-identical to the build;
- the exe contains the plugin loader (`*.goplugin.dll`), so this really is the combined exe;
- the plugin's ABI harness loads the **installed** DLL (`INIT_OK`);
- every `assets/…` and `schema/…` reference in the configurator resolves inside the install;
- headless Edge renders the installed configurator.

What it cannot check from a headless box: that the game itself starts and loads the plugin. The log
line to look for is `[Plugin] Loaded OverlayPlugin v…` in `GeneralsOnline.log`.

## The configurator is served, not opened from disk

`plugins\OverlayPlugin\configurator\` is served by the plugin itself on `127.0.0.1:<ephemeral>` while
the game runs. F2 opens `http://127.0.0.1:<port>/?t=<token>` in the default browser, and the page
reads and writes the client's presets over HTTP.

That is not a convenience: a page opened from `file://` can only write to a folder through the File
System Access API, which Firefox does not have, and the shell resolves a `file:///` URL to a path —
dropping any query string on the way to the browser. Both were observed, not assumed.

- The token is generated per launch and logged with the URL (`OverlayPlugin: configurator at …`), so
  the page can also be reached by hand when no browser opens.
- `GET /?t=<token>` sets an `HttpOnly; SameSite=Strict` cookie; every later request must carry it.
- Only `*.ini` names directly in the presets folder are reachable; names are validated, not
  sanitised. Routing is `src/ConfiguratorRoute.cpp` and is unit-tested on Linux
  (`tests/server_route.test.cpp`); `src/ConfiguratorServer.cpp` is the socket loop around it.

If the bind fails the plugin still loads and F2 falls back to the `file://` page, which works but can
only write presets in a Chromium browser.

## When it dies right after login

A combined build once killed the process the instant Online was pressed, after `LOGIN: Logged in`
and one Steam networking line. The method that found it, in the order that paid off:

1. **Read the log, not the symptom.** `Documents\Command and Conquer Generals Zero Hour Data\GeneralsOnlineData\GeneralsOnline.log`.
   `NetworkLog` opens, writes and closes per line, so nothing is lost to buffering: the last line
   really is where it died.
2. **Compare with a working run.** The same log from the previous exe went `LOGIN: Logged in` →
   `[WebSocket] Connected`. The new one logged a Steam networking warning in between and stopped —
   that gap is the whole bug.
3. **A/B the previous exe** (`swap-exe.ps1`): if the old one works, the regression is in what
   changed, and the search space collapses to the merge plus the build configuration.
4. **Disable the plugin by renaming the DLL** to `.off`. It still crashed, which cleared the plugin
   in one launch instead of a debugging session.
5. **Do not rely on WER.** `LocalDumps` for the exe was configured and wrote nothing, and no
   Application Error event appeared either. Installing **procdump as the AeDebug postmortem
   debugger** (`procdump64 -ma -i <folder>`) catches what WER drops.
6. **Instrument the gap.** Five `NetworkLog` lines bracketing each call between the last good line
   and the death (`EnsureInitialized`, the watcher registration, the websocket connect) named the
   call — the crash was one call *after* the last of them, inside the Steam networking library.

### The cause, because it will happen again

The tree carries `Include/GameNetwork/GeneralsOnline/Vendor/ValveNetworkingSockets/steam/*.h` — a
copy of the Steam networking headers — while the library itself comes from vcpkg
(`GameNetworkingSockets::GameNetworkingSockets_s`). They are **different versions**, and the copy's
`SteamNetworkingConfigValue_t::SetString` tags the value `k_ESteamNetworkingConfig_Ptr` where the
library expects `k_ESteamNetworkingConfig_String`. An include directory that made `<steam/...>`
resolve to the copy therefore handed `CreateListenSocketP2P` its TURN server/user/pass as pointers,
the library read them as strings, logged `FakeIP allocation requires Steam` and died.

The copy is gone; `<steam/...>` now resolves through the imported target, so the headers are the ones
the library was built from. Two rules follow:

- **Never add an include directory that can shadow a dependency's own headers.** Link the target and
  let it supply them.
- **A vendored copy of a library's headers is a liability**, not a convenience: it compiles, links
  and then misreads its arguments at runtime. If a header must be vendored, it has to be diffed
  against the version the linked library ships.
