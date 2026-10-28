#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
g++ -std=c++17 -I GeneralsMD/Code/GameEngine/Include \
  tools/observer_notice_test.cpp \
  GeneralsMD/Code/GameEngine/Source/Common/GencastObserverNotice.cpp \
  -o /tmp/observer_notice_test
/tmp/observer_notice_test
