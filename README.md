# nurealmschat

A native WebSocket chat client for [realms.pub](https://realms.pub), written in C with an SDL GUI and animated GIF sticker support.

## Requirements

### Common (both SDL2 and SDL3 builds)

- GCC or Clang
- libcurl with OpenSSL
- cJSON

### SDL3 build (default — desktop Linux)

- SDL3
- SDL3_ttf
- SDL3_image

### SDL2 build (Raspberry Pi OS / older distros)

- libsdl2
- libsdl2-ttf
- libsdl2-image (2.6.0 or later for animated GIF support)

---

## Building on Desktop Linux (SDL3)

```sh
# Install dependencies (Arch)
sudo pacman -S sdl3 sdl3-ttf sdl3-image curl cjson

# Install dependencies (Ubuntu 24.04+)
sudo apt install libsdl3-dev libsdl3-ttf-dev libsdl3-image-dev \
                 libcurl4-openssl-dev libcjson-dev build-essential

make
```

---

## Building for Raspberry Pi (SDL2)

SDL3 is not packaged in Raspberry Pi OS. Use `USE_SDL2=1` to build against SDL2 instead — no source changes required, a compatibility shim handles all API differences.

### 1. Install dependencies

```sh
sudo apt update
sudo apt install -y \
    libsdl2-dev \
    libsdl2-ttf-dev \
    libsdl2-image-dev \
    libcurl4-openssl-dev \
    libcjson-dev \
    build-essential
```

> **SDL2_image version:** animated GIF support requires SDL2_image 2.6.0+.
> Raspberry Pi OS Bookworm ships 2.6.x so this is fine. Bullseye ships 2.0.x —
> on Bullseye you would need to build SDL2_image from source or disable GIF animation.

### 2. Build

```sh
make USE_SDL2=1
```

To also disable text selection (useful for a touchscreen kiosk with no keyboard):

```sh
make USE_SDL2=1 TEXT_SELECT=0
```

### 3. Install a font

The app defaults to `/usr/share/fonts/noto/NotoSansMono-Regular.ttf`. On Pi OS:

```sh
sudo apt install fonts-noto
```

If the font is somewhere else, edit `FONT_PATH` in `src/gui.c`.

---

## Build flags

| Flag | Default | Description |
|------|---------|-------------|
| `USE_SDL2=1` | 0 | Build against SDL2 instead of SDL3 |
| `TEXT_SELECT=0` | 1 | Disable click-to-select / Ctrl+C for messages |

---

## Configuration

### `.env` (required)

Create a `.env` file in the same directory as the binary:

```env
WS_URL=wss://realms.pub/cable
REALM_ID=your-realm-name
API_KEY=your_api_key_here
```

`BASE_URL` is derived from `WS_URL` automatically (`wss://host` → `https://host`).
Set it explicitly if your setup differs:

```env
BASE_URL=https://realms.pub
```

### `nurealmschat.conf` (optional)

```ini
# Image cache location (default: ~/.cache/nurealmschat)
# cache_dir=~/.cache/nurealmschat

# Max sticker display size in pixels
sticker_size=64

# Max avatar display size in pixels
avatar_size=32

# Max messages shown at once (1–100)
max_messages=20

# Set to false to skip downloading stickers/avatars
# download_missing_stickers=true
# download_missing_avatars=false
```

---

## Running

```sh
./nurealmschat

# Explicit paths
./nurealmschat --env /path/to/.env --config /path/to/nurealmschat.conf

# Terminal-only mode (no SDL window)
./nurealmschat --console
```

The app connects and sends auth + join automatically on startup.

---

## GUI controls

| Control | Action |
|---------|--------|
| **Auth** button | Re-authenticate |
| **Join Room** button | Join the realm from `.env` |
| **Get Msgs** button | Fetch message history |
| **Participants** button | List users in the room |
| Type + **Enter** | Send a message |
| **Ctrl+V** | Paste into the input field |
| Click a message | Select it (teal highlight) |
| **Ctrl+C** | Copy selected message to clipboard |
| **Escape** | Clear input / deselect message |
| Scroll wheel | Scroll through history |
| **Home** / **End** | Jump to oldest / newest message |

---

## Pre-caching stickers

If you have a `stickers.json` export from the server, you can pre-populate the image cache before running the app:

```sh
pip install requests Pillow
python3 precache_stickers.py
python3 precache_stickers.py --dry-run   # preview without downloading
python3 precache_stickers.py --force     # re-download everything
```

The script reads `.env` for the host URL and `nurealmschat.conf` for the cache path and sticker size.
