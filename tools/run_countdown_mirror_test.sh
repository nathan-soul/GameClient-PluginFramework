#!/usr/bin/env bash
# Compiles and runs the mirrored countdown outside the engine.
#
# The header is engine-free on purpose: it is the same file the relay announce includes, so this is a
# test of the shipped rule and not of a copy of it.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
INCLUDE="$HERE/../GeneralsMD/Code/GameEngine/Include"

g++ -std=c++17 -Wall -Wextra -I "$INCLUDE" -o /tmp/countdown_mirror_test "$HERE/countdown_mirror_test.cpp"
/tmp/countdown_mirror_test
