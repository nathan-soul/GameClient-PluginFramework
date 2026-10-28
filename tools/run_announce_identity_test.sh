#!/usr/bin/env bash
# Compiles and runs the announce identity outside the engine.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
INCLUDE="$HERE/../GeneralsMD/Code/GameEngine/Include"

g++ -std=c++17 -Wall -Wextra -I "$INCLUDE" -o /tmp/announce_identity_test "$HERE/announce_identity_test.cpp"
/tmp/announce_identity_test
