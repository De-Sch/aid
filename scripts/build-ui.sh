#!/usr/bin/env bash
# Build the SvelteKit dashboard into ui/build/ (static SPA).
#
# Deliberately DECOUPLED from the C++ CMake build: Node is a build-time-only
# dependency; prod ships the static bundle, not Node. Run this by hand (or in
# CI) when the UI changes — never wired as a CMake custom target.
#
# After building, point the daemon's Ui.documentRoot at the ABSOLUTE path of
# ui/build/, e.g. in config.json:
#   "Ui": { "projectWebBaseUrl": "...", "documentRoot": "/abs/.../AID2.0/ui/build" }
#
# Dev caveat: the session cookie is Secure by default. For plain-http LAN/dev,
# set Auth.cookieSecure=false (localhost is already a secure context, so the
# Vite-proxy dev flow needs no change).
set -euo pipefail

cd "$(dirname "$0")/.."

pnpm --prefix ui build

echo "UI built -> $(pwd)/ui/build  (point Ui.documentRoot at this absolute path)"
