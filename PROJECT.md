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

## Status log
- 2026-06-18: Project created; architecture set.
- 2026-06-18: Phase 0 COMPLETE — toolchain set up, hello_world built to a valid
  6.3MB fake-signed .pkg. AvPlayer + net stubs confirmed present.
- 2026-06-18: Phase 1 MVP BUILT — app/ source (gfx, netutil, httpd, player,
  avplayer ABI, web UI, main) compiles + links + packages to
  dist/PS4-Cast-v0.1.pkg via ./build.sh. Lobby + HTTP server + URL intake done.
  Video path (AvPlayer NV12 blit) implemented but UNVERIFIED on hardware — needs
  the user to install on the GoldHEN 11.00 console and report. Audio + DLNA pending.

- 2026-06-18: On-device iteration. v0.1 installed/booted/web-UI OK. Crash on play.
  Added per-stage system-toast diagnostics (notify.c) + bundled local test clip
  (assets/test.mp4) to bisect network vs decoder. Crash localized to
  `sceAvPlayerAddSource` for BOTH local and URL sources → not networking.
  ROOT CAUSE (v0.4): `SceAvPlayerHandle` was declared `int32_t`; the real PS4
  handle is a 64-bit pointer (confirmed via shadPS4 `using AvPlayerHandle =
  AvPlayer*` and Force67/prosperity `int64_t handle`). Init returned a truncated
  handle (looked non-negative), addSource dereferenced it → crash. Fixed to
  int64_t; handle-valid check is now `> 0`. Awaiting on-device retest.

- 2026-06-18: More on-device iteration via a live on-screen debug overlay
  (player_debug). At v01.07 the line read `active=0 vget=0 aud=0 vcalls=3000+`:
  render loop fine, but AvPlayer demuxed NOTHING (no audio or video). Root cause:
  in homebrew, AvPlayer's internal file reader does not service /app0 paths.
  Fix (v01.08): provide fileReplacement callbacks (open/close/readOffset/size)
  backed by sceKernelOpen/Pread/Lseek/Close — the app feeds bytes to the decoder.
  Same hook will drive HTTP range streaming for URL sources (next).
  Other confirmed ABI fixes this session: AvPlayer handle is 64-bit (was int32 →
  truncation crashed AddSource); GetVideoData/GetAudioData/IsActive return bool.
  Bisection earlier proved decoder-vs-network via a bundled assets/test.mp4 clip.

- 2026-06-18: v01.24 baseline from Claude — package is signed with privileged
  `--paid 0x3800000000000035` and the matching auth-info blob used by Itemzflow /
  OpenOrbis piglet, fixing the missing authority that caused `jb.prx` load to fail
  with ENOEXEC.

- 2026-06-18: v01.25 — keep the local `/app0` fileReplacement callbacks only for
  bundled/local clips, but leave them unset for `http://` and `https://` URLs so
  AvPlayer can use its native libSceHttp/libSceSsl streaming path. This restores
  local-video isolation and adds the intended HTTPS path without importing custom
  resolver/client socket symbols into the player.

- 2026-06-18: v01.26 — boot-crash isolation build. App startup no longer calls
  `jailbreak()` or `player_init()`, so `jb.prx`, decoder sysmodules, HTTP/SSL
  streaming modules, and the 256MB direct-memory arena are touched only after
  pressing Play/Test. Also guards HTTP request polling when the web server did not
  start. If this boots to the lobby, any remaining crash is in playback init rather
  than package load or graphics/network startup.

- 2026-06-18: v01.27 — loader-safe build. Removed static `libSceAvPlayer` and
  `libSceLncUtil` link dependencies; AvPlayer is now loaded and symbol-bound with
  `sceKernelLoadStartModule`/`sceKernelDlsym` only after playback starts. Verified
  the ELF has no load-time `sceAvPlayer*` or `sceLncUtilLaunchApp` imports. If
  this still crashes before the lobby, the remaining boot suspects are the
  privileged auth-info/signing profile or basic graphics/network imports.

- 2026-06-18: v01.29 — graphics-only minimal boot diagnostic. Conservative paid
  `0x3800000000000011`, no auth-info, `BOOT_MINIMAL=1`, and only `main.c` +
  `gfx.c` linked against `libSceVideoOut`. Verified the ELF imports only kernel
  + video-out basics. This package is not a streaming build; it isolates whether
  the title/package can start and draw at all.

- 2026-06-18: v01.30 — advanced from the working unprivileged path. Restored the
  full receiver shell (`BOOT_MINIMAL=0`) with conservative signing, no auth-info,
  no static AvPlayer/LncUtil imports, dynamic AvPlayer only after Play, and a web
  UI handoff lab that can POST raw/custom URI formats to `/launch` for native
  player/browser probing without rebuilding.

- 2026-06-18: v01.31 — added a second native handoff route: `/launchapp` accepts
  `TITLE_ID\noptional-arg` and dynamically loads/binds `libSceLncUtil.sprx` to
  call `sceLncUtilLaunchApp`, keeping LncUtil out of load-time imports. Web UI now
  has editable title-id launch controls defaulting to `CUSA02012`. Hardware-player
  init status now reports the exact AvPlayer sysmodule/bind failure code.

- 2026-06-18: Live test of v01.31 at `192.168.1.253:8080`: web UI reachable.
  `jb.prx` load returns `0x80020008`; ShellUIUtil and LncUtil dynamic PRX loads
  return `0x80020002`; local TEST never reaches file open/decode. v01.32 adds
  `/browser`, a direct `sceSystemServiceLaunchWebBrowser(url)` probe that does not
  dynamically load ShellUIUtil/LncUtil, and preserves the exact player-init status
  instead of overwriting it with the generic `player init failed -1`.

- 2026-06-18: v01.33 — AvPlayer bind test. Conservative signing remains, player
  init remains lazy, but `libSceAvPlayer` is linked statically again and
  `USE_STATIC_AVPLAYER` disables the dynamic `sceKernelLoadStartModule`/dlsym bind
  path. If this boots and the local TEST advances past bind, dynamic binding was
  the blocker. If it crashes on launch, static AvPlayer imports are not safe under
  the conservative profile.

- 2026-06-18: v01.34 — AvPlayer start/decode experiment. Live v01.33 testing
  proved static AvPlayer binding is safe: `PostInit=0`, `AddSource=0`, local file
  callbacks opened/read ~192K from `/app0/assets/test.mp4`, and `/browser`
  returned `0`. The remaining failure was explicit `sceAvPlayerStart()` returning
  `0x806a0002` followed by STOP/no frames. v01.34 sets `autoStart=1` and removes
  the explicit Start call so AddSource can drive playback.

- 2026-06-18: GoldHEN install path fixed. DirectPackageInstaller GUI/CLI v8.3.3
  did not complete the old raw binloader flow against this GoldHEN 11.00 setup:
  it parsed the package, then timed out waiting for the DPI payload callback.
  GoldHEN's live payload server is HTTP on `192.168.1.253:9090`; `GET /payload`
  returns `405`, and a valid payload POST returns `200`. Added
  `scripts/push-goldhen-dpi.py`, which mirrors DPI directly: host the package on
  `:8000`, serve a BGFT JSON manifest on `:9898`, patch the DPI payload callback
  IP/port, POST it to `http://PS4:9090/payload`, then send the metadata packet.
  Successful push command:
  `python3 scripts/push-goldhen-dpi.py --host 192.168.1.139 --ps4 192.168.1.253`.
  Confirmed output: `payload POST -> HTTP 200`, followed by `metadata: PS4
  callback from 192.168.1.253`.

- 2026-06-19: v01.36/v01.37 dev-loop improvements. Added `/quit` and a web UI
  "Close App" button so a running build can terminate cleanly before reinstall
  or relaunch. Added pipeline scripts:
  `scripts/dev-cycle.sh` (build + install + open/wait + test),
  `scripts/close-ps4cast.sh`, `scripts/open-ps4cast.sh`, and
  `scripts/test-ps4cast.sh`. GoldHEN `POST /launch` currently returns `200` for
  several title-id payload shapes but does not actually start `PCST00001`, so
  launch remains best-effort with a manual-open fallback until a working
  launch payload/API is added.

- 2026-06-19: v01.37 playback telemetry build. Local baseline/main/high clips are
  fast-start MP4s (`moov` before `mdat`) but AvPlayer still opens/reads only a
  small amount and STOPs before frames. v01.37 adds file callback telemetry
  (`file size`, last read offset/length/return) to `/status` so the next run can
  distinguish bad callback reads from decoder/init rejection.

- 2026-06-19: v01.39 lifecycle automation complete. Added
  `payloads/ps4cast-control`, a repo-owned GoldHEN HTTP payload set for
  `launch`, `kill`, and `uninstall`, built with Homebrew LLVM as
  `x86_64-pc-freebsd12-elf`. Critical payload detail: the linker script must put
  `_start`/`.text` at byte zero; otherwise GoldHEN accepts the payload but it
  starts on `.rodata` and silently does nothing. Verified on console:
  `scripts/close-ps4cast.sh` kills the running app (`rv=0`, port 8080 drops),
  `scripts/delete-ps4cast.sh` sends uninstall, DPI reinstall fetches the manifest,
  and `scripts/open-ps4cast.sh 01.39` launches the app (`rv=0x6000600C`, receiver
  returns `/status`). The dev loop is now build -> payload-close -> payload-delete
  -> DPI install -> payload-open -> HTTP test, with no manual controller step.
  Current playback result after the full cycle is unchanged: TESTA/TESTB/etc open
  and read media bytes, then AvPlayer STOPs before frames; explicit Start still
  returns `0x806a0002`.

- 2026-06-19: Privileged authinfo experiment. v01.40 restored the Piglet
  `PAID=0x3800000000000035` + authinfo blob on top of lazy init, but launching it
  triggered PS4 system software error `CE-36329-3` before `/status`. v01.41 kept
  the same privileged signing but removed static `libSceAvPlayer` load-time
  imports (`sceAvPlayer*` no longer in the ELF import set); it still failed to
  boot and the console showed `CE-34878-0`. v01.42 reverted only the signing
  profile to conservative `PAID=0x3800000000000011` with no authinfo, keeping
  dynamic/lazy AvPlayer, and booted successfully. Conclusion: the Piglet authinfo
  profile itself is incompatible with the current packaged title shape; decoder
  privilege needs a different route than simply reapplying that authinfo.

- 2026-06-19: Runtime process-patch probe is unsafe. Added experimental
  GoldHEN payload modes to scan kernel `struct proc` entries for the running
  `PCST00001` process before patching creds. The broad proc scan failed to find
  PS4 Cast (`lnc_appid=0`, big app id present, no proc match) and the console
  later showed `CE-36329-3`. Treat `build/ps4cast-probe.bin` and
  `build/ps4cast-patch.bin` as unsafe; the default payload Makefile no longer
  builds them. Do not run proc-scan/patch payloads again without a much narrower
  known PID/proc address strategy.

- 2026-06-19: Browser handoff is unsafe. v01.43 booted and exposed DLNA
  description/AVTransport endpoints, but calling
  `sceSystemServiceLaunchWebBrowser(http://...)` for native playback triggered
  `CE-36329-3`. v01.44 disables automatic browser handoff from `/play` and the
  `/browser` endpoint. Do not use direct browser handoff as the native playback
  route; investigate Media Player-specific launch/DLNA server handoff instead.

- 2026-06-19: In-app DLNA branch is unsafe. v01.44/v01.45 still produced delayed
  crashes after launch, even after automatic browser handoff was disabled and
  then the in-app SSDP listener was disabled. Recovered by reinstalling the last
  known stable package, `dist/PS4-Cast-v01.42.pkg`, without rebuilding. Do not
  continue the in-app DLNA/SSDP/browser-handoff branch until the delayed crash is
  isolated offline. Next Media Player experiment should run an external Mac DLNA
  server and native Sony Media Player, with PS4 Cast not involved in playback.

- 2026-06-19: Native Media Player / DLNA handoff investigation. Confirmed over
  GoldHEN FTP that the PS4 native Media Player title id is `CUSA02012`
  (`param.sfo` title "Media Player"). `sceAppInstUtilAppInstallMediaPlayer()`
  returns `0`, and `/user/app/CUSA02012/app.pkg` plus
  `/mnt/sandbox/CUSA02012_000/app0/eboot.bin` exist. External payload launch
  returns `0x60000201`, but a follow-up app-status payload still reports
  `lnc_appid=0` for `CUSA02012` and a different foreground big app, so
  foregrounding/visibility is not solved yet. In-app `/launch` and `/launchapp`
  from v01.42 still fail with `0x80020002` because the unprivileged app cannot
  load/bind LNC/Shell UI PRXs. GoldHEN `:9090` later stopped accepting payloads;
  FTP `:2121` remained reachable. Added `scripts/install-media-player.sh`,
  `scripts/launch-media-player.sh`, `scripts/app-status.sh`, and a reusable
  ReadyMedia backend (`scripts/run-minidlna.sh` +
  `scripts/minidlna-ps4cast.conf`). ReadyMedia scans `app/assets` and is the
  preferred DMS backend for native Media Player tests; the hand-written
  `scripts/dlna-server.py` is useful for protocol experiments but got stuck on
  Sony's `GetSortCapabilities` loop.

- 2026-06-19: Native Media Player package references. OrbisPatches lists
  `CUSA02012` with content id `IP9100-CUSA02012_00-PS4MEDIAPLAYER00`. PKG-Zone
  serves `PS4_CUSA02012_v4.01.pkg` (~16 MB) for this title; local inspection of
  its `PARAM.SFO` shows `TITLE_ID=CUSA02012`, `CONTENT_ID` matching OrbisPatches,
  `CATEGORY=gde`, `APP_VER=01.00`, `VERSION=01.00`, and
  `SYSTEM_VER=0x04500000`. Use it as a native Media Player restore/install aid
  for handoff tests. It does not imply PS4 Cast can inherit Sony Media Player's
  hardware-decoder entitlement.

- 2026-06-19: Source fix for close automation. `/quit` previously only stopped
  playback and left the app running; the source now exits the main loop on
  `/quit`. This is not present in installed v01.42 until rebuilt/deployed.

- 2026-06-19: Lapy/Unity video-player reference. After installing Homebrew Store
  video/player apps, `LAPY20002` was mounted at
  `/mnt/sandbox/LAPY20002_000/app0`. Its app contains a large Unity
  `eboot.bin`, `Media/Managed`, `Media/Modules`, and `Media/Plugins`.
  Static inspection shows `UnityEngine.VideoModule.dll`,
  `UnityEngine.UnityWebRequestModule.dll`, `UnityEngine.PS4.PS4VideoPlayer`,
  `UnityEngine.Video.VideoPlayer::set_url`, and script strings such as
  `PS4VideoPlaybackSample`, `PS4VideoPlayer`, `moviePath`, `PlayVideo`,
  `VideoPlayPause`, `VideoFastForward`, and `VideoRewind`. This is the first
  strong evidence of a working non-native-code route: use Unity's PS4 video
  backend/video layer instead of binding `libSceAvPlayer` manually. It may
  already support URL playback via `VideoPlayer.url`; if not, the likely fix is
  to build a Unity PS4 receiver that maps incoming HTTP cast requests to
  `VideoPlayer.url`. Attempted payload launch of `LAPY20002` was blocked because
  GoldHEN `:9090` refused the POST again.

- 2026-06-19: Lapy source review. Cloned
  `https://github.com/Lapy055/PS4Player` to `/private/tmp/PS4Player`. The repo is
  a Unity 2017.2.0p2 PS4 project. `PS4VideoPlaybackSample.cs` uses
  `UnityEngine.PS4.PS4VideoPlayer`, creates `PS4ImageStream` luma/chroma
  textures, calls `video.Init(lumaTex, chromaTex)`, pumps `video.Update()` every
  frame, and plays with `video.Play(moviePath, isLooping)`. `Controlador.cs`
  enumerates `/usb0` via `Directory.GetFileSystemEntries`, handles `.mp4`/`.mov`
  file selection, and calls `PlayVideo()`. The helper `CheckDimensions(string
  url)` uses `UnityEngine.Video.VideoPlayer` with `VideoSource.Url` and
  `videoPlayer.url = url`, proving URL-capable Unity video APIs are registered.
  `Assets/Plugins/PS4/universal.prx` exports `FreeMountUsb`/unjail/FTP helpers
  and imports USB/net/sysmodule APIs, but not the main video decoder route. Best
  next architecture: fork/reuse this Unity project and replace USB browsing with
  a small HTTP control server that sets `moviePath` to the posted URL, or test
  whether `PS4VideoPlayer.Play()` accepts HTTP URLs directly; otherwise download
  or proxy URLs to a local file path before calling `Play`.

- 2026-06-19: pPlay planning reference. Cloned
  `https://github.com/Cpasjuste/pplay` to `/private/tmp/pplay` with submodules.
  pPlay is C++ and uses MPV/ffmpeg (`src/player/mpv.*`; `Player::load()` calls
  `mpv->load(path, Replace, "pause=yes,speed=1")`). Its README explicitly says
  it supports HTTP/FTP streaming and config-driven network roots via
  `pplay.cfg` (`NETWORK = "http://..."` or `ftp://...`). Build files include a
  PS4 path (`PS4_PKG_TITLE_ID "PPLA00001"`) and ffmpeg is configured with
  `file,http,ftp` protocols. This is likely the fastest route to a working cast
  receiver: fork pPlay, keep its MPV/ffmpeg player and UI, add a tiny HTTP
  receiver endpoint that calls `Player::load(url)`, then add SSDP/DIAL
  discovery. Tradeoff: playback is MPV/ffmpeg based, so it may be more CPU-bound
  than Unity's `PS4VideoPlayer`/native video layer.

- 2026-06-19: Revised strategy after comparing DLNA, pPlay, Unity/Lapy, and raw
  AvPlayer. Treat Unity/Lapy as a reference, not a primary implementation path,
  unless we already have the licensed Unity PS4 build module; modifying/rebuilding
  a Unity PS4 project is likely blocked by Sony/Unity licensing. Promote two real
  tracks:
  1. DLNA -> native Media Player: fastest reliability test and best hardware
     decode, but less "push/cast" unless paired with a controller/server layer.
  2. pPlay fork: best route to "our app" because HTTP/FTP streaming and controls
     already exist; add `/play`, `/stop`, `/pause`, `/seek`, `/status` endpoints
     and discovery.
  Keep raw OpenOrbis AvPlayer only as research/fallback; it should not be the
  main product path.

- 2026-06-19: v01.53 — discovery + URL streaming pass (two reported blockers).
  (1) DISCOVERY: re-enabled the in-app SSDP responder (was disabled since the
  01.50/01.51 builds destabilized boot). Root causes fixed in `ssdp.c`: the
  socket never joined the `239.255.255.250` multicast group (so it never
  received M-SEARCH — the PS4 stayed invisible to cast apps), and the receive
  loop busy-spun on error. Added `IP_ADD_MEMBERSHIP` join + multicast TTL
  (OrbisNet IP option numbers 12/10, hand-defined — not in the OO headers), a
  startup `ssdp:alive` NOTIFY burst so passive control points list the PS4
  without re-scanning, and a `usleep` backoff on the recv error path.
  (2) "BAD SOURCE" on a video link: `sceAvPlayerAddSource` rejects http(s) URLs
  because AvPlayer's native network reader doesn't work in homebrew (same reason
  /app0 needs fileReplacement). Added `httpsrc.{c,h}`, an app-managed ranged
  HTTP/1.1 reader (raw OrbisNet sockets, dotted-IP or resolver hostnames, plain
  http only — no TLS), wired into player.c's fileReplacement callbacks for
  `http://` sources so the app fetches the bytes and feeds the decoder. `https://`
  now returns a clear "use an http:// link" status instead of "bad source", and
  the AddSource failure path now reports the real rc + reader state. NOTE: this
  fixes URL *ingestion*; the decoder-never-produces-frames wall (local clips also
  STOP before frames; Start rc `0x806a0002`) is still the open AvPlayer problem —
  http URLs now reach that same stage instead of failing at AddSource. UNVERIFIED
  on hardware; needs an on-device run + `/status` telemetry.

- 2026-06-19: v01.54 — follow-up to the v01.53 on-device test, which reported:
  (a) still no discovery, (b) crash when streaming a LAN http URL, (c) internet
  http URL gave `addsource failed 0x806a0002 [resolve failed (file-examples.com)]`.
  Fixes: DISCOVERY — bind the multicast JOIN + send interface explicitly to the
  console IP (INADDR_ANY did not bind wlan0 on this stack) and added
  `ssdp_status()` to `/status` so we can see on-device whether SSDP started and
  whether the group join/IF set returned 0. DNS — the resolver needs a SceNet
  memory pool; `net_init` never made one, so `sceNetResolverCreate(memid=0)`
  failed. httpsrc now `sceNetPoolCreate`s a pool and uses it (+8s/3-retry
  resolve), which should fix hostname http URLs. ROBUSTNESS — added 8s
  send/recv socket timeouts and moved the HTTP reader's large scratch buffers to
  static storage to cut AvPlayer I/O-thread stack pressure (candidate cause of
  the LAN-http crash, vs. the decoder itself). OPEN QUESTION unchanged: whether
  raw AvPlayer can decode at all under the conservative unprivileged profile —
  every path still stalls/crashes at decode. Next on-device run should report
  `/status` `ssdp` + `debug` fields; if the LAN-http clip still crashes AFTER the
  "http opened, N bytes" toast, the crash is in the decoder, not the reader, and
  the pPlay-fork / native-Media-Player tracks become the realistic path.

- 2026-06-19: v01.55 — v01.54 on-device results: LAN http no longer crashes
  (reader hardening worked) but "buffers then drops to lobby" = AvPlayer fires
  STOP with zero frames, i.e. the data path now works end-to-end and the decoder
  is the isolated blocker — same wall as bundled /app0 clips. DNS pool fix
  confirmed (internet host resolved; `file-examples.com` returned `probe http
  301`). v01.55 adds http->http redirect following (up to 5 hops) with an
  explicit "redirects to https (unsupported)" message; reads use the final
  resolved target. CONCLUSION FORMING: raw libSceAvPlayer under the conservative
  unprivileged profile decodes nothing on any source; this strongly favors the
  pPlay (ffmpeg software-decode) fork as the real playback path.

- 2026-06-19: v02.00 — ENGINE PIVOT to ffmpeg software decode (decision made
  after live /status proved AvPlayer decodes nothing: seq=[1] STOP, 0 frames,
  all init rc=0). libSceAvPlayer's hardware decoder needs an entitlement the
  conservative unprivileged profile doesn't grant, and the privileged profile
  won't boot. ffmpeg decodes on the CPU — no entitlement needed.
  Done this build:
  * Cross-compiled ffmpeg 6.1.1 for OpenOrbis (`portlibs/build-ffmpeg.sh`):
    x86_64-pc-freebsd12-elf via Homebrew clang/lld, `--disable-network`
    `--disable-asm`, minimal decoders/demuxers, static libs in
    `portlibs/ffmpeg/lib`. Only one compat header needed (`sys/sysctl.h`).
    KEY DESIGN: ffmpeg networking is OFF because OpenOrbis has no BSD sockets;
    input is fed via a custom AVIO backed by our sceNet reader (httpsrc), so
    https will live in the reader (TLS), not ffmpeg.
  * `player_ff.c` — drop-in player.h backend: custom AVIO -> httpsrc, demux,
    software-decode video, sws_scale BGRA -> framebuffer, PTS-paced. Audio not
    wired yet (sceAudioOut linked, pending). Selected via `USE_FFMPEG=1` in the
    Makefile (filters out player.c; links the ffmpeg .a's in a --start-group).
  * `compat_ff.c` — sysctl stubs + `sceLibcHeapExtendedAlloc=1`/`sceLibcHeapSize=0`
    so ffmpeg has unbounded heap.
  * New clean mobile-first web UI (single link box, Cast/Stop, live status dot).
  Builds to `dist/PS4-Cast-v02.00.pkg` (~12 MB, ffmpeg statically linked).
  UNVERIFIED on hardware. CRITICAL OPEN QUESTIONS for the next on-device run:
  (1) does it boot + decode at all (runtime libc symbol resolution vs PS4 libc),
  (2) is CPU software decode fast enough (asm is disabled; Jaguar cores are weak
  — expect to need lower-res/720p clips first, then re-enable x86asm via nasm).
  NEXT: https = add a TLS backend (BearSSL/mbedTLS cross-build) + refactor
  httpsrc from stateless-per-read ranged GETs to a persistent streaming
  connection (a TLS handshake per read is untenable); the streaming model also
  speeds up plain http.

- 2026-06-19: v02.00 confirmed on-device — ffmpeg SOFTWARE DECODE WORKS (video
  played; the AvPlayer entitlement wall is gone). Engine pivot validated.
- 2026-06-19: v02.02 — HTTPS support (the priority feature). Approach: TLS lives
  in our reader, not ffmpeg (ffmpeg has no sockets on PS4).
  * Cross-compiled BearSSL 0.6 for OpenOrbis (`portlibs/build-bearssl.sh`,
    libbearssl.a) — chosen for zero platform deps (no time/fs/socket/entropy
    modules) so nothing extra needs resolving against the PS4 libc.
  * `tls.{c,h}` — TLS 1.2 client over an OrbisNet socket. Handshake entropy from
    `sceRandomGetRandomNumber`. Certs are NOT validated (BearSSL x509 "noanchor"
    wrapper) — LAN media caster, user-chosen sources; confidentiality kept,
    authenticity not. malloc'd ctx (BR_SSL_BUFSIZE_BIDI is large).
  * `httpsrc.c` REWRITTEN from stateless-per-read ranged GETs to a persistent
    streaming connection shared by http and https: one ranged GET from the
    offset, then sequential streaming; small forward seeks discard on the open
    connection, larger/backward seeks reconnect (one TLS handshake per seek, not
    per read). Redirects (incl. http->https) followed. Same public API, so
    player_ff.c is unchanged.
  * Linked `-lbearssl -lSceRandom` always; ffmpeg libs only under USE_FFMPEG.
  Builds to `dist/PS4-Cast-v02.02.pkg` (~12 MB). UNVERIFIED on hardware: needs an
  on-device test of an https:// link; watch `/status` debug (`pkts/frames`) and
  the http source line. Known follow-ups: audio output (sceAudioOut), and
  re-enabling x86 asm in ffmpeg for decode speed.

- 2026-06-19: v02.03 — transport controls. player.h gains pause/seek/progress;
  player_ff.c implements them: pause holds + re-blits the last frame and shifts
  the PTS pacing clock by the paused duration on resume; seek sets a pending
  flag applied on the render thread (av_seek_frame BACKWARD + flush + re-anchor
  clock) since the ffmpeg context is single-threaded; current/duration tracked
  per frame. New http endpoints `/pause` (1/0/toggle) and `/seek` (absolute
  seconds); `/status` now reports paused/cur/dur. Web UI gains a transport bar:
  scrub slider, current/total time, play-pause, +/-15s skip, stop. player.c
  (legacy AvPlayer) gets no-op control stubs so USE_FFMPEG=0 still links.
  Builds to `dist/PS4-Cast-v02.03.pkg`.

- 2026-06-19: v02.04 — fix CE-34878-0 crash on https. Root cause: the TLS
  handshake seed used `sceRandomGetRandomNumber`, which `llvm-nm -u` confirmed
  was an UNRESOLVED import in the ELF (libSceRandom does not resolve on this
  GoldHEN/homebrew setup) — calling it on the https path crashed. Fix: derive
  seed entropy in tls.c from `sceKernelGetProcessTime` (re-sampled) + stack/
  pointer addresses via a splitmix64 diffuser; dropped `-lSceRandom`. Verified
  the elf no longer imports sceRandom. (Entropy is weaker than a CSPRNG but
  sufficient for the handshake; session stays encrypted.) http path unchanged.
  Builds to `dist/PS4-Cast-v02.04.pkg`. Diagnostic note for future symbol
  crashes: OpenOrbis ELFs don't fail the link on undefined symbols — check
  `llvm-nm -u build/ps4cast.elf` for non-sce/non-libc imports.

- 2026-06-19: On-device test session (drove the console from the Mac).
  VERIFIED on v02.04: the https CRASH (CE-34878-0) is FIXED — casting an https
  URL no longer crashes; it fails gracefully ("send failed"). So the sceRandom
  removal fixed the reported crash.
  FOUND http regression (frames=0): the streaming httpsrc rewrite (v02.01) broke
  plain-http decode. Root cause (confirmed with a native BearSSL/host test +
  observing the server): python http.server answers `HTTP/1.0 200` and IGNORES
  `Range`, so every seeked read returned byte-0 data -> demuxer parsed structure
  but frame payloads were garbage -> 0 frames, err=0. Also `Content-Length`
  parsed at the wrong offset (sz=0).
  FOUND https "send failed": NOT a code bug — the same tls.c/BearSSL logic
  handshakes fine natively against the same server (even simulating PS4 time=0,
  which still returns NOT_TRUSTED->ignored). So the on-device failure is in the
  sceNet handshake I/O specifically; need the BearSSL last_error code from the
  device to pinpoint it.
- 2026-06-19: v02.05 — fixes from that session. httpsrc: handle `200`-ignores-
  Range by discarding to the requested offset on reconnect (fixes http frames=0);
  fix Content-Length offset (strlen). tls.c: add `tls_last_error`; request_from
  now reports `tls/send failed e=<code>` so the next on-device https test reveals
  the exact BearSSL error. Builds to `dist/PS4-Cast-v02.05.pkg`. NOT YET
  INSTALLED: GoldHEN's :9090 payload server stopped accepting payloads mid-
  session (flaky/single-shot) so DPI install + launch could not be pushed; the
  console must re-arm the payload server (and relaunch PS4 Cast) before v02.05
  can be installed and the http-fix / https-error-code verified.
  Test harness notes: serve clips on the Mac via `python3 -m http.server 8000`
  (http) and a self-signed https server on :8443 (our TLS accepts any cert);
  free :8000 before running push-goldhen-dpi (port clash). Native TLS validation
  harness: build BearSSL with host clang, link tls-style client, GET the https
  server — proves the client logic independent of the PS4.

- 2026-06-19: v02.07 — HTTPS + in-app controls pass. On v02.05 `/status`
  reported `tls/send failed e=53`; BearSSL error 53 is
  `BR_ERR_X509_TIME_UNKNOWN`, caused by the PS4/homebrew build having no usable
  certificate wall clock. `tls.c` now replaces the x509_minimal/noanchor wrapper
  with an accept-all verifier that decodes the end-entity public key and skips
  CA/date/name checks, matching the app's intended "accept local/self-signed
  casting URLs" behavior. `main.c` now initializes libScePad and adds in-app
  playback controls: Cross/Options pause-resume, Left/Right seek +/-10s, L1/R1
  seek +/-60s, Circle stop. Playback draws a bottom HUD with status,
  current/duration, remaining time, and a progress bar. `app/Makefile` links
  `-lScePad`; `httpsrc.h` comment updated for HTTP+HTTPS. Built cleanly to
  `dist/PS4-Cast-v02.07.pkg`; `llvm-nm -u app/build/ps4cast.elf` showed only
  expected libc/SCE imports including `scePad*`. `v02.06` was uploaded to
  `/data/PS4-Cast.pkg` and its install payload returned `HTTP 200`, but the app
  never answered `http://192.168.1.253:8080`. After a defensive TLS edge-case
  fix, `v02.07` was uploaded with `curl --ftp-method nocwd ...`, but GoldHEN
  payload server refused `:9090` for the install/launch payloads. Current PS4
  staging path contains `v02.07`; install/open still needs GoldHEN payload
  server re-armed or manual package install/open. Next verification: manually
  open PS4 Cast 02.07 (or re-arm GoldHEN payload
  server), then test `http://192.168.1.139:8000/browsertest.mp4` and
  `https://192.168.1.139:8443/browsertest.mp4`; expect HTTPS to move past
  `e=53`.

- 2026-06-19: v02.08 — Castify/DLNA discoverability pass. Castify supports
  DLNA receivers, so discovery should target UPnP AVTransport/MediaRenderer, not
  Chromecast emulation. `ssdp.c` now advertises/responds to `uuid`, rootdevice,
  MediaRenderer, ConnectionManager, AVTransport, and RenderingControl; USN
  formatting was fixed for uuid-only advertisements. `httpd.c` device XML now
  includes DLNA DMR identity fields (`dlna:X_DLNADOC`, model metadata,
  presentationURL), and AVTransport/RenderingControl/ConnectionManager SCPDs now
  include normal action argument lists and state variables instead of bare action
  names. SOAP probing improved: Pause no longer stops playback, GetTransportInfo
  reports STOPPED/PLAYING/PAUSED_PLAYBACK, RenderingControl handles GetMute, and
  ConnectionManager returns full GetCurrentConnectionInfo. `ssdp_status()` now
  includes M-SEARCH diagnostics (`seen`, source IP, requested ST) so `/status`
  can prove whether Castify multicast reaches the console. `scripts/ssdp-proxy.py`
  also advertises uuid + ConnectionManager for Castify scan diagnostics; during
  testing it saw Castify/phone `192.168.1.49` search for
  `urn:schemas-upnp-org:device:MediaRenderer:1`, confirming DLNA MediaRenderer
  is the correct discovery target. Built cleanly to `dist/PS4-Cast-v02.08.pkg`.
  Could not deploy: PS4 app was not
  answering `:8080`, GoldHEN `:9090` had been refusing payloads, and GoldHEN FTP
  on `:2121` started failing before greeting (`curl: response reading failed
  errno 36`). Next deployment requires re-arming/restarting GoldHEN services.

- 2026-06-19: v02.09/v02.10 — Castify end-to-end and overlay/input polish.
  v02.09 changed the PS4 renderer UUID to
  `uuid:7b2f63a8-2530-4e47-9f3a-0000000c5701` and changed the Mac diagnostic
  proxy UUID so Castify would not keep showing the cached Mac `192.168.1.139`
  device. User confirmed PS4 appears in Castify. Live `/status` on v02.09 then
  showed successful real online HTTPS playback from Castify:
  `playing 1920x1080`, `frames=119`, `err=0`, `https ... st=206`, duration
  244s. v02.10 is built locally (`dist/PS4-Cast-v02.10.pkg`) with HUD auto-hide
  and broader pad/remote probing: opens standard + special pad ports, shows HUD
  for a few seconds on playback start/input, keeps it visible while paused or
  buffering, maps Cross/Options to pause, Left/Right +/-10s, Up/Down +/-30s,
  L1/R1 +/-60s, Circle stop. TV HDMI-CEC remotes may still be shell-only; if so
  use Castify/web/DS4 controls. Did not install v02.10 during live playback;
  FTP first attempt failed before transfer, and preserving the active playback
  session was safer.

## Source map (app/src)
- main.c        orchestration + lobby screen + render loop
- gfx.{c,h}     sceVideoOut double-buffered framebuffer + 8x8 text (font8x8.h)
- netutil.{c,h} net bring-up + read LAN IP (sceNetCtlGetInfo)
- httpd.{c,h}   threaded HTTP control server; stashes /play URL for main loop
- web_ui.h      embedded phone control page
- player.{c,h}  libSceAvPlayer wrapper + NV12->RGBA blit
- avplayer_abi.h hand-written AvPlayer ABI (OpenOrbis ships empty stubs)
- httpsrc.{c,h} app-managed ranged HTTP/1.1 reader for http:// and https://
- ssdp.{c,h}    UPnP/DLNA discovery responder (M-SEARCH replies + alive NOTIFY)
- launcher.{c,h} native app/browser handoff probes (disabled: CE-36329-3)
- goldhen.{c,h} GoldHEN SDK jailbreak enter/restore around module loads
- escalate.{c,h} jb.prx-based privilege helper (legacy; jb.prx ENOEXEC here)
- notify.{c,h}  system-toast diagnostics
