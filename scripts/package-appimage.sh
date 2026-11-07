#!/usr/bin/env bash
# Builds a Release version of Mixtopia and packages it as dist/Mixtopia-x86_64.AppImage.
# Run inside the mixtopia-dev container. Downloads linuxdeploy into tools/ the first time
# (build-only; never needed by the finished AppImage).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/tools"
APPDIR="$ROOT/build/AppDir"
BIN="$ROOT/build/release/Mixtopia_artefacts/Release/Mixtopia"
LINUXDEPLOY="$TOOLS/linuxdeploy-x86_64.AppImage"

# The container has no FUSE, so let AppImage tools unpack themselves instead of mounting.
export APPIMAGE_EXTRACT_AND_RUN=1

"$ROOT/scripts/build.sh" Release

# linuxdeploy is pinned by its SHA-256: build 369 (git commit 07333c6, built 2026-10-01), the copy every Mixtopia
# AppImage so far was made with. A different file - e.g. a newer "continuous" build - stops the packaging.
# To use a different linuxdeploy on purpose, set LINUXDEPLOY_SHA256 to that file's SHA-256 when running the script.
LINUXDEPLOY_SHA256=${LINUXDEPLOY_SHA256:-8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1}
mkdir -p "$TOOLS"
if [ ! -x "$LINUXDEPLOY" ]; then
    wget -q -O "$LINUXDEPLOY" \
        https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
    chmod +x "$LINUXDEPLOY"
fi
if ! echo "$LINUXDEPLOY_SHA256  $LINUXDEPLOY" | sha256sum -c --quiet - ; then
    echo "$LINUXDEPLOY is not the pinned linuxdeploy (SHA-256 $LINUXDEPLOY_SHA256) - stopping" >&2
    exit 1
fi

rm -rf "$APPDIR"
mkdir -p "$ROOT/dist"

# Licences travel with the program (usr/share/doc/mixtopia): Mixtopia's own (AGPL-3.0) and NOTICE, then the licence of
# every piece of third-party code compiled into it (see NOTICE). The shared libraries linuxdeploy bundles bring their
# own copyright files (usr/share/doc/<package>/copyright).
DOC="$APPDIR/usr/share/doc/mixtopia"
mkdir -p "$DOC"
cp "$ROOT/LICENSE" "$DOC/LICENSE"
cp "$ROOT/NOTICE" "$DOC/NOTICE"
EXT="$ROOT/external"
for pair in \
    "inter/LICENSE.txt:Inter-font-LICENSE.txt" \
    "tracktion_engine/modules/juce/LICENSE.md:JUCE-LICENSE.md" \
    "tracktion_engine/LICENSE.md:Tracktion-Engine-LICENSE.md" \
    "rubberband/COPYING:RubberBand-COPYING.txt" \
    "libkeyfinder/LICENSE:libKeyFinder-LICENSE.txt" \
    "tracktion_engine/modules/tracktion_engine/3rd_party/signalsmith-stretch/LICENSE.txt:Signalsmith-Stretch-LICENSE.txt" \
    "tracktion_engine/modules/tracktion_engine/3rd_party/signalsmith-stretch/signalsmith-linear/LICENSE.txt:Signalsmith-Linear-LICENSE.txt" \
    "tracktion_engine/modules/3rd_party/choc/LICENSE.md:choc-LICENSE.md" \
    "tracktion_engine/modules/3rd_party/crill/LICENSE.txt:crill-LICENSE.txt" \
    "tracktion_engine/modules/tracktion_graph/3rd_party/farbot/LICENSE:farbot-LICENSE.txt"; do
    cp "$EXT/${pair%%:*}" "$DOC/${pair##*:}"
done
# SoundTouch (LGPL-2.1) has no licence file of its own in Tracktion's copy: the licence's full text from the system.
cp /usr/share/common-licenses/LGPL-2.1 "$DOC/SoundTouch-LGPL-2.1.txt"

# The AppImage runtime (the small starter at the front of every AppImage): tools/runtime-x86_64 if it's there, so
# packaging needs no internet and every build gets the same runtime; without it linuxdeploy downloads the current
# one from GitHub (a different file over time, so the AppImage's checksum then differs too).
if [ -f "$TOOLS/runtime-x86_64" ]; then
    export LDAI_RUNTIME_FILE="$TOOLS/runtime-x86_64"
fi

# libmp3lame is loaded at runtime (dlopen) for MP3 export, so linuxdeploy can't see it -
# name it explicitly.
(cd "$ROOT/dist" && "$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --executable "$BIN" \
    --library /usr/lib/x86_64-linux-gnu/libmp3lame.so.0 \
    --desktop-file "$ROOT/packaging/mixtopia.desktop" \
    --icon-file "$ROOT/packaging/mixtopia.svg" \
    --output appimage)

mv -f "$ROOT"/dist/Mixtopia*.AppImage "$ROOT/dist/Mixtopia-x86_64.AppImage" 2>/dev/null || true
echo
echo "AppImage: $ROOT/dist/Mixtopia-x86_64.AppImage"
echo "Bundled libraries:"
ls "$APPDIR/usr/lib"
