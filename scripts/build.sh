#!/usr/bin/env bash
# Build AID2.0 in the default debug build tree.
# Usage: ./scripts/build.sh [extra cmake args]
set -euo pipefail

cd "$(dirname "$0")/.."

cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "$@"

cmake --build build -j

# Symlink compile_commands.json to project root so clangd / clang-tidy find it.
ln -sf build/compile_commands.json compile_commands.json
