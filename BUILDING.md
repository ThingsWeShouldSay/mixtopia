# Building Mixtopia from source

Mixtopia is built on **Ubuntu 24.04** (GCC 13, CMake, Ninja). The resulting program runs on any x86_64 Linux with
glibc 2.38 or newer. If your system isn't Ubuntu 24.04, build in an Ubuntu 24.04 container - with Distrobox, for
example:

```bash
distrobox create --name mixtopia-dev --image docker.io/library/ubuntu:24.04
distrobox enter mixtopia-dev
```

Run the commands below inside it, from the folder where you cloned this repository.

## 1. Install the build tools and libraries

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git pkg-config wget file \
    libasound2-dev libjack-jackd2-dev libcurl4-openssl-dev \
    libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev \
    libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libgl1-mesa-dev \
    libmpg123-dev libmp3lame-dev libfftw3-dev \
    pipewire-alsa alsa-utils python3-numpy lame
```

(`python3-numpy` and `lame` are only needed to make the test sound for the self-tests in step 3.)

## 2. Fetch the pinned third-party sources (needs internet)

```bash
scripts/fetch-deps.sh
```

This downloads into `external/` exactly the code Mixtopia is built with, each pinned by its full git commit and
checked (the script stops if anything differs): Tracktion Engine 3.5.0, JUCE 8.0.13, Rubber Band v4.0.0, libKeyFinder,
and the Inter 4.1 font (checksum-checked). Then it applies the five fixes in `patches/`: JUCE drag-and-drop under
Wayland, JUCE submenu hover, Tracktion time-stretch timing, Tracktion direct reader (looped songs) and Tracktion
keep-readers (cutting songs while playing). Running it again is safe.

## 3. Build and test

```bash
scripts/build.sh Release
```

The program is `build/release/Mixtopia_artefacts/Release/Mixtopia`. Release builds contain no paths from your machine
(source and build folders are written as `./...` and `build/...`) and are stripped.

The self-tests need a 10-second test MP3:

```bash
mkdir -p build/test-audio
python3 scripts/analyze-tempo.py make-tone build/test-audio/tone.wav 440 10
lame --quiet -b 320 build/test-audio/tone.wav build/test-audio/test.mp3
build/release/Mixtopia_artefacts/Release/Mixtopia --self-test build/test-audio/test.mp3
```

Every line should say PASS.

## 4. Package the AppImage

```bash
scripts/package-appimage.sh
```

Result: `dist/Mixtopia-x86_64.AppImage`, with the licences of Mixtopia and of everything compiled into it in
`usr/share/doc/mixtopia/` (see NOTICE). Two things to know:

- **linuxdeploy** (the packaging tool) is pinned by its SHA-256. If `tools/linuxdeploy-x86_64.AppImage` is missing,
  the script downloads linuxdeploy's current "continuous" build - which is no longer the pinned file, so the checksum
  check then stops the packaging. To use the linuxdeploy you have on purpose, run the script with its checksum:
  `LINUXDEPLOY_SHA256=$(sha256sum tools/linuxdeploy-x86_64.AppImage | cut -d' ' -f1) scripts/package-appimage.sh`.
- **The AppImage runtime** (the small starter at the front of every AppImage): if `tools/runtime-x86_64` exists, it
  is used, and packaging needs no internet. Otherwise linuxdeploy downloads the current runtime from GitHub - so
  packaging needs internet, and AppImages built at different times differ in checksum even from identical code.

Run the AppImage outside the container, on your normal desktop.
