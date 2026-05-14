#!/bin/bash
# Deploy binary and start gdbserver on Pi

PI_HOST="vladimir@raspizero2"
BUILD_TYPE="debug"
BINARY="build/${BUILD_TYPE}/pizero2cam_app"
REMOTE_DIR="~/apps"
REMOTE_BIN="${REMOTE_DIR}/pizero2cam_app"
GDB_PORT=2345

echo "=== GDB Server Setup ==="

# Deploy latest binary
echo "Deploying binary..."
rsync -az "$BINARY" "$PI_HOST:$REMOTE_DIR/"

# Kill any existing gdbserver
ssh $PI_HOST "pkill gdbserver 2>/dev/null; sleep 0.5"

# Start gdbserver on Pi
echo "Starting gdbserver on port $GDB_PORT..."
ssh $PI_HOST "gdbserver :${GDB_PORT} ${REMOTE_BIN}" &

sleep 2
echo "Listening on port ${GDB_PORT}"

wait