#!/usr/bin/env bash
# Run clang-format on every C++ source/header in the tree.
# Usage: ./scripts/format.sh [--check]
set -euo pipefail

cd "$(dirname "$0")/.."

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format not installed. Try: sudo apt install clang-format" >&2
    exit 1
fi

mode="${1:-fix}"
mapfile -d '' files < <(find src lib tests include -type f \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) -print0)

if [[ "$mode" == "--check" ]]; then
    clang-format --dry-run --Werror "${files[@]}"
else
    clang-format -i "${files[@]}"
fi
