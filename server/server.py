#!/usr/bin/env python3
# MJPEG + PCM audio server for M5Stack Tab5 video stream project.
#
# Channels are defined in channels.json:
#   { "slug": { "url": "https://youtube.com/watch?v=...", "title": "Song Name" } }
#
# On first request for a channel, yt-dlp resolves the stream URL and ffmpeg
# extracts JPEG frames + raw PCM in one pass in a background thread.
# /frame and /audio requests are served as soon as the relevant content is
# available on disk — no need to wait for the full video.
#
# API:
#   GET  /info                       — list channels with metadata
#   GET  /frame/<slug>/<ms>          — JPEG frame at timestamp ms
#   GET  /audio/<slug>/<start>/<len> — raw u8 PCM at sample offset
#   POST /channel  {video_id, title} — add a YouTube video
#   DELETE /channel/<slug>           — remove a channel

import json
import subprocess
import sys
import threading
import time
from pathlib import Path

from flask import Flask, Response, abort, request

app = Flask(__name__)

BASE_DIR      = Path(__file__).parent
CACHE_DIR     = BASE_DIR / "cache"
CHANNELS_FILE = BASE_DIR / "channels.json"

FPS          = 20
SAMPLE_RATE  = 16000       # Hz, mono, unsigned 8-bit PCM
WAIT_TIMEOUT = 20.0        # seconds to wait for a frame/audio chunk to appear
WAIT_POLL    = 0.1         # seconds between polls

HAS_HEVC_HW = Path("/dev/video19").exists()

# ---------------------------------------------------------------------------
# Per-channel extraction state
# ---------------------------------------------------------------------------

_states: dict[str, dict] = {}
_states_lock = threading.Lock()


def _state(channel: str) -> dict:
    with _states_lock:
        if channel not in _states:
            _states[channel] = {"status": "idle", "thread": None, "error": None}
        return _states[channel]


def _resolve_stream(youtube_url: str) -> tuple[list[str], bool]:
    """Return ([url, ...], is_hevc).  One URL = muxed, two = video+audio."""
    if HAS_HEVC_HW:
        candidates = [
            ("bv[height<=720][vcodec~='^hev'][protocol=https]+ba[protocol=https]/bv[height<=720][vcodec~='^hev'][protocol=https]+ba", True),
            ("bv[height<=720][protocol=https]+ba[protocol=https]/bv[height<=720]+ba/b[height<=720]", False),
        ]
    else:
        candidates = [("bv[height<=720][protocol=https]+ba[protocol=https]/bv[height<=720]+ba/b[height<=720]", False)]

    for fmt, is_hevc in candidates:
        r = subprocess.run(
            ["yt-dlp", "-f", fmt, "--get-url", youtube_url],
            capture_output=True, text=True,
        )
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip().splitlines(), is_hevc

    raise RuntimeError(f"yt-dlp could not resolve: {youtube_url}")


def _run_extraction(channel: str, youtube_url: str) -> None:
    state = _state(channel)
    frames_dir = CACHE_DIR / channel / "frames"
    audio_path = CACHE_DIR / channel / "audio.raw"
    frames_dir.mkdir(parents=True, exist_ok=True)

    try:
        frames_done = frames_dir.exists() and bool(list(frames_dir.glob("frame_*.jpg")))
        audio_done  = audio_path.exists() and audio_path.stat().st_size > 0
        if frames_done and audio_done:
            print(f"[{channel}] cache already complete, skipping extraction")
            with _states_lock:
                state["status"] = "done"
            return

        print(f"[{channel}] resolving stream URL...")
        urls, is_hevc = _resolve_stream(youtube_url)
        hw = is_hevc and HAS_HEVC_HW
        print(f"[{channel}] {len(urls)} URL(s), hevc={is_hevc}, hw={hw}")

        if hw:
            pre = ["-hwaccel", "drm", "-c:v", "hevc_v4l2m2m"]
        else:
            pre = []

        if len(urls) == 1:
            inputs    = [*pre, "-i", urls[0]]
            video_map = ["-map", "0:v"]
            audio_map = ["-map", "0:a"]
        else:
            inputs    = [*pre, "-i", urls[0], "-i", urls[1]]
            video_map = ["-map", "0:v"]
            audio_map = ["-map", "1:a"]

        frames_done = bool(list(frames_dir.glob("frame_*.jpg")))
        audio_done  = audio_path.exists() and audio_path.stat().st_size > 0

        video_outputs = [] if frames_done else [
            *video_map,
            "-vf", "scale=992:560",
            "-r", str(FPS), "-q:v", "25",
            str(frames_dir / "frame_%05d.jpg"),
        ]
        audio_outputs = [] if audio_done else [
            *audio_map,
            "-acodec", "pcm_u8", "-ar", str(SAMPLE_RATE), "-ac", "1",
            "-f", "u8",
            str(audio_path),
        ]

        if video_outputs or audio_outputs:
            what = " + ".join(filter(None, [
                None if frames_done else "frames",
                None if audio_done  else "audio",
            ]))
            print(f"[{channel}] extracting {what}...")
            subprocess.run([
                "ffmpeg", "-y", *inputs,
                *video_outputs,
                *audio_outputs,
            ], check=True)

        print(f"[{channel}] extraction complete")
        with _states_lock:
            state["status"] = "done"

    except Exception as e:
        print(f"[{channel}] extraction failed: {e}")
        with _states_lock:
            state["status"] = "failed"
            state["error"]  = str(e)


def _ensure_extracting(channel: str, youtube_url: str) -> None:
    state = _state(channel)
    with _states_lock:
        if state["status"] in ("extracting", "done"):
            return
        state["status"] = "extracting"
        t = threading.Thread(target=_run_extraction, args=(channel, youtube_url), daemon=True)
        state["thread"] = t
        t.start()


# ---------------------------------------------------------------------------
# Wait helpers
# ---------------------------------------------------------------------------

def _wait_for_frame(channel: str, frame_num: int) -> Path | None:
    path = CACHE_DIR / channel / "frames" / f"frame_{frame_num:05d}.jpg"
    deadline = time.monotonic() + WAIT_TIMEOUT
    while time.monotonic() < deadline:
        if path.exists() and path.stat().st_size > 0:
            return path
        if _state(channel)["status"] in ("done", "failed"):
            return None
        time.sleep(WAIT_POLL)
    return None


def _wait_for_audio(channel: str, end_sample: int) -> bool:
    audio_path = CACHE_DIR / channel / "audio.raw"
    deadline   = time.monotonic() + WAIT_TIMEOUT
    while time.monotonic() < deadline:
        if audio_path.exists() and audio_path.stat().st_size >= end_sample:
            return True
        if _state(channel)["status"] in ("done", "failed"):
            return False
        time.sleep(WAIT_POLL)
    return False


# ---------------------------------------------------------------------------
# Channel config
# ---------------------------------------------------------------------------

def load_channels() -> dict[str, dict]:
    """Return {slug: {url, title}} — normalises the old {slug: url_string} format."""
    if not CHANNELS_FILE.exists():
        return {}
    with open(CHANNELS_FILE) as f:
        raw = json.load(f)
    result = {}
    for slug, val in raw.items():
        if isinstance(val, str):
            result[slug] = {"url": val, "title": slug.replace("_", " ").title()}
        else:
            result[slug] = val
    return result


def save_channels(channels: dict[str, dict]) -> None:
    with open(CHANNELS_FILE, "w") as f:
        json.dump(channels, f, indent=2)


def _channel_info(channel: str, title: str) -> dict:
    frames_dir = CACHE_DIR / channel / "frames"
    audio_path = CACHE_DIR / channel / "audio.raw"
    return {
        "title":            title,
        "duration_samples": audio_path.stat().st_size if audio_path.exists() else 0,
        "frame_count":      len(list(frames_dir.glob("frame_*.jpg"))) if frames_dir.exists() else 0,
        "fps":              FPS,
        "sample_rate":      SAMPLE_RATE,
        "status":           _state(channel)["status"],
    }


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------

@app.route("/info")
def info():
    channels = load_channels()
    for slug, ch in channels.items():
        _ensure_extracting(slug, ch["url"])
    result = {slug: _channel_info(slug, ch["title"]) for slug, ch in channels.items()}
    return Response(json.dumps(result, indent=2), mimetype="application/json")


@app.route("/channel", methods=["POST"])
def add_channel():
    data     = request.get_json(force=True) or {}
    video_id = data.get("video_id", "").strip()
    title    = data.get("title", "").strip()
    if not video_id:
        abort(400)

    youtube_url = f"https://www.youtube.com/watch?v={video_id}"

    if not title:
        r = subprocess.run(
            ["yt-dlp", "--get-title", youtube_url],
            capture_output=True, text=True, timeout=15,
        )
        title = r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else video_id

    slug = "".join(c if c.isalnum() else "_" for c in video_id)

    channels = load_channels()
    channels[slug] = {"url": youtube_url, "title": title}
    save_channels(channels)

    _ensure_extracting(slug, youtube_url)

    return Response(json.dumps({"slug": slug, "title": title}),
                    mimetype="application/json", status=201)


@app.route("/channel/<slug>", methods=["DELETE"])
def delete_channel(slug):
    channels = load_channels()
    if slug not in channels:
        abort(404)
    del channels[slug]
    save_channels(channels)
    return Response(status=204)


@app.route("/frame/<channel>/<int:ms>")
def frame(channel, ms):
    channels = load_channels()
    if channel not in channels:
        abort(404)

    _ensure_extracting(channel, channels[channel]["url"])

    frame_num  = max(1, round(ms * FPS / 1000) + 1)
    frame_path = _wait_for_frame(channel, frame_num)

    if frame_path is None:
        frames = sorted((CACHE_DIR / channel / "frames").glob("frame_*.jpg"))
        if not frames:
            abort(503)
        frame_path = frames[-1]

    return Response(frame_path.read_bytes(), mimetype="image/jpeg")


@app.route("/audio/<channel>/<int:start>/<int:length>")
def audio(channel, start, length):
    channels = load_channels()
    if channel not in channels:
        abort(404)

    _ensure_extracting(channel, channels[channel]["url"])

    audio_path = CACHE_DIR / channel / "audio.raw"

    if not _wait_for_audio(channel, start + length):
        if not audio_path.exists():
            abort(503)
        length = max(0, audio_path.stat().st_size - start)
        if length == 0:
            return Response(b"", mimetype="application/octet-stream")

    with open(audio_path, "rb") as f:
        f.seek(start)
        data = f.read(length)

    return Response(data, mimetype="application/octet-stream")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    CACHE_DIR.mkdir(exist_ok=True)

    if not CHANNELS_FILE.exists():
        print(f"No channels.json found — creating example at {CHANNELS_FILE}")
        save_channels({"example": {
            "url":   "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
            "title": "Never Gonna Give You Up - Rick Astley",
        }})

    port     = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    channels = load_channels()
    print(f"Cache directory  : {CACHE_DIR}")
    print(f"HEVC hw decode   : {'yes (/dev/video19)' if HAS_HEVC_HW else 'no'}")
    print(f"Channels         : {list(channels.keys())}")
    print(f"Listening on       http://0.0.0.0:{port}/")

    app.run(host="0.0.0.0", port=port, debug=False)
