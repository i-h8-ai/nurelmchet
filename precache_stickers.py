#!/usr/bin/env python3
"""
Pre-cache all stickers from stickers.json into the nurealmschat image cache.

Reads:
  nurealmschat.conf  -> cache_dir, sticker_size
  .env               -> host_url (or BASE_URL, or derived from WS_URL), API_KEY

Cache layout matches the C app:
  GIF  -> {cache_dir}/stickers/raw/{name}.gif      (C app animates directly)
  PNG/JPG/WebP -> {cache_dir}/stickers/raw/{name}{ext}
               + {cache_dir}/stickers/{name}_{size}.png  (C app skips download)

Usage:
  python3 precache_stickers.py [--dry-run] [--force] [--delay SECS]
"""

import argparse
import io
import json
import os
import sys
import time
from pathlib import Path

try:
    import requests
except ImportError:
    sys.exit("Missing dependency: pip install requests")

try:
    from PIL import Image
except ImportError:
    sys.exit("Missing dependency: pip install Pillow")


# ── Config / env parsers ──────────────────────────────────────────────────────

def parse_conf(path):
    cfg = {
        "cache_dir": os.path.expanduser("~/.cache/nurealmschat"),
        "sticker_size": 120,
    }
    try:
        with open(path) as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, val = line.partition("=")
                key, val = key.strip(), val.strip()
                if key == "cache_dir":
                    cfg["cache_dir"] = os.path.expanduser(val)
                elif key == "sticker_size":
                    try:
                        cfg["sticker_size"] = max(16, int(val))
                    except ValueError:
                        pass
    except FileNotFoundError:
        print(f"  (conf not found at {path}, using defaults)")
    return cfg


def parse_env(path):
    env = {}
    try:
        with open(path) as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, val = line.partition("=")
                env[key.strip()] = val.strip()
    except FileNotFoundError:
        print(f"  (env not found at {path})")
    return env


def derive_host(env):
    for key in ("host_url", "BASE_URL"):
        v = env.get(key, "").rstrip("/")
        if v:
            return v
    ws = env.get("WS_URL", "")
    if ws.startswith("wss://"):
        return "https://" + ws[6:].split("/")[0]
    if ws.startswith("ws://"):
        return "http://" + ws[5:].split("/")[0]
    return ""


# ── Image scaling ─────────────────────────────────────────────────────────────

def scale_and_save(data: bytes, sized_path: Path, size: int):
    img = Image.open(io.BytesIO(data))
    img = img.convert("RGBA")
    img.thumbnail((size, size), Image.LANCZOS)
    img.save(sized_path, "PNG")


def gif_frame_count(data: bytes) -> int:
    try:
        gif = Image.open(io.BytesIO(data))
        return getattr(gif, "n_frames", 1)
    except Exception:
        return 1


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="Pre-cache nurealmschat stickers")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print what would be downloaded without doing it")
    ap.add_argument("--force", action="store_true",
                    help="Re-download even if file already exists")
    ap.add_argument("--delay", type=float, default=0.5,
                    help="Seconds between requests (default: 0.5)")
    ap.add_argument("--conf",    default="nurealmschat.conf")
    ap.add_argument("--env",     default=".env")
    ap.add_argument("--stickers", default="stickers.json")
    args = ap.parse_args()

    here = Path(__file__).parent

    cfg       = parse_conf(here / args.conf)
    env       = parse_env(here / args.env)
    host_url  = derive_host(env)
    api_key   = env.get("API_KEY", "")
    cache_dir = Path(cfg["cache_dir"])
    sz        = cfg["sticker_size"]

    if not host_url:
        sys.exit("ERROR: cannot determine host URL — set host_url= in .env")

    with open(here / args.stickers) as f:
        blob = json.load(f)
    stickers = blob.get("stickers", blob) if isinstance(blob, dict) else blob
    total = len(stickers)

    raw_dir = cache_dir / "stickers" / "raw"
    if not args.dry_run:
        raw_dir.mkdir(parents=True, exist_ok=True)
        (cache_dir / "stickers").mkdir(parents=True, exist_ok=True)

    print(f"Stickers  : {total}")
    print(f"Cache dir : {cache_dir}")
    print(f"Size      : {sz}px")
    print(f"Host      : {host_url}")
    print(f"Delay     : {args.delay}s")
    if args.dry_run:
        print("DRY RUN — nothing will be written")
    print()

    session = requests.Session()
    session.headers["User-Agent"] = "nurealmschat-precache/1.0"
    if api_key:
        session.headers["X-Api-Key"] = api_key
        session.headers["Authorization"] = f"Bearer {api_key}"

    n_ok = n_skip = n_fail = 0
    first_request = True

    for i, sticker in enumerate(stickers):
        name      = sticker.get("name", "").strip()
        file_path = sticker.get("filePath", "").strip()
        if not name or not file_path:
            n_fail += 1
            print(f"[{i+1}/{total}] SKIP (missing name or filePath)")
            continue

        ext      = os.path.splitext(file_path)[1].lower() or ".bin"
        is_gif   = ext == ".gif"
        is_video = ext in (".webm", ".mp4")

        raw_path   = raw_dir / f"{name}{ext}"
        sized_path = cache_dir / "stickers" / f"{name}_{sz}.png"

        # Determine if we already have everything we need
        if not args.force:
            if is_gif and raw_path.exists():
                n_skip += 1
                print(f"[{i+1}/{total}] skip  {name}{ext}")
                continue
            if not is_gif and not is_video and sized_path.exists():
                n_skip += 1
                print(f"[{i+1}/{total}] skip  {name}{ext}")
                continue

        if is_video:
            n_skip += 1
            print(f"[{i+1}/{total}] skip  {name}{ext} (video, not supported)")
            continue

        url = host_url + file_path
        print(f"[{i+1}/{total}] fetch {name}{ext} ...", end=" ", flush=True)

        if args.dry_run:
            print(f"(dry) {url}")
            continue

        # Rate limit: sleep before every request except the first
        if not first_request:
            time.sleep(args.delay)
        first_request = False

        try:
            resp = session.get(url, timeout=30)
            resp.raise_for_status()
            data = resp.content

            raw_path.write_bytes(data)

            if is_gif:
                frames = gif_frame_count(data)
                print(f"ok ({len(data)//1024}KB, {frames} frame{'s' if frames != 1 else ''})")
            else:
                scale_and_save(data, sized_path, sz)
                print(f"ok ({len(data)//1024}KB -> {sz}px PNG)")

            n_ok += 1

        except requests.HTTPError as e:
            n_fail += 1
            print(f"FAIL HTTP {e.response.status_code}")
        except Exception as e:
            n_fail += 1
            print(f"FAIL {e}")

    print()
    eta = (n_ok + n_fail) * args.delay
    print(f"Done: {n_ok} downloaded, {n_skip} skipped, {n_fail} failed")
    if n_ok + n_fail:
        print(f"Time spent on delays: {eta:.0f}s")


if __name__ == "__main__":
    main()
