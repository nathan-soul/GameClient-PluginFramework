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
| plugin host | `PluginABI.h`, `PluginManager`, the render/gameplay hooks | `feature/plugin-capture` |

Neither tree contained the other's code, so "combined" could only be done by copying files between
them — which is how the two versions drift apart. `combined/gencast-overlay` is instead a merge of
the two branches, so each feature stays owned by the branch that develops it and the combined tree
is the only place they meet.

The merge had four conflicts, resolved as:

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
