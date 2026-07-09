#!/usr/bin/env bash
# Run the GoogleTest suite, building first if needed.
# Usage: ./scripts/test.sh [ctest args]
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ ! -d build ]]; then
    ./scripts/build.sh
else
    cmake --build build -j
fi

ctest --test-dir build --output-on-failure "$@"
