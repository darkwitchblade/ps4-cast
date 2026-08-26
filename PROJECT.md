# PS4 Cast — homebrew media casting receiver

Turn a jailbroken PS4 into a media receiver: discover it on the LAN, push a video
to it from a phone/PC on the same Wi-Fi, and play it fullscreen on the TV with
hardware decode.

## Origin / goal (from the request)
> "A casting app where I can find my PS4 as a display, and cast media directly to
> the app over the same Wi-Fi/network — like streaming videos."

Target device: **jailbroken PS4** (homebrew, fake-signed PKG). Build host: macOS
(Apple Silicon), OpenOrbis toolchain.

## Why this is feasible
- **`libSceAvPlayer`** is exposed by OpenOrbis → PS4 *hardware* H.264/H.265 decode
  of MP4 / HLS from a file **or URL**. No ffmpeg software decode needed.
- **`libSceNet`** sockets work in homebrew → we can run an HTTP control server +
  UPnP/DLNA discovery on the console.
- The build host (this Mac) has Docker, git, Homebrew; macOS path for OpenOrbis is
  `brew install llvm` + toolchain installer (no Docker needed).

## Architecture
```
 Phone / PC (same Wi-Fi)                    Jailbroken PS4 (this app)
 ┌───────────────────────┐                  ┌─────────────────────────────┐
 │ Browser web UI         │  HTTP :8080      │ HTTP control server         │
 │  - paste/upload URL    │ ───────────────► │  /play  /stop  /status      │
 │  OR                    │                  │           │                 │
 │ DLNA app (VLC/Bubble)  │  SSDP/UPnP       │ UPnP AVTransport renderer   │
 │  - "Cast to PS4"       │ ───────────────► │           │                 │
 └───────────────────────┘                  │           ▼                 │
                                             │ libSceAvPlayer (HW decode)  │
                                             │   → fullscreen on TV        │
                                             └─────────────────────────────┘
```

## Plan (phased)
- **Phase 0 — Toolchain** *(in progress)*: `brew install llvm`, clone OpenOrbis,
  build a sample → produce a `.pkg`. Proves we can ship an installable package.
- **Phase 1 — MVP cast**: app boots, joins network, draws a "lobby" screen showing
  the PS4 IP + QR code. Runs HTTP server. Web UI lets a phone paste a video URL and
  hit Play → AvPlayer plays it fullscreen. **Deliverable: installable PKG.**
- **Phase 2 — Auto-discovery**: add SSDP + UPnP AVTransport so the PS4 shows up as
  a cast target ("PS4 Cast") in VLC / BubbleUPnP / LocalCast automatically.
- **Phase 3 — Polish**: playback controls (pause/seek/volume), subtitles, queue,
  reconnect handling.

## Known risks / open items
- **Hardware-in-the-loop**: I can build the PKG but cannot test on the console.
  The user installs each build and reports results — iterative.
- **Apple Silicon**: OpenOrbis ships x86_64 binaries → need Rosetta 2; LibOrbisPkg
  is .NET → may need `mono`/`dotnet`. Resolve during Phase 0.
- **AvPlayer URL streaming**: confirm it accepts http(s) URLs directly vs needing a
  local file; if URL-only over HTTP, plain MP4/HLS should work.
- **Need from user**: PS4 firmware + jailbreak stack (GoldHEN version?) and how PKGs
  are installed (Remote PKG Installer / direct). Affects param.sfo SDK target.

## Build environment (working)
- Toolchain root: `oo/OpenOrbis/PS4Toolchain` (release v0.5.4, `toolchain-llvm-18`).
- Compilers (Apple Silicon, pass inline — not persisted to shell):
  - `CC=/opt/homebrew/opt/llvm/bin/clang`
  - `CCX=/opt/homebrew/opt/llvm/bin/clang++`
  - `LD=/opt/homebrew/opt/lld/bin/ld.lld`  (lld is a SEPARATE brew formula)
- Fixups applied: symlinked `bin/macos/create-fself` → `create-fself-macos`.
- PkgTool.Core is x86_64 .NET, runs via Rosetta 2 — produces fake-signed `.pkg`.
- Target: GoldHEN 11.00; PKG installer on PS4 not set up yet (Phase 4 / task #4).
- Build helper: `build.sh` (wraps make with the inline compiler overrides).

## History

The full build-by-build status log lives in [docs/history.md](docs/history.md).
Current engineering decisions and on-device test history: [STEERING.md](STEERING.md).
