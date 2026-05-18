# PiCamera — Daily Workflow & Mode-Switching Guide

**Board:** Raspberry Pi Zero 2 W · Debian 13 (Trixie)  
**App ports:** HTTP :8080 (MJPEG + web UI) · WebSocket :8081 (commands)

---

## Two Operating Modes

| | Home / Client mode | AP (Outdoor) mode |
|---|---|---|
| Pi's role | WiFi client, connected to home router | WiFi Access Point |
| Pi IP | assigned by router (hostname `raspizero2`) | `192.168.4.1` (static) |
| Laptop connects to | Home router | SSID **PiCamera** |
| Web UI | `http://raspizero2:8080` | `http://192.168.4.1:8080` |
| mDNS | `http://raspizero2.local:8080` | `http://picamera.local:8080` |
| SSH | `ssh raspizero2` | `ssh vladimir@192.168.4.1` |
| Used for | Development, deploy, debug | Outdoor testing, final integration |

---

## Key Behaviour: App Survives Mode Switch

`pizero2cam_app` ignores `SIGHUP` (added to `main.cpp`). This means:

- When a mode-switch script drops the WiFi connection, the SSH session dies.
- The app keeps running — it listens on `0.0.0.0` and works in both modes.
- **You never need to restart the app after a mode switch.**

---

## Daily Development Workflow (Home Mode)

### 1. Build and deploy

```bash
# From project root on your Ubuntu machine:
./scripts/build_deploy.sh
```

This compiles, verifies the ARM64 binary, rsyncs it plus `web/` and the
mode-switch scripts to `~/apps/` on the Pi, kills the old instance, and
launches a new one in the foreground via SSH.

### 2. Run in foreground (debug output visible)

```bash
./scripts/deploy_run.sh debug
```

Live app output streams to your terminal. `Ctrl-C` sends `SIGINT` → clean
shutdown (GStreamer pipeline stopped, WebSocket connections closed).

### 3. Run in background (survives mode switch)

```bash
./scripts/deploy_run.sh debug --detach
```

The app starts with `nohup` on the Pi. This terminal tails `/tmp/cam.log`.
`Ctrl-C` here stops the tail only — **the app keeps running**.

Use this mode whenever you plan to switch to AP mode, so you do not have to
restart the app afterwards.

### 4. Test in Home mode

Open in browser:
```
http://raspizero2:8080
```

Quick WebSocket smoke test (browser DevTools → Console):
```javascript
ws = new WebSocket('ws://raspizero2:8081/ws')
ws.onmessage = e => console.log(JSON.parse(e.data))
ws.send(JSON.stringify({cmd:'ping'}))
ws.send(JSON.stringify({cmd:'get_status'}))
```

---

## Switching Home → AP Mode

### Prerequisites

- The app must be running in **detach mode** before you switch, or already
  running as a background process. If it was started with `deploy_run.sh debug`
  (foreground), the SSH will drop but the app still survives (SIGHUP is ignored).
- You need **two terminal windows** on your machine.

### Step-by-step

**Terminal 1** — start the app detached (if not already running):
```bash
./scripts/deploy_run.sh debug --detach
# Ctrl-C to stop the log tail — app keeps running on Pi
```

**Terminal 2** — switch the Pi to AP mode:
```bash
ssh raspizero2 "sudo ~/apps/ap_mode.sh"
```

Expected output on Terminal 2:
```
=== Switching to AP mode ===
=== AP mode active ===
  SSID   : PiCamera
  IP     : 192.168.4.1/24
  Web UI : http://192.168.4.1:8080
  mDNS   : http://picamera.local:8080
```

The SSH session in Terminal 2 **closes** immediately after — the Pi has left
your home WiFi. That is expected.

**On your laptop:** connect to WiFi network **PiCamera** (password: `picamera123`).

**Verify:**
```bash
ping 192.168.4.1                     # Pi responds
ssh vladimir@192.168.4.1             # SSH via AP works
```

---

## Testing in AP Mode

All commands below assume your laptop is connected to the **PiCamera** SSID.

### Network layer

```bash
ping 192.168.4.1
ping picamera.local                  # mDNS — should resolve to 192.168.4.1

# Verify laptop received a DHCP lease (192.168.4.2–192.168.4.20):
ip addr show                         # on laptop

# On Pi — check DHCP leases and connected clients:
ssh vladimir@192.168.4.1 "cat /var/lib/misc/dnsmasq.leases"
ssh vladimir@192.168.4.1 "sudo hostapd_cli all_sta"
```

### Web UI and MJPEG stream

Open in browser:
```
http://192.168.4.1:8080
http://picamera.local:8080           # via mDNS
```

Both URLs must show the UI and a live MJPEG stream.

### WebSocket commands

Browser DevTools → Console (or use `wscat -c ws://192.168.4.1:8081/ws`):

```javascript
ws = new WebSocket('ws://192.168.4.1:8081/ws')
ws.onmessage = e => console.log(JSON.parse(e.data))

ws.send(JSON.stringify({cmd:'ping'}))
// → {status:"ok", ts:...}

ws.send(JSON.stringify({cmd:'get_status'}))
// → {camera:{resolution,fps}, recording:{active,file}, storage:{free_mb,used_mb}, ...}

ws.send(JSON.stringify({cmd:'snapshot'}))
// → {status:"ok", file:"recordings/snap_...jpg"}

ws.send(JSON.stringify({cmd:'record_start'}))
// → {status:"ok", file:"recordings/rec_...mp4"}
// wait 5 seconds — recording_status push messages arrive every 1 s

ws.send(JSON.stringify({cmd:'record_stop'}))
// → {status:"ok", duration_s:5, file:"..."}

ws.send(JSON.stringify({cmd:'list_recordings'}))
// → {status:"ok", files:[{name, size_mb, modified}, ...]}
```

### WiFi link stability (power save check)

```bash
# 60 pings — must be 0% packet loss
ping -c 60 192.168.4.1

# Verify power save is off on Pi:
ssh vladimir@192.168.4.1 "iw dev wlan0 get power_save"
# Expected: Power save: off
```

### App log

```bash
ssh vladimir@192.168.4.1 "tail -f /tmp/cam.log"
```

---

## Switching AP → Home Mode

**From a terminal connected to the Pi via the AP** (192.168.4.1):

```bash
ssh vladimir@192.168.4.1 "sudo ~/apps/client_mode.sh"
```

The SSH session **closes** — the AP has shut down. That is expected.

**On your laptop:** disconnect from **PiCamera** and reconnect to your home WiFi.

Wait ~10 seconds, then:

```bash
ping raspizero2
ssh raspizero2                       # Pi is back on home WiFi

# App is still running — verify:
ssh raspizero2 "pgrep -a pizero2cam_app && tail -5 /tmp/cam.log"
```

---

## Quick Reference Card

```
─── Home mode ─────────────────────────────────────────────────────────────────

  Build & deploy:    ./scripts/build_deploy.sh
  Run (foreground):  ./scripts/deploy_run.sh debug
  Run (background):  ./scripts/deploy_run.sh debug --detach
  Web UI:            http://raspizero2:8080
  SSH:               ssh raspizero2
  Stop app:          ssh raspizero2 'pkill -x pizero2cam_app'
  Watch log:         ssh raspizero2 'tail -f /tmp/cam.log'

─── Switch Home → AP ──────────────────────────────────────────────────────────

  ssh raspizero2 "sudo ~/apps/ap_mode.sh"
  # SSH closes → connect laptop to WiFi: PiCamera / picamera123

─── AP mode ───────────────────────────────────────────────────────────────────

  Web UI:            http://192.168.4.1:8080
                     http://picamera.local:8080
  SSH:               ssh vladimir@192.168.4.1
  Watch log:         ssh vladimir@192.168.4.1 'tail -f /tmp/cam.log'
  DHCP clients:      ssh vladimir@192.168.4.1 'sudo hostapd_cli all_sta'

─── Switch AP → Home ──────────────────────────────────────────────────────────

  ssh vladimir@192.168.4.1 "sudo ~/apps/client_mode.sh"
  # SSH closes → reconnect laptop to home WiFi → ssh raspizero2

───────────────────────────────────────────────────────────────────────────────
```

---

## Troubleshooting

### SSH hangs or connection refused after mode switch

Expected — the network interface is changing. Wait 5–10 seconds and retry.
If it still fails after 30 seconds:

```bash
# Home mode: try the IP directly instead of hostname
ssh vladimir@$(arp -n | grep raspizero2 | awk '{print $1}')

# AP mode: verify hostapd started
# (requires a direct HDMI + keyboard connection, or a pre-existing AP SSH)
sudo systemctl status hostapd
```

### App not reachable after mode switch

Check it survived:
```bash
ssh <pi-address> "pgrep pizero2cam_app || echo 'NOT RUNNING'"
```

If not running (e.g., it was started in the foreground and SIGHUP was not yet
handled), restart it:
```bash
ssh <pi-address> "cd ~/apps && nohup ./pizero2cam_app >> /tmp/cam.log 2>&1 &"
```

### `picamera.local` does not resolve in AP mode

```bash
ssh vladimir@192.168.4.1 "systemctl status avahi-daemon"
# If not running:
ssh vladimir@192.168.4.1 "sudo systemctl start avahi-daemon"
```

### Packet loss in AP mode (power save re-enabled)

```bash
ssh vladimir@192.168.4.1 "iw dev wlan0 set power_save off"
```

If this recurs after every ap_mode.sh run, verify that
`/etc/NetworkManager/conf.d/wifi-power-save.conf` exists on the Pi with
`wifi.powersave = 2`.

### hostapd fails to start (`channel not allowed`)

Regulatory domain not set. Run on Pi:
```bash
sudo iw reg set DE
sudo systemctl restart hostapd
# Make permanent:
grep -q 'ieee80211_regdom' /etc/modprobe.d/cfg80211.conf || \
  echo 'options cfg80211 ieee80211_regdom=DE' | sudo tee /etc/modprobe.d/cfg80211.conf
```

---

## One-Time AP Setup (if not done yet)

Run these once on the Pi via SSH while in Home mode:

```bash
sudo apt install -y hostapd dnsmasq avahi-daemon ufw iw rfkill

# Regulatory domain
echo 'options cfg80211 ieee80211_regdom=DE' | \
  sudo tee /etc/modprobe.d/cfg80211.conf

# hostapd config
sudo tee /etc/hostapd/hostapd.conf <<'EOF'
interface=wlan0
driver=nl80211
ssid=PiCamera
hw_mode=g
channel=6
country_code=DE
ieee80211n=1
wmm_enabled=1
macaddr_acl=0
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=picamera123
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
rsn_pairwise=CCMP
EOF
sudo sed -i 's|#DAEMON_CONF=.*|DAEMON_CONF="/etc/hostapd/hostapd.conf"|' \
  /etc/default/hostapd

# dnsmasq
sudo tee /etc/dnsmasq.d/picamera.conf <<'EOF'
interface=wlan0
bind-interfaces
dhcp-range=192.168.4.2,192.168.4.20,255.255.255.0,12h
address=/picamera.local/192.168.4.1
domain-needed
bogus-priv
EOF

# Services stay disabled — ap_mode.sh starts them manually
sudo systemctl disable hostapd dnsmasq

# UFW
sudo ufw allow ssh
sudo ufw allow 8080/tcp
sudo ufw allow 8081/tcp
sudo ufw allow 5353/udp
sudo ufw --force enable

# avahi
sudo hostnamectl set-hostname picamera
sudo systemctl enable avahi-daemon

# WiFi power save
sudo tee /etc/NetworkManager/conf.d/wifi-power-save.conf <<'EOF'
[connection]
wifi.powersave = 2
EOF
```
