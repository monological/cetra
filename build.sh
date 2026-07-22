#!/bin/bash
set -e

# Thin wrapper over CMakePresets.json. The presets own the generator, build type
# and binary directory -- this script only translates the flags people already
# have in their fingers into a preset name.
#
# On Windows use the presets directly, from an x64 Native Tools prompt:
#   cmake --workflow --preset windows-debug

usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -r, --release    Build in Release mode"
    echo "  -c, --clean      Clean build directory first"
    echo "  --no-joltc       Disable JoltC physics library"
    echo "  -h, --help       Show this help"
    exit 0
}

CLEAN=0
JOLTC=ON
BUILD_TYPE="debug"

while [[ $# -gt 0 ]]; do
    case $1 in
        -r|--release) BUILD_TYPE="release"; shift ;;
        -c|--clean) CLEAN=1; shift ;;
        --no-joltc) JOLTC=OFF; shift ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

case "$(uname -s)" in
    Darwin) PLATFORM="macos" ;;
    Linux)  PLATFORM="linux" ;;
    *)      echo "Unsupported host: $(uname -s). On Windows use: cmake --workflow --preset windows-debug"; exit 1 ;;
esac

PRESET="${PLATFORM}-${BUILD_TYPE}"

# Mirrors the binaryDir in CMakePresets.json; keep the two in step.
if [[ "$BUILD_TYPE" == "release" ]]; then
    BUILD_DIR="out/release"
else
    BUILD_DIR="out"
fi

if [[ $CLEAN -eq 1 ]]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# Initialize submodules if needed
if [[ ! -f cetra/src/ext/JoltC/CMakeLists.txt ]]; then
    echo "Initializing submodules..."
    git submodule update --init --recursive
fi

echo "Configuring ($PRESET)..."
cmake --preset "$PRESET" -DCETRA_BUILD_JOLTC="$JOLTC"

echo "Building..."
cmake --build --preset "$PRESET"

echo "Done. Outputs in $BUILD_DIR/bin/"
