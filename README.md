# PS4 Cast

Turn a **jailbroken PS4** into a media receiver. Launch the app, then from any
phone or PC on the same Wi-Fi open the URL shown on the TV and cast a video link
or upload a local video. Eligible H.264 plays through `sceVideodec2`; FFmpeg
handles demux, audio, networking, and software fallback.

> Homebrew for your own jailbroken console. Fake-signed PKG — it will **not**
> install on a stock/retail PS4.

## Install the PKG (GoldHEN 11.00)

The build output is **`dist/PS4-Cast-v<version>.pkg`**. Two ways to install:

### A) USB (simplest for the first time)
1. Format a USB stick to **exFAT**.
2. Copy the latest `PS4-Cast-v<version>.pkg` onto it.
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
   `http://<mac-ip>:8000/PS4-Cast-v<version>.pkg`

## Use it
1. Launch **PS4 Cast**. The TV shows `http://<ps4-ip>:8080`.
2. Open that URL on your phone/PC browser (same network).
3. Paste a **direct** video link (`.mp4`, `.mkv`, or HLS `.m3u8`) and tap
   **Cast**, or choose **Choose a local file** to upload and play a video from
   that phone or computer. Page URLs such as YouTube watch pages are not media
   URLs and will not work.
4. Uploaded videos use PS4 internal storage. The web UI shows the saved file and
   its size; use the trash button there to remove it. A new upload replaces the
   previous one, so PS4 Cast never accumulates an upload library silently.
5. For browser casting, open **Settings → Phone browser helper** once. It creates
   an Android bookmarklet or an iPhone Safari Shortcut script tied to the current
   PS4 address. Detected videos always show a confirmation before casting.
6. The TV and web interfaces have two modes: **Cast receiver** for direct links,
   uploads and DLNA, and **Live TV** for M3U playlists, bouquets and channels.
   The receiver keeps listening in both modes.
7. During IPTV playback, **Down** opens the channel guide, **L1/R1** changes
   channel and **L2/R2** changes bouquet. During cast playback, Left/Right and
   L1/R1 seek instead. Cross pauses, Circle stops and Triangle exits.

### Cast directly from Chrome

The optional unpacked extension in [`chrome-extension/`](chrome-extension/)
detects non-DRM video requests made by playing tabs, including requests from
third-party player iframes. It shows a toolbar badge and an optional in-player
**Cast to PS4** control, then sends the selected source directly to the receiver.

Open `chrome://extensions`, enable **Developer mode**, choose **Load unpacked**,
and select the `chrome-extension` directory. Enter the PS4 address once in the
extension popup. The permission covers all HTTP(S) pages because cross-origin
iframe detection requires it; candidates remain in memory and cookies or
authorization headers are never sent to the console.

## Build from source
Requires macOS with `brew install llvm lld`. The OpenOrbis toolchain is fetched
by `scripts/fetch-toolchain.sh` (cached in `../ps4-cast-artifacts/`). Then:
```bash
./build.sh          # -> dist/PS4-Cast-v<version>.pkg
./build.sh clean
```

Pure-logic regression tests run on the Mac alone (HLS parsing, URL options,
page scraping): `make -C tests/host test`. Cut a release with
`scripts/release.sh <version>` — bumps `app/Makefile`, builds, and appends the
pkg SHA-256 to STEERING.md.

For the full development loop, keep GoldHEN's payload server armed and run:

```bash
PS4_IP=192.168.1.4 scripts/setup-remote-launch.sh  # one-time pairing
PS4_IP=192.168.1.4 scripts/dev-deploy.sh
PS4_IP=192.168.1.4 scripts/dev-deploy.sh test       # deploy + playback matrix
```

It builds, closes the old process, uninstalls it, installs through DPI, waits
for AppInstUtil's real `progress=100` ready state, launches PS4 Cast through an
authenticated Chiaki Remote Play session, and verifies `/status` reports the
exact new version and a non-anonymous user. Pair Chiaki once with the PS4's
**Remote Play Connection Settings -> Add Device** PIN; its registration stays
in the current macOS user's Chiaki preferences. The pinned, signature-checked
Chiaki devtool is installed under Git-ignored `.devtools/`. If GoldHEN is armed,
setup can read the active console Account ID and copy its Chiaki base64 form to
the clipboard without using a PSN password. Use
`scripts/dev-deploy.sh nobuild` to deploy an already-built package. Set
`HOST_IP` only when the Mac's active LAN address cannot be detected automatically.
Use `test-nobuild` to deploy the existing package and run the same playback and
transport-control matrix without rebuilding it.
The injected GoldHEN launch payload and Second Screen are deliberately not used:
the former lacks an authenticated user session on FW 11 and can crash ShellUI,
while Second Screen requires a PSN-backed mobile identity unavailable in this
offline homebrew setup.

## Status
| Part | State |
|------|-------|
| Boots + lobby screen with cast URL | implemented |
| HTTP control server + phone web UI | implemented |
| URL and local-file playback | implemented |
| Hardware H.264 (`sceVideodec2` -> NV12 -> framebuffer) | implemented with software fallback |
| Audio, A/V sync, pause, seek, and stop | implemented |
| DLNA/UPnP renderer discovery and controls | implemented |

See [PROJECT.md](PROJECT.md) for architecture, risks, and the phase plan.

Current engineering notes and on-device test history live in
[STEERING.md](STEERING.md).
