# PiCamera — Raspberry Pi Zero 2 W Camera Controller

A self-contained outdoor camera system. The Pi acts as a standalone WiFi access
point; a laptop connects directly to it and receives a live MJPEG video stream
in the browser while sending control commands over WebSocket — no router or
internet required.

---

## Hardware

| Component | Detail |
|---|---|
| Compute | Raspberry Pi Zero 2 W (quad-core Cortex-A53, 512 MB LPDDR4) |
| OS | Debian 13 (Trixie), 64-bit |
| Camera | Pi Camera Module v1 (OV5647, 5 MP, CSI-2) |
| Power | Waveshare UPS HAT (C) — INA219 battery monitor at I2C address 0x43 |
| Battery | 1× 18650 Li-ion (3.7 V) |
| Storage | MicroSD, 32 GB+ (U3/A2 class recommended for recording) |
| WiFi | Onboard 2.4 GHz 802.11 b/g/n (AP mode outdoors, client mode at home) |

---

## Features

| Feature | Detail |
|---|---|
| MJPEG stream | 30 fps at 720p, hardware-encoded via `v4l2jpegenc` (bcm2835-codec) |
| H.264 recording | MP4 via `openh264enc` software encoder; start/stop/restart on demand |
| JPEG snapshots | Captured from the live MJPEG pipeline, saved to `recordings/` |
| Resolution switch | 640×480 / 1280×720 / 1920×1080, hot-switched at runtime |
| Battery monitor | INA219 polled every 5 s; voltage, current, percentage pushed over WebSocket |
| Web UI | Single-page HTML served from the Pi; resolution buttons, record controls, recordings list |
| WebSocket API | JSON commands and server-push events on port 8081 |
| AP mode | Pi becomes a WiFi AP (SSID: PiCamera); no router needed outdoors |

---

## Port Map

| Port | Protocol | Purpose |
|---|---|---|
| 8080 | TCP | HTTP — web UI, MJPEG stream (`/stream.mjpeg`), snapshots (`/snapshot.jpg`) |
| 8081 | TCP | WebSocket — JSON commands and push events |
| 5353 | UDP | mDNS (avahi) — `picamera.local` resolves in AP mode |
| 22 | TCP | SSH — administration and deployment |

---

## WebSocket Command Reference

Send JSON to `ws://<pi>:8081/ws`. Every command returns a JSON response.

### Commands

| Command | Parameters | Response |
|---|---|---|
| `ping` | — | `{status, ts}` |
| `get_status` | — | `{camera, streaming, ws, recording, storage, battery}` |
| `get_battery` | — | `{voltage_v, current_ma, power_mw, percentage, charging}` |
| `set_resolution` | `width`, `height` | `{status, resolution}` e.g. `"1280x720@30"` |
| `record_start` | `filename` (optional) | `{status, file}` |
| `record_stop` | — | `{status, file, duration_s}` |
| `record_restart` | — | `{status, file}` |
| `snapshot` | `filename` (optional) | `{status, file}` |
| `list_recordings` | — | `{status, files:[{name, size_mb, modified}]}` |
| `delete_recording` | `filename` | `{status}` |

### Server push messages (unsolicited)

| Type | When |
|---|---|
| `battery_update` | Every 5 s |
| `recording_status` | Every 1 s while recording (includes duration and file size) |
| `storage_warning` | When free space drops below 500 MB |

---

## GStreamer Pipeline

```
libcamerasrc
! video/x-raw,format=NV12,width=W,height=H,framerate=FPS/1
! tee name=t

  t. ! queue leaky=downstream
     ! v4l2jpegenc                  ← hardware JPEG (bcm2835-codec, DMA-BUF, 0 CPU)
     ! appsink name=mjpeg_sink      → MjpegServer (HTTP) + snapshot

  t. ! queue leaky=downstream
     ! appsink name=rec_sink        → rec_drain_thread → Recorder
```

The **Recorder** creates a separate GStreamer pipeline per session:
```
appsrc (NV12) ! videoconvert ! openh264enc ! h264parse ! mp4mux ! filesink
```

`v4l2h264enc` cannot run alongside `v4l2jpegenc` — the bcm2835-codec block
cannot serve two simultaneous V4L2 M2M DMA-BUF consumers. `openh264enc`
(software) uses ~35–55% of one Cortex-A53 core at 720p.

---

## Source Tree

```
camera_controller/
├── aarch64-pi-zero2.cmake          # Cross-compilation toolchain (Ubuntu → aarch64)
├── CMakeLists.txt                  # C++17, pkg-config for libcamera + GStreamer
├── scripts/
│   ├── build_deploy.sh             # Build → verify → rsync → run
│   ├── deploy_run.sh               # Deploy only; foreground or --detach mode
│   ├── ap_mode.sh                  # Switch Pi to WiFi AP mode
│   ├── client_mode.sh              # Switch Pi back to home WiFi client
│   └── start_gdb_server.sh         # Remote GDB server for cross-debug
├── src/
│   ├── main.cpp                    # Entry point; command registration; main loop
│   ├── camera/
│   │   ├── resolution.h            # LOW / MEDIUM / HIGH presets
│   │   ├── camera_manager.h/.cpp   # libcamera raw-capture wrapper (legacy path)
│   │   └── frame_buffer.h          # FramePtr / Frame — zero-copy buffer handle
│   ├── stream/
│   │   ├── gst_camera_source.h/.cpp  # GStreamer tee pipeline; recording; snapshot
│   │   ├── mjpeg_server.h/.cpp       # HTTP multipart/x-mixed-replace MJPEG server
│   │   ├── jpeg_hw_encoder.h/.cpp    # V4L2 M2M JPEG encoder (legacy)
│   │   └── jpeg_mt_encoder.h/.cpp    # libjpeg-turbo NEON encoder (legacy)
│   ├── record/
│   │   ├── recorder.h/.cpp           # GStreamer NV12 → openh264enc → MP4
│   │   └── h264_hw_encoder.h/.cpp    # V4L2 M2M H.264 encoder (legacy)
│   ├── snapshot/
│   │   └── snapshot.h/.cpp           # Standalone JPEG snapshot (legacy)
│   ├── control/
│   │   ├── ws_server.h/.cpp          # WebSocket server (cpp-httplib, port 8081)
│   │   └── command_dispatcher.h/.cpp # JSON command routing
│   └── power/
│       ├── ina219.h/.cpp             # INA219 I2C driver
│       └── battery_monitor.h/.cpp    # 5 s polling thread + WebSocket push
├── third_party/
│   ├── httplib.h                   # cpp-httplib — HTTP + WebSocket server
│   └── nlohmann/json.hpp           # JSON parsing
├── web/
│   └── index.html                  # Browser UI (served by MjpegServer)
├── ModeSwitchRaspiCam.md           # Daily workflow and mode-switching guide
└── PlanImpCamRaspiZero.md          # Implementation plan and phase history
```

---

## Cross-Compilation Setup

Built on Ubuntu 22.04 x86-64, deployed to the Pi via rsync over SSH.

**Toolchain:** `aarch64-linux-gnu-g++` (Ubuntu package `gcc-aarch64-linux-gnu`)  
**Sysroot:** `~/rpi-sysroot` — sync once from the Pi:

```bash
rsync -avz raspizero2:/usr/include ~/rpi-sysroot/usr/
rsync -avz raspizero2:/usr/lib     ~/rpi-sysroot/usr/
rsync -avz raspizero2:/lib         ~/rpi-sysroot/
```

**Build and deploy:**

```bash
./scripts/build_deploy.sh
```

This configures CMake with the toolchain file, builds, verifies the binary is
ARM64, rsyncs it to `~/apps/` on the Pi, and launches it.

**Run without rebuilding:**

```bash
./scripts/deploy_run.sh debug            # foreground — Ctrl-C for clean shutdown
./scripts/deploy_run.sh debug --detach   # background — survives SSH disconnection
```

---

## Network Modes

The Pi operates in two mutually exclusive WiFi modes.

| | Home / Client mode | AP (Outdoor) mode |
|---|---|---|
| Pi IP | assigned by home router | `192.168.4.1` (static) |
| SSH | `ssh raspizero2` | `ssh vladimir@192.168.4.1` |
| Web UI | `http://raspizero2:8080` | `http://192.168.4.1:8080` |
| mDNS | — | `http://picamera.local:8080` |

**Switch to AP mode** (from any terminal on home WiFi):
```bash
ssh raspizero2 "sudo ~/apps/ap_mode.sh"
# SSH closes → connect laptop to WiFi: PiCamera / picamera123
```

**Switch back to home WiFi** (from a terminal connected via AP):
```bash
ssh vladimir@192.168.4.1 "sudo ~/apps/client_mode.sh"
# SSH closes → reconnect laptop to home WiFi
```

The app ignores `SIGHUP` and keeps running across both mode switches.  
See [ModeSwitchRaspiCam.md](ModeSwitchRaspiCam.md) for the full workflow and test checklist.

---

## One-Time Pi Setup

Run once on the Pi (via SSH, in home mode) before first outdoor use:

```bash
sudo apt install -y hostapd dnsmasq avahi-daemon ufw iw rfkill

# Regulatory domain (DE)
echo 'options cfg80211 ieee80211_regdom=DE' | \
  sudo tee /etc/modprobe.d/cfg80211.conf

# hostapd
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
sudo systemctl disable hostapd dnsmasq

# dnsmasq
sudo tee /etc/dnsmasq.d/picamera.conf <<'EOF'
interface=wlan0
bind-interfaces
dhcp-range=192.168.4.2,192.168.4.20,255.255.255.0,12h
address=/picamera.local/192.168.4.1
domain-needed
bogus-priv
EOF

# Firewall
sudo ufw allow ssh
sudo ufw allow 8080/tcp
sudo ufw allow 8081/tcp
sudo ufw allow 5353/udp
sudo ufw --force enable

# mDNS hostname
sudo hostnamectl set-hostname picamera
sudo systemctl enable avahi-daemon

# WiFi power save off (persistent)
sudo tee /etc/NetworkManager/conf.d/wifi-power-save.conf <<'EOF'
[connection]
wifi.powersave = 2
EOF
```

---

## Dependencies

### On the Pi (runtime)

```bash
sudo apt install -y \
  libcamera0 gstreamer1.0-tools \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  libopencore-amrnb0                    # pulled by openh264enc
```

`libcamerasrc` GStreamer plugin is installed to `~/gst-plugins/` (user-local,
no root required). `GstCameraSource` sets `GST_PLUGIN_PATH` before `gst_init()`.

### On the build host (compile-time)

```bash
sudo apt install -y \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  cmake pkg-config
```

Sysroot provides: `libcamera`, `gstreamer-1.0`, `gstreamer-app-1.0`, `libjpeg`.

---

## Resolution Presets

| Label | Width | Height | FPS | H.264 bitrate | Use case |
|---|---|---|---|---|---|
| LOW | 640 | 480 | 30 | 500 kbps | Weak signal, maximum range |
| MEDIUM | 1280 | 720 | 30 | 1500 kbps | Default |
| HIGH | 1920 | 1080 | 25 | 3000 kbps | Strong signal only |
