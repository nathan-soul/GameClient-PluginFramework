# Capture feature — the engine and ABI side

This is the framework half of the map-capture feature: the accessors a plugin needs to describe the
frame it is looking at, and the client-side writer that turns one keypress into a PNG plus a JSON
sidecar.

**The end-to-end document lives in the other repo:**
`overlay-plugin/docs/capture-feature.md`. It covers the studio, the sidecar schema, the build box and
the consumer; this file covers only what is in this repository, and links there for everything else.

| | |
|---|---|
| Repo | `nathan-soul/GameClient-PluginFramework` |
| Branch | `pr-upstream-base`, pushed as `feature/plugin-capture` |
| Committed tip | `6b3f55a6a` |
| Rebased onto | `92289d416` (upstream, 2026-09-29) — the August base heap-corrupted at startup |
| Working tree | **modified, uncommitted** — the capture implementation, observed 2026-10-29 20:55 |

Version 1 of the ABI is what ships; the capture additions are not committed and have never been
compiled. Each section says which side of that line it is on.

---

## 1. What the engine already had

`GeneralsMD/Code/GameEngine/Include/GameNetwork/GeneralsOnline/Plugins/PluginABI.h` is the whole
contract: pure C, POD structs, function pointers, no engine types, `GO_PLUGIN_ABI_VERSION 1`, a
`structSize` field a plugin checks before touching the table, and `GO_ABI_ASSERT_LAYOUT` size asserts
for the 32-bit build.

Relevant to capture, committed:

| Entry | What it gives | Limit |
|---|---|---|
| `worldToScreen(x,y,z,outX,outY)` | the engine's own projection, integer pixels | 0 off-frustum, outputs untouched |
| `worldToScreenClamped` | same, clamped just inside the view edge | 0 only when no projection is possible |
| `getObjectHealthBarScreenPosition(id,…)` | the health-bar anchor, projected | 0 if gone or off-screen |
| `getObjectScreenBounds(id,…)` | screen box from the bounding radius (square) | screen pixels only |
| `getObjectBuildingCategory(id)` | five buckets | producing buildings only |
| `enumeratePlayerObjects(playerIndex,cb,ud)` | id + world position per player | per player; **excludes neutral** |
| `getContainedObjects(id,out,max)` | occupants: id, template, player | |
| `getObjectContainerId(id)` | container an object is inside, or 0 | |
| `getScreenSize`, `getLogicFrame`, `getLogicFramesPerSecond` | render size and simulation clock | |
| `isLocalPlayerObserver` | true when spectating | host withholds every callback while false |
| `getUserDataPath` | per-user directory, trailing backslash | where capture files belong |

What it did not have, and the capture needs: the camera, the map, an object list including neutral
owners, and a template name per object. Those are the four things this branch adds. The plugin cannot
get them any other way, which is why the capture lives in the client rather than in the plugin
(`../overlay-plugin/docs/superpowers/specs/2026-10-29-capture-location-decision.md`).

---

## 2. `getCameraState` — BUILT, committed, compiled

Added by `6f823ce20`, fixed by `41e6e4527`.

```c
struct GOCameraState
{
    float pos[3];                    // eye position, world units
    float target[3];                 // look-at point, world units
    float yawRadians;                // heading, engine convention
    float pitchRadians;              // elevation, engine convention (sign unverified in game)
    float fovHorizontalDegrees;      // Generals' FOV is horizontal
    int32_t viewportX, viewportY, viewportWidth, viewportHeight;
    uint32_t logicFrame;             // simulation clock at sample time
    uint8_t paused;
    uint8_t valid;                   // 1 when the rest of the struct means anything
};
GO_ABI_ASSERT_LAYOUT(sizeof(GOCameraState) == 60);
```

Implementation (`PluginManager.cpp`, `HostAPI_GetCameraState`):

- eye and direction from `TheTacticalView->get3DCameraPosition()` / `get3DCameraDirection()`;
- `target` from `TheTacticalView->getPosition()` — the engine's own look-at point, carried **instead
  of** trusting the reported pitch, because the FX pitch moves the target's Z;
- `pitchRadians = asinf(-dir.z / len)`, sign **unverified in game**;
- `fovHorizontalDegrees` from `View::getFieldOfView()` (horizontal, `View.h:246`);
- `viewport` from `TheDisplay->getWidth()/getHeight()` — the display, **not** the tactical view's
  sub-rect. For a letterboxed or split view that is not necessarily the area `worldToScreen` projects
  into; unreconciled;
- matrices deliberately absent: exact projection should call `worldToScreen`, not rebuild the
  pipeline across the ABI.

`41e6e4527` exists because the first attempt called `get3DCamera()`, which lives on `W3DView` —
`PluginManager` only ever sees `View*` (`TheTacticalView`), so the camera-object route was wrong twice:
wrong class, and it would have tied this device-independent manager to the W3DDevice layer.

**Compiled.** `2f057c90b` fixed zlib's `Byte` typedef colliding with `BaseTypeCore.h`'s, and the build
on the box finished `[1006/1006] Linking CXX executable GeneralsMD\Release\GeneralsOnlineZH_60.exe`
(`build4.log`, 20:35). That exe contains `getCameraState` and nothing else from this feature.

---

## 3. The capture accessors — PARTIAL, uncommitted

Added to the same header, implemented in the same file, **in the working tree only**. Never compiled.

```c
struct GOMapInfo { char name[64]; int32_t extent[2]; int32_t valid; };          // 76
struct GOObjectInfo {
    int32_t id; char template_name[64]; int32_t owner_index;                  // -1 = neutral
    float pos[3]; float radius; int32_t is_structure; int32_t complete;
    int32_t screen[2]; int32_t on_screen;
};                                                                            // 108
struct GOCaptureRequest {
    const char* name; const char* trigger;
    const GOCameraState* camera; const GOObjectInfo* objects; int32_t object_count;
};                                                                            // 20
```

| Function | Behaviour | State |
|---|---|---|
| `getLocalPlayerIndex()` | local player index, `-1` when none | **PARTIAL** — uncommitted |
| `getMapInfo(GOMapInfo*)` | map file stem from `TheGlobalData->m_mapName`, extents from `TheTerrainLogic->getExtent()`; `valid=1` whenever a game is loaded, even with zero extent (the base `TerrainLogic` is a stub) | **PARTIAL** — uncommitted |
| `getMapObjects(out, max, kindMask, outTotal)` | walks `TheGameLogic`'s **world object list** (so neutral/civilian owners are included), skips dead objects, classifies `KINDOF_STRUCTURE` vs `KINDOF_VEHICLE\|INFANTRY`, projects the position with `worldToScreen`, sets `on_screen` only when inside `TheDisplay`'s width/height | **PARTIAL** — uncommitted |
| `requestCapture(name)` | `trigger="api"`, no metadata; host samples its own camera and objects | **PARTIAL** — uncommitted |
| `requestCaptureEx(req)` | copies name, trigger, camera struct and object array into `s_pendingCapture`; refuses when no game or a capture is already pending | **PARTIAL** — uncommitted |

`getMapObjects` is the accessor that makes the feature work at all: `enumeratePlayerObjects` is
per-player and excludes neutral, so a civilian garrison building would be invisible through it. The
world list is what "destroy all objects" iterates, and it has no such hole.

---

## 4. The writer — PARTIAL, uncommitted

### 4.1 A screenshot to a caller-chosen path

`Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DScreenshot.cpp:253`:

```c
bool W3D_TakeCompressedScreenshotTo(const char* pathname, Int* outWidth, Int* outHeight);
```

Same readback as the shipped `W3D_TakeCompressedScreenshot` (line 166) — `_Get_DX8_Back_Buffer`,
system-memory copy via `_Create_DX8_Surface` + `_Copy_DX8_Rects`, `Lock`, BGRA→RGB, `stbi_write_png`
— but synchronous and to a path the caller chooses. The ordinary screenshot path picks its own
`sshot_YYYYMMDD_HHMMSS_mmm.png` under `Screenshots\` on a worker thread and posts a UI message; a
capture needs a predictable pair, so that route is the wrong shape.

### 4.2 The seam and the frame-end hook

- `GOPluginManager::SetScreenshotWriter` (declared in `PluginManager.h`) is the same pattern as the
  D3D device and window handles: `W3DDisplay::init` installs it at `W3DDisplay.cpp:1054`, through a
  static adapter that forwards to `W3D_TakeCompressedScreenshotTo`. Null in a headless build; the
  capture then refuses rather than writing a sidecar with no image.
- `GOPluginManager::ProcessPendingCapture()` is called from `W3DDisplay::render`, immediately before
  `WW3D::End_Render()` (`W3DDisplay.cpp:2173`). At that point the back buffer holds the frame the
  plugin's metadata was sampled on, and the plugin's ImGui menu (drawn from an `EndScene` hook) has
  not touched it yet. **That ordering is a code-reading argument, not a measurement** — it is the
  assumption the feature's "one frame" property rests on.
- The pending flag is cleared **before** the write is attempted, so a failure does not retry every
  frame; a failure is one log line.

### 4.3 `WriteCapturePair`

Writes `<userData>\OverlayPlugin\captures\<slug>__<YYYYMMDD>-<HHMMSS>__<W>x<H>[-n].{png,json}`:

1. metadata from the request (or sampled from the host when the request carried none);
2. objects from `getMapObjects` with a 4096-entry buffer when the request carried none;
3. garrisons by walking the world list for `getContain()` + `getContainCount() > 0`, projected with
   `worldToScreen` and dropped when outside the display;
4. PNG to `<name>.png.tmp` through the installed writer; the written size is authoritative, and the
   pair is renamed if it disagrees with the display size;
5. JSON built by hand, written to `<name>.json.tmp`;
6. PNG moved, then JSON moved (`MOVEFILE_REPLACE_EXISTING`);
7. `captures\manifest.js` rewritten as `window.OVERLAY_CAPTURES = [{name, png, json, w, h, utc, map,
   buildingCount}]`, newest first;
8. `TheInGameUI->message("Capture saved: <file>")`.

Known wart: if the JSON's **rename** fails, its temporary is deleted but the already-moved PNG is left
behind — a PNG with no sidecar. The JSON's *write* failure path is clean.

### 4.4 What the sidecar contains

`buildings[]` and `units[]` hold only objects whose base point was inside the viewport; everything
else is counted in `counts.dropped_offscreen`. `garrisons[]` is occupancy only — no capacity, no
`garrisonable` flag — because the caster garrisons before capturing. `transform` is `null` on
purpose: the dots come from the engine's own projection, so a derived matrix would only compete with
it. `camera.roll_deg`, `zoom`, `near`, `far`, `ground_z` are `null`; `forward/right/up` are *derived*
from eye and look-at, not read from a view matrix.

The full key-by-key contract, the three schema decisions and the studio's reader are in
`overlay-plugin/docs/capture-feature.md` §3.

---

## 5. ABI version — a correctness item, not a style one

`GO_PLUGIN_ABI_VERSION` is **1**, and the loader's only guard is
`info.abiVersion != GO_PLUGIN_ABI_VERSION` (`PluginManager.cpp:1321`).

Since `6f823ce20`, `getCameraState` has been inserted **mid-table** (after `getScreenSize`); the
working tree adds `getLocalPlayerIndex` after `isLocalPlayerObserver` and four more after
`getCameraState`. Every insertion shifts later slots. A plugin built against an older header reports
ABI 1, passes the check, and calls the wrong pointer: its `getLogicFrame()` lands on
`getCameraState`, which memsets 60 bytes through an argument the caller never passed. The overlay
plugin does call `getLogicFrame` (`OverlayPlugin/src/OverlayState.cpp:747`).

Bump the version before any of this is published. The header's own rule says to.

---

## 6. Building it

Box: `ssh gencast-build` → `dennis@192.168.2.113`, Windows. Checkout: `C:\build\plugin-capture`
(branch `capture`, fetched from `gh` = `nathan-soul/GameClient-PluginFramework`, seeded from
`C:\Users\Dennis\gencast-static.bundle`).

```
cmake --preset win32-vcpkg -DCMAKE_MAKE_PROGRAM=<ninja> \
  -DVCPKG_TARGET_TRIPLET=x86-windows-static-md \
  -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" \
  -DRTS_BUILD_OUTPUT_SUFFIX=_60 \
  -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_GENERALS=OFF \
  -DRTS_BUILD_CORE_TOOLS=OFF -DRTS_BUILD_ZEROHOUR_TOOLS=OFF \
  -DRTS_BUILD_CORE_EXTRAS=OFF -DRTS_BUILD_ZEROHOUR_EXTRAS=OFF
cmake --build --preset win32-vcpkg
```

All of it runs from `C:\build\build-capture.bat`, whose first line is the part that matters. Output:
`build\win32-vcpkg\GeneralsMD\Release\GeneralsOnlineZH_60.exe` (14,426,624 bytes at 20:35).

### Traps, all three hit for real

1. **Run every `cmake` and `cl` inside `VsDevCmd.bat`.** Without the MSVC environment you do not get
   "compiler not found", you get C1083 on standard headers — `'utility'` from
   `Dependencies/Utility/Utility/CppMacros.h:25`, `'time.h'` from `resources/gitinfo/gitinfo.h:20`,
   `'winresrc.h'` from `GeneralsMD/Code/Main/RTS.RC(10)`. All three are in `build3.log`. The script
   opens with `call "…\VsDevCmd.bat" -arch=x86 -host_arch=x64`.
2. **The `Vendor/` headers are untracked.** `git ls-files` finds only three files under
   `GeneralsMD/Code/GameEngine/Include/GameNetwork/GeneralsOnline/Vendor/`:
   `DiscordRPC/discord_rpc.h`, `stb_image/stb_image_resize.h`, `stb_image/stb_image_write.h`;
   `libcurl/`, `sentry/` and `ValveNetworkingSockets/` exist only on the box, as untracked
   directories, copied from the gencast tree. A fresh clone cannot compile without them.
   `6b3f55a6a` removed the August vendor-header list from `GAMEENGINE_SRC` for the same reason —
   those filenames do not exist on this base.
3. **vcpkg's static-md zlib installs `zs.lib`; the link line wants `zlib.lib`.** `build.log` is one
   line: `ninja: error: 'vcpkg_installed/x86-windows-static-md/lib/zlib.lib' … missing and no known
   rule to make it`. After the fix both files are present, 455,100 bytes each with the same
   timestamp — `zs.lib`'s bytes placed at `zlib.lib`. A clean `vcpkg_installed` deletes it again.

---

## 7. What is not built

- Nothing here has been compiled since the capture landed; the box's exe predates it.
- No capture has been taken, so no sidecar has been read back.
- `transform` is null, so the projection self-check (F11) does not run.
- Truncation past 4096 objects is silent — `outTotal` is reported by `getMapObjects` and ignored by
  the writer.
- A failed JSON rename can leave an orphan PNG.
- `getCameraState`'s pitch sign and its display-sized viewport are both unreconciled.

The consumer side — studio, schema, markers, presets — is documented in
`overlay-plugin/docs/capture-feature.md`, including the stage-by-stage BUILT / PARTIAL / PLANNED
table.
