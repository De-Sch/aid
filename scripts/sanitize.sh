#!/usr/bin/env bash
# Build with ASan + UBSan in build-asan/ and run the tests.
# Usage: ./scripts/sanitize.sh
set -euo pipefail

cd "$(dirname "$0")/.."

cmake -S . -B build-asan -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DAID_SANITIZE=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build-asan -j

# Sanitizer ENV is set per-test via CMake's ENVIRONMENT property
# (tests/CMakeLists.txt) — not exported globally here, because doing so
# preloads libasan into ctest itself which then crashes on glibc.
ctest --test-dir build-asan --output-on-failure
