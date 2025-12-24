#!/bin/bash
set -e

BUILD_DIR="build"
BUILD_TYPE="Debug"
GENERATOR="Ninja"

usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -r, --release    Build in Release mode"
    echo "  -c, --clean      Clean build directory first"
    echo "  -j, --joltc      Enable JoltC physics library"
    echo "  -h, --help       Show this help"
    exit 0
}

CLEAN=0
JOLTC=OFF

while [[ $# -gt 0 ]]; do
    case $1 in
        -r|--release) BUILD_TYPE="Release"; shift ;;
        -c|--clean) CLEAN=1; shift ;;
        -j|--joltc) JOLTC=ON; shift ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

if [[ $CLEAN -eq 1 ]]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# Initialize submodules if needed
if [[ ! -f cetra/src/ext/JoltC/CMakeLists.txt ]]; then
    echo "Initializing submodules..."
    git submodule update --init --recursive
fi

echo "Configuring ($BUILD_TYPE)..."
cmake -G "$GENERATOR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCETRA_BUILD_JOLTC="$JOLTC"

echo "Building..."
ninja -C "$BUILD_DIR"

echo "Done. Outputs in $BUILD_DIR/bin/"
