#!/bin/bash
# Deploy binary to Pi Zero 2 W and run.
#
# Usage:
#   deploy_run.sh [debug|release]           # foreground — output in this terminal
#   deploy_run.sh [debug|release] --detach  # background — tail log, survives mode switch

set -euo pipefail

BUILD_TYPE=${1:-debug}
DETACH=false
if [[ "${2:-}" == "--detach" ]]; then DETACH=true; fi

PI_HOST="vladimir@raspizero2"
BINARY="build/${BUILD_TYPE}/pizero2cam_app"
REMOTE_DIR="~/apps"
REMOTE_BIN="${REMOTE_DIR}/pizero2cam_app"
REMOTE_LOG="/tmp/cam.log"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

echo -e "${YELLOW}=== Deploy and Run (${BUILD_TYPE}${DETACH:+, detached}) ===${NC}"

# ── Verify binary ─────────────────────────────────────────────────────────────
if [ ! -f "$BINARY" ]; then
    echo -e "${RED}ERROR: Binary not found: $BINARY${NC}"; exit 1
fi
if ! file "$BINARY" | grep -q "ARM aarch64"; then
    echo -e "${RED}ERROR: Binary is not ARM64!${NC}"; file "$BINARY"; exit 1
fi
echo -e "${GREEN}✓ Binary verified: ARM64  ($(du -h "$BINARY" | cut -f1))${NC}"

# ── Deploy ────────────────────────────────────────────────────────────────────
ssh "$PI_HOST" "mkdir -p $REMOTE_DIR"
rsync -avz --progress "$BINARY" "$PI_HOST:$REMOTE_DIR/"
echo -e "${GREEN}✓ Deployed${NC}"

# ── Stop any running instance ─────────────────────────────────────────────────
ssh "$PI_HOST" "pkill -x pizero2cam_app 2>/dev/null; sleep 1; true"

# ── Run ───────────────────────────────────────────────────────────────────────
echo ""
echo "────────────────────────────────"

if $DETACH; then
    # Background: nohup keeps the app alive after SSH disconnects.
    # SIGHUP is also ignored in main() — double protection.
    # Tail the log so output is still visible here; Ctrl-C stops the tail
    # but does NOT kill the app on the Pi.
    ssh "$PI_HOST" "nohup $REMOTE_BIN >$REMOTE_LOG 2>&1 &"
    echo -e "${GREEN}✓ App started in background (PID on Pi: $(ssh "$PI_HOST" pgrep -x pizero2cam_app))${NC}"
    echo -e "  Log : $REMOTE_LOG"
    echo -e "  Stop: ssh $PI_HOST 'pkill -x pizero2cam_app'"
    echo ""
    echo -e "${YELLOW}Tailing log — Ctrl-C stops the tail, app keeps running:${NC}"
    echo "────────────────────────────────"
    ssh "$PI_HOST" "tail -f $REMOTE_LOG"
else
    # Foreground: output flows directly to this terminal.
    # Ctrl-C here sends SIGINT to the remote app (clean shutdown).
    # If you switch network mode in another terminal the SSH session will
    # drop, but the app survives because SIGHUP is ignored in main().
    echo -e "${YELLOW}Running in foreground — Ctrl-C for clean shutdown:${NC}"
    ssh -t "$PI_HOST" "$REMOTE_BIN"
    EXIT_CODE=$?
    echo "────────────────────────────────"
    if [ $EXIT_CODE -eq 0 ]; then
        echo -e "${GREEN}✓ Exited cleanly${NC}"
    else
        echo -e "${RED}✗ Exited with code $EXIT_CODE${NC}"
    fi
fi
