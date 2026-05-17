#!/bin/bash
# Full pipeline: configure → build → deploy → run

set -e

# Resolve paths from the project root (directory above scripts/)
# so the script works correctly regardless of the caller's working directory.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

TOOLCHAIN="$PROJECT_DIR/aarch64-pi-zero2.cmake"
BUILD_DIR="$PROJECT_DIR/build/debug"
PI_HOST="vladimir@raspizero2"

echo "=== Build + Deploy Pipeline ==="
date

# Guard: if the cached compiler does not match the toolchain's compiler,
# the cache must be deleted — CMake will silently ignore the toolchain file
# and keep the stale compiler otherwise.
#
# CMake stores the compiler in CMakeFiles/<ver>/CMakeCXXCompiler.cmake,
# not in CMakeCache.txt directly (when set via toolchain file).
EXPECTED_COMPILER="/usr/bin/aarch64-linux-gnu-g++"
COMPILER_CMAKE=$(find "$BUILD_DIR/CMakeFiles" -name "CMakeCXXCompiler.cmake" 2>/dev/null | head -1)
if [ -n "$COMPILER_CMAKE" ]; then
    CACHED_COMPILER=$(grep 'set(CMAKE_CXX_COMPILER ' "$COMPILER_CMAKE" \
                      | sed 's/.*"\(.*\)".*/\1/')
    if [ "$CACHED_COMPILER" != "$EXPECTED_COMPILER" ]; then
        echo "!! Cached compiler ($CACHED_COMPILER) != expected ($EXPECTED_COMPILER)"
        echo "   Deleting stale build directory..."
        rm -rf "$BUILD_DIR"
    fi
fi

# Configure
echo "── Configure ──"
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Verify the compiler that was configured
COMPILER_CMAKE=$(find "$BUILD_DIR/CMakeFiles" -name "CMakeCXXCompiler.cmake" 2>/dev/null | head -1)
ACTUAL=$(grep 'set(CMAKE_CXX_COMPILER ' "$COMPILER_CMAKE" 2>/dev/null \
         | sed 's/.*"\(.*\)".*/\1/')
if [ "$ACTUAL" != "$EXPECTED_COMPILER" ]; then
    echo "ERROR: configure used wrong compiler: $ACTUAL"
    exit 1
fi
echo "Compiler: $ACTUAL ✓"

# Build
echo "── Build ──"
time cmake --build "$BUILD_DIR" --parallel $(nproc)

# Verify ARM64
echo "── Verify ──"
file "$BUILD_DIR/pizero2cam_app" | grep -q "ARM aarch64" \
    && echo "Binary: ARM aarch64 ✓" \
    || { echo "ERROR: binary is not ARM aarch64"; exit 1; }

# Deploy
echo "── Deploy ──"
ssh $PI_HOST "mkdir -p ~/apps"
rsync -avz --progress "$BUILD_DIR/pizero2cam_app" \
    "$PI_HOST:~/apps/"

# Run
echo "── Run on Pi Zero 2 W ──"
echo "────────────────────────────────"
ssh $PI_HOST "~/apps/pizero2cam_app"
echo "────────────────────────────────"
