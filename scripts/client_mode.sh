#!/usr/bin/env bash
# client_mode.sh — switch wlan0 back to home WiFi client (NetworkManager)
# Tested on Debian 13 (Trixie), Raspberry Pi Zero 2 W
set -euo pipefail

IFACE="wlan0"

err() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || err "Run as root: sudo $0"

command -v nmcli >/dev/null || err "nmcli not found"

echo "=== Switching to client mode ==="

# 1. Stop AP services
systemctl stop hostapd dnsmasq 2>/dev/null || true

# 2. Flush static IP
ip addr flush dev "$IFACE" 2>/dev/null || true

# 3. Return wlan0 to NetworkManager — it will restart wpa_supplicant
#    and connect to the highest-priority saved network automatically
nmcli device set "$IFACE" managed yes
nmcli device connect "$IFACE" || true

echo ""
echo "=== Client mode active ==="
echo "Waiting for IP assignment..."
sleep 4

IP=$(ip -4 addr show "$IFACE" 2>/dev/null | grep -oP '(?<=inet )\S+' || echo "none")
echo "  Interface : $IFACE"
echo "  Address   : $IP"
echo ""
echo "SSH: ssh vladimir@$(echo "$IP" | cut -d/ -f1)"
