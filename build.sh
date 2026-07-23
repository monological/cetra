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

# Paths kept out of the working-tree sync: build output, VCS, the big runtime
# asset dir, and assimp's 200MB+ vendored test-model corpus (the build never
# uses it -- ASSIMP_BUILD_TESTS is OFF).
SYNC_EXCLUDE_DIRS=(out .git my_models cetra/src/ext/assimp/test)

# This invocation's build knobs as a flag string in the target's own spelling
# ($1 release, $2 clean, $3 no-joltc) -- so the two remote arms don't each
# re-derive the same three predicates.
remote_flags() {
    local f=""
    [[ "$BUILD_TYPE" == "release" ]] && f+=" $1"
    [[ $CLEAN -eq 1 ]] && f+=" $2"
    [[ "$JOLTC" == "OFF" ]] && f+=" $3"
    printf '%s' "$f"
}

# ---- Remote dispatch (target differs from this host) ----
if [[ "$TARGET" != "$HOST_PLATFORM" ]]; then
    case "$TARGET" in
        linux)
            # rsync: delta transfer + --delete, both ends have rsync.
            EX=(); for d in "${SYNC_EXCLUDE_DIRS[@]}"; do EX+=(--exclude="/$d"); done
            echo "Syncing working tree -> $CETRA_LINUX_SSH:~/cetra ..."
            rsync -az --delete -e ssh "${EX[@]}" --exclude='.DS_Store' --exclude='._*' \
                ./ "$CETRA_LINUX_SSH:cetra/"
            echo "Building on $CETRA_LINUX_SSH (linux-$BUILD_TYPE)..."
            exec ssh "$CETRA_LINUX_SSH" "cd ~/cetra && ./build.sh$(remote_flags -r -c --no-joltc)"
            ;;
        windows)
            # Windows has no rsync; a tar archive over scp preserves mtimes on
            # extract, so rebuilds stay incremental (only the transfer is full).
            DEST="C:/Users/dev/cetra"
            WIN_TGZ="C:/Users/dev/cetra-sync.tgz"
            TB="${TMPDIR:-/tmp}/cetra-sync-$$.tgz"
            # COPYFILE_DISABLE stops macOS bsdtar emitting AppleDouble (._*) entries
            # from xattrs; the nested excludes drop any on-disk macOS cruft too
            # (CMake's source glob would otherwise try to compile ._*.c on Windows).
            EX=(); for d in "${SYNC_EXCLUDE_DIRS[@]}"; do EX+=(--exclude="./$d"); done
            echo "Packing working tree..."
            COPYFILE_DISABLE=1 tar czf "$TB" "${EX[@]}" \
                --exclude='.DS_Store' --exclude='*/.DS_Store' \
                --exclude='._*' --exclude='*/._*' .
            echo "Syncing -> $CETRA_WIN_SSH:$DEST ..."
            scp -q "$TB" "$CETRA_WIN_SSH:$WIN_TGZ"
            rm -f "$TB"
            # tar has no --delete, so clear the previous source first (keeping out/
            # for the build cache) or files removed upstream would linger, then
            # extract. tar restores the archived mtimes, so rebuilds stay incremental.
            ssh "$CETRA_WIN_SSH" "if (Test-Path '$DEST') { Get-ChildItem -Path '$DEST/*' -Force -Exclude out | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue }; New-Item -ItemType Directory -Force '$DEST' | Out-Null; tar xzf $WIN_TGZ -C '$DEST'"
            echo "Building on $CETRA_WIN_SSH (windows-$BUILD_TYPE)..."
            exec ssh "$CETRA_WIN_SSH" "powershell -ExecutionPolicy Bypass -File $DEST/tools/build.ps1$(remote_flags -Release -Clean -NoJoltc)"
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
