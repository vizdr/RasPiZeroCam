#!/usr/bin/env bash
# ap_mode.sh — switch wlan0 from NetworkManager client to standalone AP
# Tested on Debian 13 (Trixie), Raspberry Pi Zero 2 W
# Requires: hostapd, dnsmasq, iw, rfkill  (see Phase 5 setup)
set -euo pipefail

AP_IP="192.168.4.1"
AP_PREFIX="24"
IFACE="wlan0"

err() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || err "Run as root: sudo $0"

command -v nmcli    >/dev/null || err "nmcli not found (install NetworkManager)"
command -v hostapd  >/dev/null || err "hostapd not found (sudo apt install hostapd)"
command -v dnsmasq  >/dev/null || err "dnsmasq not found (sudo apt install dnsmasq)"
command -v iw       >/dev/null || err "iw not found (sudo apt install iw)"

echo "=== Switching to AP mode ==="

# 1. Set wireless regulatory domain (DE) before bringing the interface up
#    Required for channel 6 to be usable at legal TX power
iw reg set DE

# 2. Disconnect and release wlan0 from NetworkManager
nmcli device disconnect "$IFACE" 2>/dev/null || true
nmcli device set "$IFACE" managed no

# 3. Stop wpa_supplicant on this interface so hostapd can own it
pkill -x wpa_supplicant 2>/dev/null || true
sleep 0.5

# 4. Unblock radio (rfkill may re-block after NM release)
rfkill unblock wifi
sleep 0.3

# 5. Bring interface up with static IP
ip link set "$IFACE" up
ip addr flush dev "$IFACE"
ip addr add "${AP_IP}/${AP_PREFIX}" dev "$IFACE"

# 6. Disable power save (prevents AP from dropping silent clients)
iw dev "$IFACE" set power_save off

# 7. Start hostapd (reads /etc/hostapd/hostapd.conf)
systemctl start hostapd
sleep 1
systemctl is-active hostapd >/dev/null || err "hostapd failed to start — check: journalctl -u hostapd"

# 8. Start dnsmasq (DHCP + DNS for connected clients)
systemctl start dnsmasq
systemctl is-active dnsmasq >/dev/null || err "dnsmasq failed to start — check: journalctl -u dnsmasq"

echo ""
echo "=== AP mode active ==="
echo "  SSID   : PiCamera"
echo "  IP     : ${AP_IP}/${AP_PREFIX}"
echo "  Web UI : http://${AP_IP}:8080"
echo "  mDNS   : http://picamera.local:8080"
echo ""
echo "Connected clients:"
hostapd_cli all_sta 2>/dev/null | grep -E '^([0-9a-f]{2}:){5}' || echo "  (none yet)"
