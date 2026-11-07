#!/usr/bin/env bash
# Builds Mixtopia. Run inside the mixtopia-dev container.
# Usage: scripts/build.sh [Debug|Release]   (default: Release)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG="${1:-Release}"
BUILD="$ROOT/build/$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"

"$ROOT/scripts/fetch-deps.sh"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD" --parallel

echo
echo "Built: $BUILD/Mixtopia_artefacts/$CONFIG/Mixtopia"
