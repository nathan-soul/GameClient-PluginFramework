#!/usr/bin/env bash
# Builds and installs the combined build: the gencast client (with the plugin host) plus the overlay
# plugin and its configurator, into the Gencast install.
#
#   bash tools/combined/deploy-combined.sh
#
# The two halves are separate repos and stay separate: this script only moves each repo's own build
# output into one install. Override with OVERLAY_DIR / DEPLOY_HOST / DEPLOY_INSTALL_DIR.
set -euo pipefail
cd "$(dirname "$0")/../.."
ENGINE=$(pwd)
OVERLAY=${OVERLAY_DIR:-../overlay-plugin}
HOST=${DEPLOY_HOST:-gencast-build}
RUN="ssh -o BatchMode=yes $HOST"
TAR=/tmp/overlay-plugin.tar.gz

[ -d "$OVERLAY" ] || { echo "no overlay checkout at $OVERLAY (set OVERLAY_DIR)"; exit 1; }

echo "— bundling and packaging the studio"
python3 "$OVERLAY/tools/bundle.py" >/dev/null
# No pipe: package.py writes one line per file and a short-lived reader (head) kills it with
# BrokenPipeError halfway through the copy.
python3 "$OVERLAY/tools/package.py" >/dev/null

echo "— syncing the plugin tree to $HOST"
tar -C "$OVERLAY" --exclude=.git --exclude=build --exclude=__pycache__ --exclude=docs -czf "$TAR" .
scp -q "$TAR" "$HOST:C:/build/overlay-plugin.tar.gz"
scp -q "$OVERLAY/tools/deploy/sync.ps1" "$HOST:C:/build/sync-overlay.ps1"
$RUN 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\build\sync-overlay.ps1' | tail -1

# The plugin compiles against the engine's PluginABI.h, so the engine repo has to be current before
# the plugin builds - otherwise a new ABI field is invisible and the build fails on a stale header.
echo "— updating the engine tree"
$RUN 'cmd /c "cd /d C:\build\plugin-capture && git fetch -q gh combined/gencast-overlay && git checkout -q -B combined/gencast-overlay gh/combined/gencast-overlay && echo ENGINE_TREE_OK"' | grep -E 'ENGINE_TREE_OK|WRONG_BRANCH'

echo "— building the plugin against the engine's ABI"
$RUN 'cmd /c "cd /d C:\build\overlay-plugin-capture && tools\build-windows.bat C:\build\plugin-capture"' | grep -E 'CONFIGURE_EXIT|BUILD_EXIT|BUILD_OK|NO_ABI'

echo "— building the combined exe"
$RUN 'cmd /c "cd /d C:\build\plugin-capture && git fetch -q gh combined/gencast-overlay && git checkout -q -B combined/gencast-overlay gh/combined/gencast-overlay && tools\combined\build-combined.bat"' | grep -E 'BRANCH|CONFIGURE_EXIT|BUILD_EXIT|BUILD_OK|WRONG_BRANCH'

# The ABI check runs a mock host whose structSize comes from its own copy of PluginABI.h; refresh it
# after every engine build, or a grown ABI looks like a plugin that refuses to load.
echo "— refreshing the ABI test harness"
$RUN 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\build\refresh-harness.ps1' | grep -E 'HARNESS_REFRESHED|error'

echo "— checking the plugin against the host ABI"
$RUN 'cmd /c C:\build\run-host-test.bat' | grep -E 'plugin: name|INIT_OK|SHUTDOWN_OK'

echo "— installing the combined build"
OVERLAY_COMMIT=$(git -C "$OVERLAY" rev-parse --short HEAD 2>/dev/null || echo unknown)
scp -q tools/combined/install-combined.ps1 "$HOST:C:/build/install-combined.ps1"
$RUN "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\build\\install-combined.ps1 -OverlayCommit $OVERLAY_COMMIT"
