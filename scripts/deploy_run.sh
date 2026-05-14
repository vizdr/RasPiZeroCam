#!/bin/bash
# Deploy binary to Pi Zero 2 W and run

BUILD_TYPE=${1:-debug}
PI_HOST="vladimir@raspizero2"
BINARY="build/${BUILD_TYPE}/pizero2cam_app"
REMOTE_DIR="~/apps"
REMOTE_BIN="${REMOTE_DIR}/pizero2cam_app"

RED='\033[0;31m'; GREEN='\033[0;32m'
YELLOW='\033[1;33m'; NC='\033[0m'

echo -e "${YELLOW}=== Deploy and Run ===${NC}"

# Check binary exists
if [ ! -f "$BINARY" ]; then
    echo -e "${RED}ERROR: Binary not found: $BINARY${NC}"
    exit 1
fi

# Verify ARM64
ARCH=$(file "$BINARY" | grep -o "ARM aarch64")
if [ -z "$ARCH" ]; then
    echo -e "${RED}ERROR: Binary is not ARM64!${NC}"
    file "$BINARY"
    exit 1
fi
echo -e "${GREEN}✓ Binary verified: ARM64${NC}"
echo "Size: $(du -h "$BINARY" | cut -f1)"

# Create remote dir and deploy
ssh $PI_HOST "mkdir -p $REMOTE_DIR"
rsync -avz --progress "$BINARY" "$PI_HOST:$REMOTE_DIR/"

if [ $? -ne 0 ]; then
    echo -e "${RED}ERROR: rsync failed${NC}"; exit 1
fi
echo -e "${GREEN}✓ Deployed successfully${NC}"

# Run on Pi
echo ""
echo -e "${YELLOW}Running on Pi Zero 2 W...${NC}"
echo "────────────────────────────────────"
ssh -t $PI_HOST "$REMOTE_BIN ${@:2}"
EXIT_CODE=$?
echo "────────────────────────────────────"

if [ $EXIT_CODE -eq 0 ]; then
    echo -e "${GREEN}✓ Program completed successfully${NC}"
else
    echo -e "${RED}✗ Program exited with error (code: $EXIT_CODE)${NC}"
fi