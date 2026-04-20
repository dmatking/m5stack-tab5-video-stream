# M5Stack Tab5 Video Stream

MJPEG video + synchronized PCM audio streaming over WiFi to the M5Stack Tab5 (ESP32-P4).

The server pre-extracts frames and audio from any YouTube video (via yt-dlp) or local
file into a disk cache, then serves them over HTTP. The firmware fetches frames and audio
chunks on demand, decodes JPEG in hardware, rotates via PPA, and plays audio through the
ES8388 codec — all with A/V sync locked to wall clock.

---

## Hardware

| Component | Detail |
|-----------|--------|
| Board | M5Stack Tab5 |
| SoC | ESP32-P4 (dual-core RISC-V 400 MHz) |
| WiFi | ESP32-C6 co-processor via SDIO |
| Display | 5" 1280×720 MIPI-DSI (portrait framebuffer) |
| Audio | ES8388 codec, onboard speaker |

---

## Server setup

The server runs on any Linux machine with Python 3, ffmpeg, and yt-dlp. A Raspberry Pi 5
works well and is what this was developed on.

### Install dependencies

```bash
pip3 install flask gunicorn yt-dlp
# ffmpeg via system package manager, e.g.:
sudo apt install ffmpeg
```

### Configure channels

Edit `server/channels.json` — each key is a channel name, value is a YouTube URL
or a path to a local video file:

```json
{
  "my_channel": "https://www.youtube.com/watch?v=..."
}
```

On first request the server resolves the URL with yt-dlp and extracts all frames
and audio into `server/cache/<channel>/`. Subsequent runs serve from cache instantly.

### Run the server

```bash
cd server
gunicorn -w 2 -b 0.0.0.0:8080 server:app
```

The server will begin extraction in the background on the first request. Video playback
starts as soon as the first frames are available — you don't need to wait for the full
video to be extracted.

---

## Firmware setup

### Prerequisites

- ESP-IDF 5.5.3 (`~/esp/esp-idf-v5.5.3` or set `IDF_PATH`)
- WiFi credentials in `~/.esp_creds`:

```
CONFIG_WIFI_SSID="YourNetwork"
CONFIG_WIFI_PASS="YourPassword"
```

### Configure

```bash
idf.py menuconfig
# → Video Stream Config
#   SERVER_IP  — IP address of the machine running the server
#   SERVER_PORT — 8080 by default
#   CHANNEL    — must match a key in channels.json
```

### Build and flash

```bash
idf.py build
idf.py flash
```

---

## Architecture

### HTTP pull model

The firmware requests data on demand rather than the server pushing a stream. This
tolerates WiFi hiccups gracefully — a missed frame is simply retried on the next
request.

```
ESP32-P4                          Server (Pi)
─────────────────────────────     ────────────────────────────
GET /frame/<channel>/<ms>   ───►  serve frame_NNNNN.jpg from disk
GET /audio/<channel>/<s>/<n>───►  serve raw u8 PCM slice from audio.raw
GET /info                   ───►  channel metadata (duration, fps, etc.)
```

### Video pipeline (ESP32-P4)

```
[fetch task, core 1]          [decode task, core 0]
  HTTP GET /frame               xQueueReceive(ready_q)
  → JPEG in PSRAM slot          HW JPEG decode → RGB565
  → xQueueSend(ready_q)         PPA rotate 90° CW → framebuffer
  ← xQueueReceive(free_q)       board_lcd_commit()  (double-buffer flip)
                                vTaskDelayUntil(50ms)  ← paces to 20fps
```

16 pipeline slots provide ~800 ms of buffer to absorb WiFi retransmit spikes.

### A/V sync

Both audio and video reference wall clock from the moment the first frame is
successfully fetched. Audio samples are consumed by the I2S DMA at exactly
16 kHz — any drift in the fetch rate shows up as silence (not desync).

### Display

Frames are extracted at 992×560 (landscape) and rotated 90° CW on-device via the
PPA hardware accelerator, then letterboxed into the 720×1280 portrait framebuffer.
Double buffering (2 hardware DPI framebuffers) eliminates tearing.

### Server pre-processing

ffmpeg extracts frames at 20 fps and audio as mono unsigned 8-bit PCM at 16 kHz.
On Raspberry Pi 5, H.265 sources use hardware decode (`hevc_v4l2m2m`);
H.264/VP9 fall back to software (the Pi 5 CPU handles this at these resolutions).

---

## Notes

**The channel to play is hardcoded in the firmware.** It is set via `CHANNEL` in
`menuconfig` (or `sdkconfig.defaults`) and compiled in. To switch to a different video,
update the channel name, rebuild, and reflash.

---

## TODO

- **Play / pause and volume controls** — use the Tab5's onboard buttons or touchscreen
  to pause playback and adjust volume without reflashing
- **On-device channel selection** — browse and switch channels directly from the Tab5
  touchscreen, no server interaction or reflash required
- **Server web interface** — a browser UI to add new videos (YouTube URLs or local
  files), monitor extraction progress, and manage the channel list

---

## Tuning

| Parameter | Location | Effect |
|-----------|----------|--------|
| `FPS` | `server/server.py` | Extraction frame rate (default 20) |
| `PIPELINE_SLOTS` | `main/main.c` | Pre-fetch buffer depth (default 16 = ~800 ms) |
| `AUDIO_CHUNK_SAMPLES` | `main/main.c` | Audio fetch granularity (default 1600 = 100 ms) |
| `JPEG_IN_MAX` | `main/main.c` | Max compressed JPEG size per frame (default 128 KB) |
| `SRC_W` / `SRC_H` | `main/main.c` | Frame dimensions — must be divisible by 8 |
