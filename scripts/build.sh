#!/usr/bin/env bash
set -euo pipefail
MCONNECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MCONNECT_BUILD_DIR="${MCONNECT_BUILD_DIR:-$MCONNECT_ROOT/build}"
python3 "$MCONNECT_ROOT/scripts/bootstrap.py"
MCONNECT_ARGS=(-DCMAKE_BUILD_TYPE=Release)
if [[ "$(uname -s)" == Darwin ]]; then
    MCONNECT_ARGS+=('-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' '-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0')
fi
cmake -S "$MCONNECT_ROOT" -B "$MCONNECT_BUILD_DIR" "${MCONNECT_ARGS[@]}" "$@"
cmake --build "$MCONNECT_BUILD_DIR" --config Release --parallel "${MCONNECT_JOBS:-6}"
ctest --test-dir "$MCONNECT_BUILD_DIR" -C Release --output-on-failure
