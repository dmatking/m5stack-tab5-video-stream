#!/usr/bin/env python3
# MJPEG + PCM audio server for M5Stack Tab5 video stream project.
#
# Channels are defined in channels.json:
#   { "channel_name": "https://youtube.com/watch?v=..." }
#
# On first request for a channel, yt-dlp resolves the stream URL and ffmpeg
# extracts JPEG frames + raw PCM in one pass in a background thread.
# /frame and /audio requests are served as soon as the relevant content is
# available on disk — no need to wait for the full video.

import json
import subprocess
import sys
import threading
import time
from pathlib import Path

from flask import Flask, Response, abort

app = Flask(__name__)

BASE_DIR      = Path(__file__).parent
CACHE_DIR     = BASE_DIR / "cache"
CHANNELS_FILE = BASE_DIR / "channels.json"

FPS         = 30
SAMPLE_RATE = 16000       # Hz, mono, unsigned 8-bit PCM
WAIT_TIMEOUT = 20.0       # seconds to wait for a frame/audio chunk to appear
WAIT_POLL    = 0.1        # seconds between polls

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
    # Prefer H.265 on Pi 5 for hardware decode, otherwise take best <= 720p
    if HAS_HEVC_HW:
        candidates = [
            # Require direct HTTPS (no HLS) so hevc_v4l2m2m can decode it
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
        # Skip if both outputs already exist from a previous run
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

        # Build ffmpeg input args
        if hw:
            pre = ["-hwaccel", "drm", "-c:v", "hevc_v4l2m2m"]
        else:
            pre = []

        if len(urls) == 1:
            inputs     = [*pre, "-i", urls[0]]
            video_map  = ["-map", "0:v"]
            audio_map  = ["-map", "0:a"]
        else:
            inputs     = [*pre, "-i", urls[0], "-i", urls[1]]
            video_map  = ["-map", "0:v"]
            audio_map  = ["-map", "1:a"]

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

        if not video_outputs and not audio_outputs:
            print(f"[{channel}] nothing to extract")
        else:
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

def load_channels() -> dict[str, str]:
    if not CHANNELS_FILE.exists():
        return {}
    with open(CHANNELS_FILE) as f:
        return json.load(f)


def _channel_info(channel: str) -> dict:
    frames_dir = CACHE_DIR / channel / "frames"
    audio_path = CACHE_DIR / channel / "audio.raw"
    return {
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
    for ch, url in channels.items():
        _ensure_extracting(ch, url)
    result = {ch: _channel_info(ch) for ch in channels}
    return Response(json.dumps(result, indent=2), mimetype="application/json")


@app.route("/frame/<channel>/<int:ms>")
def frame(channel, ms):
    channels = load_channels()
    if channel not in channels:
        abort(404)

    _ensure_extracting(channel, channels[channel])

    frame_num  = max(1, round(ms * FPS / 1000) + 1)
    frame_path = _wait_for_frame(channel, frame_num)

    if frame_path is None:
        # Frame not yet available — try clamping to last extracted frame
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

    _ensure_extracting(channel, channels[channel])

    audio_path = CACHE_DIR / channel / "audio.raw"

    if not _wait_for_audio(channel, start + length):
        # Serve whatever is available
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
        with open(CHANNELS_FILE, "w") as f:
            json.dump({"example": "https://www.youtube.com/watch?v=dQw4w9WgXcQ"}, f, indent=2)

    port     = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    channels = load_channels()
    print(f"Cache directory  : {CACHE_DIR}")
    print(f"HEVC hw decode   : {'yes (/dev/video19)' if HAS_HEVC_HW else 'no'}")
    print(f"Channels         : {list(channels.keys())}")
    print(f"Listening on       http://0.0.0.0:{port}/")

    app.run(host="0.0.0.0", port=port, debug=False)
