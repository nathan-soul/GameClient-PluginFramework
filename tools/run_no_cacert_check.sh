#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

hits=$({ grep -rn "cacert.pem" GeneralsMD/Code/GameEngine/Source GeneralsMD/Code/GameEngine/Include 2>/dev/null || true; } | wc -l | tr -d ' ')

if [ "$hits" != "0" ]; then
  echo "FAIL: $hits references to cacert.pem in the client sources"
  grep -rn "cacert.pem" GeneralsMD/Code/GameEngine/Source GeneralsMD/Code/GameEngine/Include
  exit 1
fi

echo "PASS: no cacert.pem in the client sources"
