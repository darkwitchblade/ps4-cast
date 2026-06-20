# PS4 Cast

Turn a **jailbroken PS4** into a media receiver. Launch the app, then from any
phone or PC on the same Wi-Fi open the URL shown on the TV and cast a video link
to it — played fullscreen with the PS4's hardware decoder (`libSceAvPlayer`).

> Homebrew for your own jailbroken console. Fake-signed PKG — it will **not**
> install on a stock/retail PS4.

## Install the PKG (GoldHEN 11.00)

The build output is **`dist/PS4-Cast-v0.1.pkg`**. Two ways to install:

### A) USB (simplest for the first time)
1. Format a USB stick to **exFAT**.
2. Copy `PS4-Cast-v0.1.pkg` onto it.
3. Plug it into the PS4.
4. `Settings → Debug Settings → Game → Package Installer` → install from USB →
   pick the pkg. (On some GoldHEN builds it's a standalone *Package Installer* app.)
5. **PS4 Cast** appears on the home screen.

### B) Over the network (HTTP)
1. On the Mac, from the repo root:
   ```bash
   cd dist && python3 -m http.server 8000
   ```
2. Get the Mac's LAN IP (`ipconfig getifaddr en0`).
3. On the PS4 use the **Remote Package Installer** (Debug Settings → Package
   Installer → install from URL) and enter:
   `http://<mac-ip>:8000/PS4-Cast-v0.1.pkg`

## Use it
1. Launch **PS4 Cast**. The TV shows `http://<ps4-ip>:8080`.
2. Open that URL on your phone/PC browser (same network).
3. Paste a **direct** video link — `.mp4` or HLS `.m3u8` — and tap **Cast**.
   (Page URLs like YouTube watch pages won't work; it needs the media file URL.)
4. **Stop** returns to the lobby.

## Build from source
Requires macOS with `brew install llvm lld`. The OpenOrbis toolchain is vendored
under `oo/`. Then:
```bash
./build.sh          # → dist/PS4-Cast-v0.1.pkg
./build.sh clean
```

## Status — v0.1 (MVP)
| Part | State |
|------|-------|
| Boots + lobby screen with cast URL | implemented |
| HTTP control server + phone web UI | implemented |
| URL intake (`/play`, `/stop`, `/status`) | implemented |
| Hardware video playback (AvPlayer → NV12 → framebuffer) | implemented, **needs on-device testing** |
| Audio output | not yet (video-first MVP) |
| DLNA/UPnP auto-discovery ("find PS4 as a display") | planned (Phase 2) |

See [PROJECT.md](PROJECT.md) for architecture, risks, and the phase plan.

### Known things to verify/iterate on hardware
- **Frame pitch**: the blit assumes `pitch == width`. If video looks sheared,
  switch to `sceAvPlayerGetVideoDataEx` and honor the returned pitch.
- **Texture memory type**: frames are decoded into write-combined direct memory
  (type 3). If CPU readback is slow, try WB_ONION.
- **Audio**: add `sceAudioOut` + a `sceAvPlayerGetAudioData` pump.
