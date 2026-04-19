# M5Stack Tab5 Video Stream — Project Notes

## Goal

MJPEG video + synchronized audio streaming over WiFi to the M5Stack Tab5 (ESP32-P4).
Optionally stream from local video files or live web sources (YouTube via yt-dlp).

This is a sibling project to `~/hardware/video-stream` (which targets the ESP32-P4 EV Board
and Waveshare ESP32-S3). The Tab5 board gets its own project due to different display,
audio codec, and resolution.

---

## Target Hardware: M5Stack Tab5

| Component | Detail |
|-----------|--------|
| SoC | ESP32-P4 (dual-core RISC-V, 400MHz) |
| WiFi/BT | ESP32-C6-MINI-1U via SDIO (WiFi 6) |
| Display | 5" 1280×720 IPS, MIPI-DSI |
| Flash / PSRAM | 16MB Flash, 32MB PSRAM |
| Audio codec | ES8388 (stereo, I2S + I2C) |
| AEC front-end | ES7210 |
| Microphones | Dual array |
| Audio out | Onboard speaker + 3.5mm headphone jack |
| Camera | SC2356 2MP MIPI-CSI |
| IMU | BMI270 6-axis |
| Power | NP-F550 removable Li-ion battery |

BSP available in `espressif/esp-bsp` — use it for display init, audio codec init,
touch, and power management rather than writing drivers from scratch.

BSP audio functions: `bsp_audio_codec_speaker_init()`, `bsp_audio_codec_microphone_init()`

---

## Architecture

### Server (Raspberry Pi)

HTTP-based pull model — the ESP32 requests data on demand rather than the server pushing
a stream. This handles WiFi hiccups gracefully and makes A/V sync straightforward.

Endpoints:
- `GET /info` — returns list of available channels with duration (in audio samples)
- `GET /frame/<channel>/<ms>` — returns JPEG for that channel at timestamp ms
- `GET /audio/<channel>/<start>/<length>` — returns raw 8-bit PCM mono at 16kHz,
  starting at sample `start` for `length` samples

Pre-processing:
- On first run, extract all JPEG frames and PCM audio from each video into a cache dir
- Use ffmpeg for extraction (see ffmpeg commands below)
- Serve from cache on subsequent runs

For web/YouTube sources:
- Use yt-dlp to get the direct stream URL, pipe into ffmpeg
- Hardware HEVC decode available on Pi 5 via `-hwaccel drm -c:v hevc_v4l2m2m` if source
  is H.265 — use it when available, fall back to software for H.264/VP9
- Pi 5 CPU handles H.264 software decode easily at these resolutions

### Firmware (ESP32-P4 / Tab5)

1. Connect to WiFi via ESP32-C6 (esp_hosted over SDIO — same as P4 EV board)
2. Fetch `/info` to get channel list
3. Start I2S audio playback to ES8388, requesting audio chunks via `/audio/...`
4. **Lock video to audio:** use I2S DMA sample counter to know elapsed playback time,
   request `/frame/<channel>/<ms>` at the correct timestamp
5. Decode JPEG via ESP32-P4 hardware JPEG decoder, push to MIPI-DSI framebuffer
6. Double-buffer the display for smooth playback

A/V sync rationale: the I2S peripheral's sample counter is the ground truth for elapsed
time — it never drifts. Requesting video frames based on samples consumed (not wall clock)
means audio and video are always in sync by construction.

---

## Key Differences from Sibling Project (video-stream)

| | video-stream | this project |
|---|---|---|
| Protocol | TCP push | HTTP pull |
| Display | 1024×600 MIPI-DSI (P4 EV) / 320×240 SPI (S3) | 1280×720 MIPI-DSI |
| Audio | None | ES8388 stereo |
| A/V sync | N/A | I2S sample counter |
| Web streaming | Planned | Planned |
| Board | P4 EV + Waveshare S3 | M5Stack Tab5 only |

---

## ffmpeg Commands

### Extract frames from a local video (pre-processing)
```bash
ffmpeg -i input.mp4 \
  -vf "scale=1280:720:force_original_aspect_ratio=increase,crop=1280:720" \
  -r 30 -q:v 4 \
  cache/frames_%05d.jpg
```

### Extract audio (pre-processing)
```bash
ffmpeg -i input.mp4 \
  -vn -acodec pcm_u8 -ar 16000 -ac 1 \
  -af "loudnorm" \
  cache/audio.raw
```

### Web source via yt-dlp
```bash
URL=$(yt-dlp -f "best[height<=720]" --get-url "https://...")
# H.265 source — hardware decode
ffmpeg -hwaccel drm -c:v hevc_v4l2m2m -i "$URL" ...
# H.264/VP9 source — software decode (Pi 5 handles this fine)
ffmpeg -i "$URL" ...
```

---

## Reference

- Sibling project: `~/hardware/video-stream` — read this for P4 board patterns,
  esp_hosted WiFi setup, MIPI-DSI framebuffer approach, hardware JPEG decode
- esp-bsp Tab5 BSP: `espressif/esp-bsp` (component registry)
- atomic14/esp32-tv: studied for architecture ideas only — no license, do NOT copy code.
  Key ideas borrowed: HTTP pull model, I2S-locked A/V sync concept.
- Pi 5 hardware decoders: HEVC only (`/dev/video19`, `rpi-hevc-dec`). No hardware H.264.
  Use `-hwaccel drm` not v4l2m2m for HEVC on Pi 5.

---

## ESP-IDF Notes

- Default IDF version: 5.5.3 (use `~/bin/idf` wrapper)
- Target: `idf set-target esp32p4`
- Requires `CONFIG_ESP32P4_SELECTS_REV_LESS_THAN_V3=y` — all hardware is v1.x silicon
- WiFi via esp_hosted (ESP32-C6 SDIO) — same as P4 EV board, copy that config
- PSRAM: verify `CONFIG_SPIRAM=y` after every `set-target`
- sdkconfig.defaults must explicitly list `sdkconfig.defaults` before `~/.esp_creds`
  when using SDKCONFIG_DEFAULTS append pattern

---

## Implementation Order

1. Server: basic HTTP server with local file support (no web streaming yet)
2. Firmware: display init via BSP, show test image
3. Firmware: fetch and display JPEG frames (video only, no audio)
4. Firmware: add I2S audio via ES8388, play audio chunks from server
5. Firmware: lock video requests to I2S sample counter (A/V sync)
6. Server: add yt-dlp web source mode
7. Server: add hardware HEVC decode path for Pi 5
