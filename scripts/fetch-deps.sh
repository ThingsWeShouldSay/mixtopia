#!/usr/bin/env bash
# Downloads the third-party source code Mixtopia builds against into external/.
# Versions are pinned so every build uses exactly the same code.
# Safe to re-run: anything already present is left alone.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$ROOT/external"
mkdir -p "$EXT"

TRACKTION_COMMIT=38d5a2750367d8a95a58da6d3bbab49823285f24   # Tracktion Engine 3.5.0 (develop, 2026-10-14)
JUCE_COMMIT=37c894f83d379179b2070d437ccd0f1cd9af9576   # JUCE 8.0.13, pinned by Tracktion
RUBBERBAND_COMMIT=1d95888bec3ae0a17c0c4af791810d5a63f6bc35   # Rubber Band v4.0.0 (the commit that tag gave; pinned by hash 2026-11-05)
LIBKEYFINDER_COMMIT=941e517ebf853c2153a8b9d6efcc0c729199aa0b   # libKeyFinder 2.2.8 + 10 (the version that won the key bake-off)

# check_commit <dir> <full commit hash>: stops if a checkout isn't exactly the pinned code.
check_commit() {
    local have
    have="$(git -C "$1" rev-parse HEAD)"
    if [ "$have" != "$2" ]; then
        echo "$(basename "$1") is at $have, not the pinned $2 - stopping" >&2
        exit 1
    fi
}

# fetch_commit <dir> <url> <full commit hash>
fetch_commit() {
    local dir="$1" url="$2" ref="$3"
    if [ -d "$dir/.git" ]; then
        echo "already have $(basename "$dir")"
        check_commit "$dir" "$ref"
        return
    fi
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$ref"
    git -C "$dir" checkout -q FETCH_HEAD
    check_commit "$dir" "$ref"
    echo "fetched $(basename "$dir") @ $ref"
}

if [ ! -d "$EXT/tracktion_engine/.git" ]; then
    git clone -q https://github.com/Tracktion/tracktion_engine.git "$EXT/tracktion_engine"
    git -C "$EXT/tracktion_engine" checkout -q "$TRACKTION_COMMIT"
    echo "fetched tracktion_engine @ $TRACKTION_COMMIT"
else
    echo "already have tracktion_engine"
fi
check_commit "$EXT/tracktion_engine" "$TRACKTION_COMMIT"

# Tracktion's .gitmodules uses an SSH URL; fetch the pinned JUCE commit over HTTPS instead.
fetch_commit "$EXT/tracktion_engine/modules/juce" https://github.com/juce-framework/JUCE.git "$JUCE_COMMIT"
fetch_commit "$EXT/rubberband" https://github.com/breakfastquay/rubberband.git "$RUBBERBAND_COMMIT"
fetch_commit "$EXT/libkeyfinder" https://github.com/mixxxdj/libkeyfinder.git "$LIBKEYFINDER_COMMIT"

# Inter (SIL Open Font License 1.1): the app's font, bundled so it looks the same on every desktop
# (user's choice 2026-10-21: easier to read at small sizes than the Arial look-alike JUCE picked).
INTER_VERSION=4.1
INTER_SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
if [ ! -f "$EXT/inter/Inter-Regular.ttf" ]; then
    mkdir -p "$EXT/inter"
    curl -sSL -o "$EXT/inter/Inter-$INTER_VERSION.zip" "https://github.com/rsms/inter/releases/download/v$INTER_VERSION/Inter-$INTER_VERSION.zip"
    echo "$INTER_SHA256  $EXT/inter/Inter-$INTER_VERSION.zip" | sha256sum -c --quiet -
    unzip -q -j -o "$EXT/inter/Inter-$INTER_VERSION.zip" extras/ttf/Inter-Regular.ttf extras/ttf/Inter-Bold.ttf LICENSE.txt -d "$EXT/inter"
    rm "$EXT/inter/Inter-$INTER_VERSION.zip"
    echo "fetched Inter $INTER_VERSION"
else
    echo "already have Inter"
fi

# Local fixes to third-party code (see patches/). Each is applied once; re-runs skip it.
apply_patch() {
    local dir="$1" patch="$2"
    if git -C "$dir" apply --reverse --check "$patch" 2>/dev/null; then
        echo "already patched: $(basename "$patch")"
    else
        git -C "$dir" apply "$patch"
        echo "applied: $(basename "$patch")"
    fi
}

apply_patch "$EXT/tracktion_engine/modules/juce" "$ROOT/patches/juce-xdnd-wayland.patch"
apply_patch "$EXT/tracktion_engine/modules/juce" "$ROOT/patches/juce-submenu-hover.patch"
apply_patch "$EXT/tracktion_engine" "$ROOT/patches/tracktion-timestretch.patch"
apply_patch "$EXT/tracktion_engine" "$ROOT/patches/tracktion-direct-reader.patch"
apply_patch "$EXT/tracktion_engine" "$ROOT/patches/tracktion-keep-readers.patch"
