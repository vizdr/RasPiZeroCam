#!/bin/bash
# Full pipeline: configure → build → deploy → run

set -e

TOOLCHAIN="aarch64-pi-zero2.cmake"
BUILD_DIR="build/debug"
PI_HOST="vladimir@raspizero2"

echo "=== Build + Deploy Pipeline ==="
date

# Configure
echo "── Configure ──"
cmake -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Build
echo "── Build ──"
time cmake --build "$BUILD_DIR" --parallel $(nproc)

# Verify ARM64
echo "── Verify ──"
file "$BUILD_DIR/pizero2cam_app"

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