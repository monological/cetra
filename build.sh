#!/bin/bash
set -e

# Thin wrapper over CMakePresets.json. The presets own the generator, build type
# and binary directory -- this script only translates the flags people already
# have in their fingers into a preset name.
#
# --target dispatches a build to a remote VM: from macOS, `--target linux` and
# `--target windows` sync the working tree to the Linux / Windows build VM and
# build there. Addresses live in ~/.ssh/config (aliases cetra-linux / cetra-win),
# not here. See specs/8.6-remote-build-orchestration.md.
#
# On Windows directly, use the presets from an x64 Native Tools prompt:
#   cmake --workflow --preset windows-debug

usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -t, --target <os>  macos|linux|windows (default: this host). linux/windows"
    echo "                     from macOS build on the corresponding remote VM."
    echo "  -r, --release      Build in Release mode"
    echo "  -c, --clean        Clean build directory first"
    echo "  --no-joltc         Disable JoltC physics library"
    echo "  -h, --help         Show this help"
    exit 0
}

CLEAN=0
JOLTC=ON
BUILD_TYPE="debug"
TARGET=""

while [[ $# -gt 0 ]]; do
    case $1 in
        -t|--target) TARGET="$2"; shift 2 ;;
        -r|--release) BUILD_TYPE="release"; shift ;;
        -c|--clean) CLEAN=1; shift ;;
        --no-joltc) JOLTC=OFF; shift ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

case "$(uname -s)" in
    Darwin) HOST_PLATFORM="macos" ;;
    Linux)  HOST_PLATFORM="linux" ;;
    *)      HOST_PLATFORM="unknown" ;;
esac

# Default target is the host platform -> an ordinary local build.
[[ -z "$TARGET" ]] && TARGET="$HOST_PLATFORM"

# SSH aliases; overridable so CI can point elsewhere. The addresses (and the
# jump through the KVM host) live in ~/.ssh/config, never in the repo.
CETRA_LINUX_SSH="${CETRA_LINUX_SSH:-cetra-linux}"
CETRA_WIN_SSH="${CETRA_WIN_SSH:-cetra-win}"

# ---- Remote dispatch (target differs from this host) ----
if [[ "$TARGET" != "$HOST_PLATFORM" ]]; then
    case "$TARGET" in
        linux)
            # rsync: delta transfer + --delete, both ends have rsync.
            echo "Syncing working tree -> $CETRA_LINUX_SSH:~/cetra ..."
            rsync -az --delete -e ssh \
                --exclude='/out' --exclude='/.git' --exclude='/my_models' \
                --exclude='.DS_Store' --exclude='._*' \
                ./ "$CETRA_LINUX_SSH:cetra/"
            RFLAGS=""
            [[ "$BUILD_TYPE" == "release" ]] && RFLAGS="$RFLAGS -r"
            [[ $CLEAN -eq 1 ]] && RFLAGS="$RFLAGS -c"
            [[ "$JOLTC" == "OFF" ]] && RFLAGS="$RFLAGS --no-joltc"
            echo "Building on $CETRA_LINUX_SSH (linux-$BUILD_TYPE)..."
            exec ssh "$CETRA_LINUX_SSH" "cd ~/cetra && ./build.sh$RFLAGS"
            ;;
        windows)
            # Windows has no rsync; a tar archive over scp preserves mtimes on
            # extract, so rebuilds stay incremental (only the transfer is full).
            DEST="C:/Users/dev/cetra"
            TB="${TMPDIR:-/tmp}/cetra-sync-$$.tgz"
            echo "Packing working tree..."
            tar czf "$TB" \
                --exclude='./out' --exclude='./.git' --exclude='./my_models' \
                --exclude='.DS_Store' --exclude='._*' .
            echo "Syncing -> $CETRA_WIN_SSH:$DEST ..."
            scp -q "$TB" "$CETRA_WIN_SSH:C:/Users/dev/cetra-sync.tgz"
            rm -f "$TB"
            ssh "$CETRA_WIN_SSH" "New-Item -ItemType Directory -Force $DEST | Out-Null; tar xzf C:/Users/dev/cetra-sync.tgz -C $DEST"
            PFLAGS=""
            [[ "$BUILD_TYPE" == "release" ]] && PFLAGS="$PFLAGS -Release"
            [[ $CLEAN -eq 1 ]] && PFLAGS="$PFLAGS -Clean"
            [[ "$JOLTC" == "OFF" ]] && PFLAGS="$PFLAGS -NoJoltc"
            echo "Building on $CETRA_WIN_SSH (windows-$BUILD_TYPE)..."
            exec ssh "$CETRA_WIN_SSH" "powershell -ExecutionPolicy Bypass -File $DEST/tools/build.ps1$PFLAGS"
            ;;
        macos)
            echo "No remote macOS build host configured -- build macOS locally on a Mac."; exit 1 ;;
        *)
            echo "Unknown target: $TARGET (expected macos|linux|windows)"; exit 1 ;;
    esac
fi

# ---- Local build (host == target) ----
if [[ "$HOST_PLATFORM" == "unknown" ]]; then
    echo "Unsupported host: $(uname -s). On Windows use: cmake --workflow --preset windows-debug"; exit 1
fi

PRESET="${TARGET}-${BUILD_TYPE}"

# --fresh wipes the cache and reconfigures from scratch, so the script never has
# to know where the preset builds. (CMake >= 3.24; the presets require 3.25.)
FRESH=""
if [[ $CLEAN -eq 1 ]]; then
    FRESH="--fresh"
fi

# Initialize submodules if needed
if [[ ! -f cetra/src/ext/JoltC/CMakeLists.txt ]]; then
    echo "Initializing submodules..."
    git submodule update --init --recursive
fi

echo "Configuring ($PRESET)..."
cmake --preset "$PRESET" $FRESH -DCETRA_BUILD_JOLTC="$JOLTC"

echo "Building..."
cmake --build --preset "$PRESET"

echo "Done ($PRESET)."
