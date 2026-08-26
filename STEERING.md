# PS4 Cast — items needing your steering

Things the autonomous loop can't decide or do alone. Revisit these together.

## Autonomous development deploy

- `scripts/dev-deploy.sh [nobuild|test|test-nobuild]` is the single unattended entry point. It
  builds, closes, uninstalls, sends the DPI install, waits on AppInstUtil's real
  stable `AppExists` state, launches, and verifies the exact `/status` version
  and a non-anonymous initial user. Do not restore progress polling: it crashes
  the GoldHEN host process on firmware 11.
- The `test` modes additionally run `scripts/streamtest/run.sh`: a local fixture
  server plus HW/SW codec/container/HLS routing, unique presented FPS, split
  drop causes, app liveness, pause/resume, and seek checks.
- Run `scripts/setup-remote-launch.sh` once to register the pinned Chiaki-ng
  devtool through **Remote Play Connection Settings -> Add Device**. When
  GoldHEN is armed, setup obtains `sceUserServiceGetNpAccountId` through the
  read-only app-status payload and copies the correctly encoded Account ID to
  the clipboard. Normal deploys open a short authenticated Remote Play session,
  move one tile left after reinstall, send Cross, verify `/status`, then
  disconnect. The left move is required because uninstall shifts selection to
  the neighboring PS Vue tile and reinstall does not move it back.
- The earlier `ps4-waker`/Second Screen plan is superseded. This console's
  offline workflow has no PSN-backed Second Screen phone identity, and a made-up
  64-hex credential is rejected by the PS4 (`LOGIN_FAILED 21/23`). Do not repeat
  that experiment.
- GoldHEN payload launch is unsafe on this FW: UserService is anonymous in the
  injected process, and supplying the last numeric user id still launched the
  title with `iu=0xffffffff`, followed by a ShellUI Invalid User Id crash. It is
  available only with `PS4CAST_UNSAFE_PAYLOAD_LAUNCH=1`. Manual icon launch is
  available with `PS4CAST_ICON_LAUNCH=1`.

## v04.49: hardening, split modules, host tests, pairing token

- Built package: `dist/PS4-Cast-v04.49.pkg`, SHA-256
  `c15f06c3933e60be80be9e7b449696aec9e341bceaf7ec459fb8ea76922cce95` (15M).

- **Pairing token**: the receiver now generates a per-install token (persisted
  in /data, shown on the TV in the lobby URL and QR). Every state-changing HTTP
  endpoint requires it (`?t=` or `X-PS4Cast-Token`); DLNA/UPnP and read-only
  /status, /trace, /crashlog, /token stay open so SSDP clients and the dev
  pipeline keep working. /status exposes the token so local tooling can
  self-provision (scripts/ps4-api.sh). Web UI Settings gains a "Pairing
  required" toggle; the Chrome extension accepts the full TV URL and sends the
  token on cast handoff.
- **Fake-live seek**: live-FLAGGED playlists (movie CDNs that omit
  EXT-X-ENDLIST) were unseekable because hls_can_seek refused and the fallback
  av_seek_frame can never work on the concatenated stream. Seeks now reposition
  by segment index, clamped to the known window.
- **VOD segment resilience + diagnostics**: httpsrc segment-open failures now
  record their reason in /status (`segfail=`) and automatically fall back to
  fetching that segment through aseg (which carries the native SceHttp
  fallback), pinned per stream. This is the fix for the "playlist opens, video
  never starts" failure class seen with a Cloudflare-fronted movie CDN.
- **Module splits**: pure HLS parsing/variant selection moved to
  hls_parse.c (host-testable); the channel store + M3U parsing + /channels and
  /channel/* endpoints moved to httpd_channels.c. httpd.c shrinks to routing,
  DLNA, uploads, and status.
- **Host test harness**: `make -C tests/host test` compiles the pure modules on
  macOS with stubs — 3 suites, fast regression without the console.
- **Repo hygiene**: vendored portlibs sources (11k files) untracked — fetched
  and extracted by portlibs/fetch.sh; OpenOrbis toolchain via
  scripts/fetch-toolchain.sh (tarball cached outside the repo); .DS_Store
  purged; empty root dirs removed.
- **Release ritual**: scripts/release.sh <ver> bumps, builds, and appends the
  pkg SHA-256 to this file.
- **CI**: .github/workflows/build.yml builds the pkg on a macOS runner
  (toolchain + portlibs cached) and runs the host tests on every push.
- **Docs**: the historical status log moved to docs/history.md; PROJECT.md now
  carries only goal/architecture; AUTONOMOUS_PIPELINE.md removed (superseded
  by README's dev-loop section and STEERING).
- Deploy pipeline: push-goldhen-dpi.py closes the running app before burning
  the one-shot GoldHEN :9090 bootstrap and waits for a console-side rearm
  instead of failing; dev-deploy.sh no longer blocks install on a missing
  Chiaki pairing (launch degrades to manual icon tap). scripts/ps4-ui.sh adds
  an authenticated Remote Play UI driver (keys + window screenshots) for
  autonomous console navigation.

## v04.46: scanout safety and resident deployment

- VideoOut now records the last frame submitted by each of the three scanout
  buffers. A surface is reused only after a newer flip is confirmed, so the CPU
  cannot draw into the buffer the display is still scanning. This fixes the
  intermittent partial-frame/flashing UI without giving up triple buffering.
- A resident installer on PS4 TCP `:9192` removes the per-deploy GoldHEN rearm.
  It clears only stale BGFT tasks matching PS4 Cast's exact content ID, searches
  all documented task subtypes, and closes its listener on intentional exit.
- On-device acceptance passed the complete HW/SW/container/HLS matrix, transport
  controls, an autonomous close/reinstall/launch cycle that explicitly reused
  the resident agent, and a final 1080p60 HW run (480 frames, five late drops,
  no queue/reorder drops or crash).
- Built package: `dist/PS4-Cast-v04.46.pkg`, SHA-256
  `503ffe61b2db69b73152e72d79d572922f5fd5b9275e288eb3948a731ea0ed7e`.
- Chrome extension 0.3.0 keeps its control attached to the active video across
  resize, scrolling, zoom and fullscreen changes. It prefers either free outer
  edge before covering the picture, hides for tiny/offscreen media, and changes
  from the cast glyph to a green check only after the PS4 accepts the handoff.
  Pure layout tests cover right, left, constrained-inside and hidden placements.

## v04.44: clean playback transitions

- A new playback now invalidates the cached letterbox/pillarbox state for one
  complete scanout-buffer rotation. Previously, returning to the lobby and then
  opening a source with the same geometry could leave lobby or QR pixels in the
  bars because the video presenter correctly overwrote only the picture rect but
  incorrectly assumed the unchanged bars were already black.
- The clear count is derived from `GFX_BUFFER_COUNT`, keeping the software and
  hardware paths correct if scanout buffering changes again.
- Retains v04.43's early H.264 B-frame reorder floor, regressed-PTS rejection,
  unique-frame FPS telemetry, initialized triple buffers, and authenticated
  development launcher.
- Built package: `dist/PS4-Cast-v04.44.pkg`, SHA-256
  `163054a0d7878365d37a0b5fe9731aec610a668e2c8b574e46b36689edba3bce`.

## v04.43: authenticated launch and visual-order hardening

- The first authenticated-launch implementation used Second Screen. It was
  subsequently replaced by Chiaki Remote Play because Second Screen pairing
  requires a PSN-backed mobile identity. The underlying requirement remains:
  `sceLncUtil` from a GoldHEN payload launched PCST00001 with `iu=0xffffffff`
  even when passed the observed numeric user id, then crashed ShellUI.
- `build.sh` pins self-contained .NET extraction to ignored `.dotnet-bundle/`,
  removing the intermittent `PkgTool.Core.dll does not exist` packaging failure.
- HW H.264 now detects `PTS != DTS` before decode and establishes a four-frame
  reorder floor before any future-PTS picture can escape. The render queue also
  rejects a regressed PTS instead of displaying time backward. Diagnostics split
  these as `drop=...(rN)` and report NV12 conversion average/max as `cv=avg/max`.
- On-screen FPS now counts newly shown source frames, not repeated refresh flips.
  All three scanout buffers are initialized, and flip-status query errors use the
  existing fail-closed path. Triple buffering and HW decode remain enabled.
- Built package: `dist/PS4-Cast-v04.43.pkg`, SHA-256
  `3e3a88ce1e741a09746a2b21dadc40de3e723a473a205b5e3bc7b1808161a802`.
- On-device acceptance: 720p60 TS should report about 60 FPS with `r=0`; any
  remaining drops are now attributable to queue pressure (`q`) or presentation
  lateness (`l`). Re-test the 720p and 1920x960 fMP4 URLs, pause/seek/resume, and
  a five-minute steady playback before declaring the visual issue closed.

## v04.42: HLS startup cushion and split drop telemetry

- HLS now waits for 1000ms of decoded audio before releasing playback (500ms
  for non-HLS). On the 720p60 TS control, the former 250ms gate left the shared
  demux/decode producer hovering at its 400ms audio-protection threshold, where
  it intentionally discarded video frames to reach upcoming audio packets.
- Player diagnostics now report `drop=N(qQ/lL)` and `ro=R`: `q` counts frames
  discarded by full-queue audio protection, `l` counts earlier-due frames
  discarded by the presentation clock, and `ro` exposes the active H.264 reorder
  depth. This makes throughput, audio-pressure, and timestamp-order failures
  distinguishable during live tests.

## v04.41: optimize the application hot paths

- The application sources now compile with `-O2 -fno-strict-aliasing`. Until
  this build, only the prebuilt FFmpeg libraries were optimized; PS4 Cast's
  per-pixel NV12 presenter and all surrounding C code were emitted at the
  compiler default (`-O0`). The aliasing opt-out keeps the optimization change
  conservative for the existing low-level media and platform structs.
- This is intentionally isolated from graphics-layout changes so the 720p60
  control stream can be compared directly with v04.40 (47-48 app fps and about
  40% decoded-frame drops). The already-green 720p and 1920x960 fMP4 tests must
  also stay green before this becomes the new baseline.

## v04.40: fMP4 hardware decode and presentation fast path
- Fixed-variant H.264 fMP4 HLS now keeps the stable continuous AVIO/MOV demuxer,
  applies `h264_mp4toannexb`, and feeds the proven `sceVideodec2` decoder. Normal
  fMP4 fragment boundaries do not reset decoder references; real HLS reset events
  retain the existing full re-anchor. Software remains the failure fallback.
- Hardware-enabled fMP4 masters select their final H.264 rendition up to 1080p
  and 10Mbps before FFmpeg sees the init segment. Software mode keeps the 720p,
  4Mbps cap. fMP4 remains variant-locked because changing MOV init/config state
  under an open demuxer caused persistent frame drops.
- The parallel NV12 presenter now uses precomputed horizontal maps, YUV
  contribution tables, and a direct-width NV12 pair fast path. This removes the
  old per-output-pixel 64-bit divide that limited 1080p and 720p60 presentation.
- Chrome extension 0.2.0 replaces the fixed text overlay with a circular cast
  control attached to the active video. It only appears for an exact-frame
  playable candidate and exposes sending, casting, and error states.

## v04.39: fMP4 variant lock
- v04.38 proved automatic bad-header recovery works, but also proved that
  switching a fragmented-MP4 playlist after FFmpeg has opened its MOV demuxer is
  unsafe: a 480p -> 720p switch kept the old stream configuration and discarded
  roughly one frame in three. Direct 720p remained perfect.
- Master fMP4 playlists now inspect the conservative first rendition, select a
  final H.264 rendition up to 720p/4 Mbps before exposing any bytes to FFmpeg,
  and lock that variant for the session. Midstream ABR remains enabled for TS,
  where segment-boundary switching is safe. Telemetry marks this as `FMP4LOCK`.

## v04.38: exact request context and clean-header recovery
- Live testing proved the `an earlier Cloudflare-fronted test source` URL and chunked transport are
  valid: the same master plays for minutes with full audio/buffer when sent
  without page headers, while the guessed outer-page Referer/Origin produces a
  deterministic 403 from both BearSSL and native SceHttp.
- The Chrome extension no longer invents Referer/Origin from the playing frame's
  outer page. It forwards those values only when captured from the exact media
  request; User-Agent fallback remains.
- PS4 Cast now treats a 403 with page headers as recoverable: it retries once
  without Referer/Origin while preserving UA/Cookie. If that succeeds, the clean
  header policy remains active for every variant and segment in that stream.
  Native SceHttp remains the final host-scoped fallback for a genuine transport
  fingerprint rejection.
- ABR now receives the decoder path's capability ceiling. Proven H.264 hardware
  streams may promote through 1080p; software-decoded HLS stops at 720p. The
  failing fMP4 source measured zero frame drops at fixed 1440x720, while its
  1920x960 software rendition decoded 24 fps but could present only about 17 fps
  and discarded roughly one frame in three despite a full network buffer.

## v04.37: sandbox-safe native HTTP loading
- v04.36 proved the PS4 receives a real CDN 403 and preserves it correctly, but
  retail sandboxing hid direct `/system/common/lib/libSceHttp.sprx` and
  `libSceSsl.sprx` loads with ENOENT. The lazy fallback now loads both through
  `sceSysmoduleLoadModuleInternal`, as OpenOrbis's HTTP sample does, enumerates
  their process module handles, and resolves the API at runtime. This keeps boot
  free of eager HTTP/SSL imports while using the supported sysmodule route.

## v04.36: native HTTPS compatibility fallback
- HTTPS resources that receive a real HTTP 403 through the custom BearSSL path
  are retried once with the PS4's native `SceHttp`/`SceSsl` stack. If that retry
  works, only that CDN host remains on native HTTP; established BearSSL behavior
  stays unchanged for every other source.
- The native modules and memory pools are initialized lazily on the first 403,
  so boot and normal playback do not touch the new path. The fallback preserves
  browser Referer, Origin, User-Agent and Cookie options, follows redirects,
  handles chunked bodies, reuses same-host connections, and remains bounded and
  abortable.
- Starting a new HLS source clears stale HTTP errors. Source-open diagnostics are
  captured before teardown and remain visible in `/status`, including native
  initialization/request failures.

## v04.35: adaptive HLS handoff and startup
- Chrome candidate ranking now strongly prefers a master/multivariant playlist
  over a fixed `1080p` rendition from the same playing frame. The picker labels
  these as **Adaptive master** and **Fixed rendition** so manual selection is
  no longer ambiguous.
- Master playlists start on the highest H.264 rendition up to 2.5 Mbps, then
  promote one step only after three segment downloads prove at least 40% network
  headroom. Rebuffering still requests an immediate one-step downshift.
- Fixed a latent ABR switch bug: stopping the prefetch worker left its shared
  fetch abort set, causing the new variant playlist fetch to fail before the
  worker was restarted. Variant switches now resume the fetcher first.
- Public segment connects get up to 6 seconds while playlist connects retain the
  tighter 2.5 second limit; both remain bounded by the existing whole-fetch
  budget and are immediately abortable during stop/cast.

## v04.34: chunked HLS transport compatibility
- Added bounded HTTP/1.1 chunk decoding to the shared HLS resource fetcher.
  Chunk-size lines, extensions and trailers are now consumed at the transport
  layer instead of leaking into the playlist parser as fake segment URLs.
- The decoder reads across arbitrary TLS/socket boundaries, enforces the existing
  16 MB resource and fetch-time budgets, honors aborts, and fully consumes the
  terminating chunk so same-host keep-alive remains safe.
- This directly addresses the `an earlier Cloudflare-fronted test source` failure from the Chrome
  extension: its media playlist is served without Content-Length using
  `Transfer-Encoding: chunked`, and v04.33 tried to fetch the hexadecimal chunk
  size (`dff9`) as segment zero.

## v04.33: Chrome media capture companion
- Added an unpacked Manifest V3 extension under `chrome-extension/`. It observes
  non-DRM media requests from every Chrome frame, correlates candidates with the
  frame that is actively playing, and offers both a toolbar picker and an
  optional in-player **Cast to PS4** button.
- Candidate ranking prefers HLS and the exact playing iframe, rejects fragment
  URLs, penalizes tiny MP4s against long videos, and never auto-selects DASH.
  This avoids the outer-page advertising MP4 that a naive Resource Timing scan
  found on iframe-based streaming pages.
- Added `POST /cast`, a bounded form handoff carrying URL plus Referer, Origin
  and User-Agent. Values are safely percent-decoded and stripped of HTTP control
  characters. Cookies and authorization are deliberately not accepted or sent.
- Receiver addresses are restricted to private-LAN IPv4 or `.local` hosts.
  Candidate/session metadata is memory-only and expires automatically; video
  bytes travel from the origin straight to the PS4.
- Extension detector unit tests, JavaScript/manifest syntax checks and desktop
  visual QA pass. `dist/PS4-Cast-v04.33.pkg` is signed and passes every
  OpenOrbis package hash/signature check. Loading the unpacked extension in the
  user's normal Chrome profile and live on-console casting remain pending.

## v04.29: separate Cast and Live TV UX
- The console home is no longer a permanent split screen. `L1/R1` switches
  between a focused Cast receiver (QR, address, ready state) and a full-width
  Live TV bouquet/channel browser; the last manually selected home mode persists.
- Playback origin is explicit (`CAST` vs `IPTV`). A loaded playlist can no longer
  steal D-pad input from a cast video. Web `/chan` requests carry IPTV origin;
  direct links, uploads and DLNA carry Cast origin.
- The duplicate flat auto-tuning zapper and its independent selection state were
  removed. IPTV now has one guide: Down opens it, Up/Down selects, Left/Right
  changes bouquet, Cross tunes, Circle closes and Square toggles favourite.
  Browsing never tears down/reopens the decoder until Cross commits a channel.
- Outside the guide, `L1/R1` quick-zaps channels and `L2/R2` quick-zaps bouquets
  with a compact banner instead of reopening the channel list. Cast mode keeps
  Left/Right and `L1/R1` for seeking; Down only reveals or hides the HUD.
- The phone UI mirrors the same `Cast | Live TV` modes. Now Playing and failures
  remain shared, while direct links/uploads/history and IPTV import/search/manage
  are separated. The flatter solid-color styling removes decorative blur and
  reduces card radius without adding any work to the PS4 playback path.
- C compilation/linking passed. Browser QA passed at 390x844 and 1280x800 with
  one visible mode at a time, persisted selection, Arabic channel metadata, no
  horizontal overflow and no console warnings. `dist/PS4-Cast-v04.29.pkg` is
  signed and passes every OpenOrbis package hash/signature check. On-device
  controller/playback checks remain pending because the last-known PS4
  (`192.168.1.4`) is offline on both `:8080` and `:9090`.

## v04.28: direct phone-browser handoff
- `/handoff` serves the normal embedded UI with a focused confirmation dialog.
  The detected media URL and title live in the URL fragment, so long signed CDN
  links are not sent in the HTTP request line or truncated by the route parser.
- **Phone browser helper** generates an Android bookmarklet and an iPhone
  Shortcuts script using the current PS4 host. Both inspect the active page for
  playing `<video>`/`<audio>` sources; MediaSource/blob players fall back to the
  latest HLS or direct-file Resource Timing entry.
- DRM, inaccessible cross-origin iframe players, and pages exposing no direct
  media URL fail closed with a clear message. Nothing is sent through an
  external resolver or proxy.
- Clipboard copy has a manual select-and-copy fallback because the PS4 UI is an
  HTTP LAN origin and mobile browsers may withhold the modern Clipboard API.
- `dist/PS4-Cast-v04.28.pkg` builds cleanly and passes every OpenOrbis package
  hash/signature check. Browser QA passed on phone and desktop layouts, including
  Arabic titles and exact signed-URL submission. On-device deployment is pending:
  the last-known PS4 (`192.168.1.4`) is offline on both `:8080` and `:9090`.

## v04.27: managed local upload + actionable web failures
- The web UI now shows the one persistent uploaded file, its exact storage size,
  and explicit replay/delete controls. Delete also cancels a queued local play
  or stops an active one before its open file handle is released.
- Playback failures now survive teardown as structured categories instead of
  being overwritten by `player_stop()` as `stopped`. The web UI offers Retry,
  software-decode retry, diagnostic copy, and dismiss actions.
- `/status` now JSON-escapes every diagnostic/title field, so UTF-8 filenames and
  punctuation cannot corrupt the control page response.
- `dist/PS4-Cast-v04.27.pkg` compiles cleanly and passes all OpenOrbis package
  hash/signature checks. Browser QA passed at 390px and 1280px with Arabic file
  names and no horizontal overflow. On-device testing is pending because the
  last-known PS4 (`192.168.1.4`) is offline on both `:8080` and `:9090`.

## v04.26: local-file upload from the web UI
- **Implemented:** **Choose a local file** uploads a video directly from the
  browser to `/data/ps4cast_upload.bin`, then queues the normal in-app player.
- Uploads stream to a temporary file and atomically replace the previous upload
  only after all bytes arrive. Interrupted/full-disk transfers leave the prior
  file untouched; a vanished client times out after 30 seconds.
- Playback is fully seekable and uses the same H.264 hardware eligibility gate,
  audio/sync path, and software fallback as remote files. The original UTF-8
  filename is retained for the HUD; the browser never controls a PS4 path.
- The file is uploaded completely before playback starts. This is intentional:
  arbitrary MP4/MKV files may keep required metadata at the end, so progressive
  start would fail for common non-faststart files.
- `dist/PS4-Cast-v04.26.pkg` built and passed every OpenOrbis hash/signature
  validation. Not yet deployed: the last-known PS4 (`192.168.1.4`) was offline.

## Blocking the autonomous loop
- **GoldHEN `:9090` must be armed on the console** for the deploy/test pipeline to run. When it is not armed (or the console is off), the loop falls back to offline code work and build verification.
- **Authenticated remote launch needs one-time Chiaki pairing.** Run
  `scripts/setup-remote-launch.sh`, use the PS4's Remote Play Add Device PIN, and
  enter the same encoded Account ID used by Chiaki-Up. If GoldHEN `:9090` is
  armed, setup retrieves that Account ID from UserService and places it on the
  clipboard. After registration, launch is unattended.

## Decisions that need you (risk / product calls)
- **Live HLS buffer is capped at the server's window** (the test stream publishes only ~3 segments ≈ 9s). We can't buffer more than the live edge exposes. ~9s + keep-alive is enough to ride dips; just know true "minutes of headstart" is only possible for VOD playlists, not this live stream.

## Engine status (verified live, v02.99)
Robust HW+SW decode engine across formats, all tested on-device:
- **mp4 (H264)** — HW decode, `drop≈0`, audio OK (w3schools faststart clip). ✓
- **mkv (H264+AAC)** — HW decode, `drop=0`, audio OK (remuxed clip over LAN HTTP). ✓
- **m3u8 / live HLS** — HW decode + segment read-ahead, `rb=0`, audio underruns frozen, no micro-cuts, survives discontinuities. ✓
- **SW fallback** — gate falls back to software for non-H264 (HEVC/VP9) or if `vdec_hw_open` fails; `/hwdecode` toggles HW at runtime.

## RESOLVED/RECLASSIFIED: the "VOD-HLS crash" was NOT VOD HLS
After recovery (user re-armed :9090), retested: **VOD HLS plays fine end-to-end** on a fresh app — locally-generated TS media playlist (`#EXT-X-ENDLIST`, H264+AAC over LAN HTTP) decoded 243 frames across both segments, drained to EOF (`hls mem drained next_idx=2/2`), app stayed responsive. So **VOD HLS is exonerated** — the AVIO `hls_read` + software `decode_thread_main` path is fine, and the "robust engine" now covers VOD HLS too.
- **`/crashlog` was empty** ("no crash logged") → the earlier failure was NOT a signal/segfault. The fatal-signal handler writes the log before _exit; an empty log means the **freeze-watchdog** `_exit`'d it (it doesn't write a log). So it was a **hang**, not a crash.
- **Reclassified root cause:** a rare **resource-accumulation hang after long uptime + ~9 cast/stop cycles** (the VOD cast was just the final straw, hours into the session). NOT triggered by VOD content and NOT a normal-usage path (needs sustained heavy automated cast cycling).
- **Ruled out:** segment fetch/serve (`hls_read`/`ensure_segment`/`prefetch_*` reviewed clean); aseg keep-alive socket (single static slot, `conn_close`'d on host-change/reuse — doesn't accumulate). hls_close() does NOT close the keep-alive socket but that's at most 1 fd, not a per-cast leak.
- **Remaining leak suspects (need resource telemetry to confirm):** GPU/direct-memory not fully freed per `vdec_hw_open`/`vdec_hw_close` cycle (ties to the SceShellUI SYSTEM_VM_RUNTIME note), or another per-cast resource. **Next diagnostic step:** add fd-count/heap telemetry to `/status` so accumulation is observable, then run a long cast-cycle soak. Until then this is low-priority (not a normal-usage bug).
- **Lesson stands:** casting in tight automated loops + flaky :9090 = risk of unrecoverable downtime. Keep heavy soak tests for when the user is present.
- **Lesson for unattended loops:** casting untested content types can crash the app, and when `:9090` is unarmed I can't recover it autonomously. Keep risky content-compat probes for when you're present / `:9090` is reliably armed.

## v03.00 DEPLOYED + verified; dmem leak DISPROVEN; live test stream DEAD
- v03.00 is live (user deployed). `dmem=<KB>` telemetry works: idle=0, mp4 HW play=35952KB, **steady across 4 cast cycles** → `vdec_hw_close` releases cleanly, **no direct-memory leak**. 8-cycle HLS↔mp4 soak: no crash, app responsive throughout. The earlier "resource-accumulation hang" is NOT a dmem leak — it remains a rare unreproduced transient; deprioritized.
- **The live test stream `s3.../cdnb102/hls/0/stream_1280x720_3300k.m3u8` is permanently dead** (`AllAccessDisabled`). App handles it gracefully ("hls fetch failed", no crash). **Need a NEW live, single-media-playlist, MPEG-TS HLS source** to re-verify the live seg-demux + read-ahead path (most public live HLS are master playlists, which use the SW AVIO path, not seg-demux — see master-playlist item).
- SEG_RING 24MB byte budget shipped in v03.00; behavior unchanged for normal ~1MB segments (3 slots ≪ 24MB), so no runtime re-verify needed; the live read-ahead itself was verified on v02.99.

## (superseded) Ready to deploy when you're present: v03.00 (telemetry + ring budget)
Built + committed, NOT yet deployed (deployed app is still 02.99; :9090 flaky so I held the deploy). Two changes:
1. **dmem telemetry** — `/status` now reports `dmem=<KB>` (outstanding HW direct memory). After each cast it should return to the same baseline; growth across cast cycles = the suspected leak, finally observable.
2. **Read-ahead byte budget** — SEG_RING bounded by 24MB as well as 3 slots (caps RAM on large-segment streams; small segments unchanged).
**Verification plan (needs you present, :9090 armed):** deploy v03.00 → cast live HLS, confirm `rb=0` + `dmem` steady → run a soak (repeated cast/stop cycles) watching `dmem`: if it climbs and never returns to baseline, that's the resource-accumulation hang root cause → fix `vdec_hw_close` to fully release. Just say "deploy it" when ready.

## FIXED (v03.01): present-drop — triple-buffered pipelined flip
Was: 2 framebuffers + blocking flip-wait serialized the NV12→BGRA convert with scanout → ~30Hz present cap (40fps content dropped 25%). Fix: 3 buffers, submit-then-advance without blocking, throttle only when >(N-1) flips in flight (gfx.c). Verified on synthetic clips (the 40fps live test stream is dead): 40fps content **31→41fps presented, 0 drops** (original problem solved); 60fps content 31→48fps (drops 30/s→13/s). Stable, no GPU hang across casts. Remaining: present is now **convert-bound at ~48fps** — 50/60fps sources still drop ~13/s. To reach full 60fps would need the **HW-scaled framebuffer** (render at source res, let sceVideoOut upscale → ~2.3× cheaper convert); larger change, edge-case value (most content ≤48fps now drop-free), deferred.

## (former) Needs you present (GPU/display risk): live-HLS 40fps→30fps present-drop
Diagnosed (v02.99): the test stream is genuinely **40fps** (ffprobe: 120 frames / 3.009s, r_frame_rate=40/1). Decode delivers all 40fps (HW), `q=24/24`, `rb=0`, but we present ~30fps and drop ~10/s. Root cause is NOT scale/blit cost — it's the display pipeline: `gfx_present` (gfx.c:70-91) uses **2 framebuffers + a blocking wait-for-flip** (`sceVideoOutGetFlipStatus` loop), which serializes the CPU NV12→BGRA convert with scanout → effective ~30Hz even though `sceVideoOutSetFlipRate(...,0)` allows 60Hz.
Fix options (each touches the core display path → can GPU-hang/black-screen and need a physical power-cycle, so do these with the user present):
1. **Triple buffering + pipelined present** — register 3 buffers, submit flip and immediately convert the next frame into the next buffer instead of blocking on flip completion. Lets all 40fps show.
2. **HW-scaled 720p framebuffer** — render the convert at native 720p and let the video-out compositor upscale to 1080p; cuts per-frame convert cost ~2.2× so render+flip fits one vblank. Also a buffer-attribute change.
Until then: 40fps→30fps is a minor judder on 40fps sources; everything else is smooth.

## Open compatibility gap: some HTTPS CDNs read 0 body bytes (httpsrc)
Direct mp4 AND mkv from `test-videos.co.uk` fail the same way: httpsrc connects, parses the `206` header (gets size), but reads **0 body bytes** (`fill=0KB seek=0 serve=0`) → `avformat_open_input` fails → "stopped". w3schools mp4 and LAN HTTP work fine, so it's server-specific (HTTP/2-only? a TLS/HTTP-read quirk BearSSL/httpsrc mishandles, or index-at-end seek over that server). Pre-existing (unrelated to the HW/read-ahead work). **Next investigation:** klog the httpsrc read loop against a failing CDN; check redirect/HTTP-2/chunked handling in httpsrc.c. *(separate from the HLS aseg path, which works.)*

## Open investigations
- **`SceShellUI` crashed once with `SYSTEM_VM_RUNTIME` (0xa0028401)** during repeated m3u8 crash+relaunch cycles — a GPU/video-memory runtime fault in the *system UI*, likely GPU/direct-memory not fully released across our app's crash cycles. Shell auto-recovered. Watch for recurrence; may need stricter GPU/dmem release on teardown.
- **CE-36329-3 compositor-side HW decode fault:** v03.09 reproduced on the reverted 2-buffer present path, so triple-buffer is NOT the root cause. Likely lead is `sceVideodec2` open/close churn during repeated live-HLS recasts/discontinuities. v03.12 added `sceVideodec2` lifecycle trace/counters, extra GPU fences, short quiesce delays around decoder close/open, and HLS segment-param reset/reopen. The earlier production siglongjmp decode guard was removed: it was process-wide/cross-thread unsafe and cannot catch this async compositor-side fault anyway. After green testing, v03.13 restored triple-buffered present with fail-closed flip checks intact.

### RESOLVED
- **Hardware decode for HLS** — DONE (v02.98). HW H.264 now runs on the live HLS seg-demux path (TS fed annex-b directly, no bsf; PTS→µs; reorder reset on discontinuity). Verified: drops halved (~10/s→~5/s), audio intact, survived a DISCONTINUITY reset with no crash. Toggle via `/hwdecode`. *(player_ff.c gate ~414, decode_video_hw_seg, decode_segment_thread_main)*
- **The m3u8 ~9s crash** — FIXED (v02.95, confirmed 3+ min no crash). Root cause was `build_scaled` reusing a stale `g_sws` across a discontinuity resolution change → `sws_scale` over-read. Now rebuilds on source-dim/format change.
- **Audio causes buffering on live HLS** — FIXED (v02.97). Root cause found via `/trace`: `av_find_best_stream(AUDIO)` returned `AVERROR_STREAM_NOT_FOUND` (-1381258232) on most per-segment TS demuxes (mid-stream AAC not classified within the 1s analyze window), so audio decoded for only ~1 in 4 segments → bursty `wr`, ~180 underruns/sec → audible buffering. Fix: fall back to a manual `codec_type` scan, then to a cached last-known-good stream index (`g_segVideoIdx`/`g_segAudioIdx`), reset on `player_play`. *(player_ff.c decode_segment_thread_main)*

### Code-review crash candidates (from offline audit — confirm with /crashlog before fixing)
The stream is live muxed TS with `EXT-X-DISCONTINUITY-SEQUENCE`; crash hits ~9s (first discontinuity). Ranked:
1. **Decoders not reopened on a param change at a discontinuity.** `g_vdec`/`g_adec`/`g_swr` are opened once from the first segment; the seg-demux feeds every per-segment `sfmt`'s packets into them. If a discontinuity changes resolution (video) or channel count/rate (AAC), `apply_hls_reset` re-inits swr with the *old* config and the decoders aren't reopened → `swr_convert`/`sws_scale` over-read. FIX (needs device test): when each segment's `sfmt` codecpar differs from the open decoder, reopen `g_vdec`/`g_adec`+`g_swr` from the new params. *(player_ff.c decode_segment_thread_main ~1143)*
2. **`g_resetGen` may not fire on `DISCONTINUITY-SEQUENCE` change** (only on negative index per the audit) → the (good) `apply_hls_reset` machinery never runs at the discontinuity. FIX: bump resetGen when DISCONTINUITY-SEQUENCE changes between live refreshes. *(hls.c parse_media / refresh_live_playlist)*
3. **aseg keep-alive doesn't handle `Transfer-Encoding: chunked`** — if a CDN serves a `.ts` chunked (no Content-Length), the read-to-EOF returns chunk-framing bytes as segment data → demuxer corruption. (This S3 stream is Content-Length, so not the cause here, but a compatibility gap.) FIX: de-chunk or force-close on chunked. *(aseg.c do_request/aseg_fetch_inner)*

**Already fixed (safe, build-verified, no device test needed):** the `g_sws` over-read — it now rebuilds when the SOURCE resolution/format changes, not just the output size (player_ff.c build_scaled). This is a real memory-safety bug independent of the above.

## Visibility + reproduction harness (NEW) — for the intermittent "system software error" crash
The user reports an intermittent crash dialog ("An error has occurred in the system software") with the app lingering in the background. `/status` is BLIND to it (decode/HTTP threads stay alive while the display faults). Tooling built to see + reproduce it:
- **`scripts/klog-capture.py`** — streams the PS4 kernel log (:3232), flags faults. CONFIRMED it captures the crash dialog: `CrashReportSuggestActionScene`/`CrashReportNavigationScene` = the dialog loading/unloading. Run it in the background during any test.
- **`scripts/fake-live-hls.py`** — local LIVE HLS simulator (sliding-window playlist over ffmpeg TS segments) → drives the real live seg-demux + read-ahead + HW path (the public test stream is dead). `ffmpeg ... -f segment -segment_time 2 -segment_format mpegts seg_%03d.ts` into /tmp/livesim, then cast `http://<host>:8010/live.m3u8`.
- **`scripts/auto-recover.sh`** — kill+relaunch via :9090 (validated 3×).

**Crash characterization (so far):** casting the live path intermittently kills the app with an **EMPTY /crashlog** (NOT a signal — fatal_signal would log; NOT the hang-watchdog — it logs HANG on v03.02) and the app is **absent from the klog FMEM process snapshot** → it's terminated from OUTSIDE by a **GPU/system fault**, which in-process signal/watchdog handlers cannot catch. Reproduced once (~5s in), ran clean the next time → intermittent. In the live HW-decode + triple-buffer flip path.
**Next leads:** (a) add app-side GPU/flip error logging — check `sceVideoOutSubmitFlip`/`sceGnmSubmitDone` return codes (currently IGNORED in gfx_present) + log to catch the fault precursor before the system kills us; (b) extended live-sim soak with klog to gather crash instances + look for a trigger (discontinuity? a specific segment boundary?); (c) A/B the triple-buffer contribution (user says crash predates it, but aggressive flips may raise its rate).

## Memory/process audit (3 parallel agents) — answer to "is it on point?"
**Memory: YES, on point.** Full alloc→free trace found NO accumulating per-cast leak, double-free, unbounded queue, or overrun. Teardown is disciplined (all worker threads joined before frees; GPU dmem balanced). Only bounded nits (aseg keep-alive socket/TLS/pool/mutex never explicitly closed — one connection, not growth).
**Process/GPU: had real gaps that explain the UNKILLABLE freeze.** Findings + status:
- ✅ FIXED v03.07 — **P0: `_exit()` on GPU/flip fault/freeze released nothing** → VideoOut scanout context stayed registered → kernel couldn't reclaim → unkillable. Added `gfx_emergency_release()` (sceGnmSubmitDone + sceVideoOutClose) before _exit in gfx_fatal, watchdog (before the crash-log write), and fatal_signal.
- ✅ FIXED v03.07 — **P1: HW decoder compute queue never fenced at close** → freeing garlic/onion mid-DMA (GPU page fault) + SUBMITDONE_TIMEOUT_IN_SUSPEND. Added sceGnmSubmitDone() at top of vdec_hw_close.
- ✅ FIXED v03.07 — **P0: remote thread blocked forever in scePadReadStateExt, never stopped/joined, leaked HID handle.** Now stop flag + closable handle + slow retry + DEFAULT OFF (dormant until verified).
- ⬜ TODO — **P0: aseg `g_abort` self-clears at top of `aseg_fetch_inner` (aseg.c:229)** → a stop-issued abort can be lost; fetch thread can wedge in DNS resolve/connect (resolver not abortable, ~8s) holding `g_fetchMtx`. Fix: don't self-clear g_abort (clear on fresh play); separate audio/video abort flags; bound the resolver.
- ⬜ TODO — **P1: HW `pDecode` (vdec_hw.c) has no timeout/fault guard** (the probe uses siglongjmp; production doesn't). Malformed H.264 can hang the GPU compute queue while the main loop keeps petting the watchdog (so it never fires). Fix: guard pDecode (siglongjmp like the probe) and/or a decode-timeout that triggers vdec_hw_close + fault.
- ⬜ TODO — **P1 (watchdog IO-stall):** watchdog now releases GPU before the crash-log write (mitigated), but persist_crash can still block on /data. Consider _exit-after-deadline.
- ⬜ TODO — **P1 (defensive):** move present_pool_stop() right after the decode-thread join; P2: aseg_close() on exit; escalate after N consecutive decode failures.
**Build v03.07 is the freeze-hardened build — NOT yet deployed (console needs power-cycle).** Verify on-device after power-cycle, then re-enable the remote for its own test.

## GPU "refresh" when stuck — what's possible
There's no way to reset the PS4 GPU from the Mac directly. The practical refresh is **force-kill the app** (control payload → releases its GPU/video-out context) + relaunch = fresh GPU context (auto-recover.sh), or a full reset via `ps4cast-reboot.bin`. **Both require `:9090` (GoldHEN loader) to be responsive** — and the trap is that when the GPU/app is badly stuck, `:9090` also stops answering, so remote recovery is impossible and it needs a manual console action (PS button → close app, or reboot). The in-app fail-closed (gfx_present flip-error/stall → clean _exit) is the in-process equivalent (exit → relaunch gets a fresh GPU) but only fires if the loop still runs. Possible future: app-side display reinit (close+reopen sceVideoOut) on a soft stall before exiting — gentler, but only helps non-frozen cases; untested.

## Deploy best practice (v-bump on console) — CODIFIED in redeploy.sh
Every new version: **close previous cleanly → verify closed → delete cleanly → install safely.** redeploy.sh [2/5] now: tries clean `/quit` (LoadExec exit, no dialog) first, then force-kill, retries up to 5×, and VERIFIES truly closed (unreachable on 3 consecutive checks) before uninstall/install — never installs over a running/frozen app, and gives a clear "clear it on the console" message if `:9090` won't accept the kill. Then [3/5] uninstall (delete) → [4/5] DPI install → [5/5] launch+verify.

Control-payload builds are reproducible and must never use a deleted `/private/tmp`
tree or a stale `.bin`. `scripts/setup-payload-deps.sh` checks out the pinned official
DirectPackageInstaller **DN6** payload source under ignored `third_party/`, and both
`redeploy.sh` and `app-status.sh` run it before compiling. The payload Makefile
uses upstream's checked-in `lib/syscalls.asm`, so rebuilds do not download syscall
tables. A control-payload compile failure is fatal for that deploy step; never add
`|| true` around it.

## Clean close (v03.05) — IMPLEMENTED & VERIFIED
`/quit` (and exit) now call `sceSystemServiceLoadExec("exit", NULL)` → klog shows `GameStopped(PCST00001) ... LoadExec => 0`, NO notifyAppCrash/coredump/CrashReport dialog. Fixes the "fake crash dialog on /quit". (Returning from main/_exit was read as abnormal.) Removed the temporary `/forcecrash` test endpoint.

## Crash dialog dismissal — findings (the system modal can't be closed from our app)
The "error has occurred in the system software" dialog is a **SceShellUI** (NPXS20001) modal, NOT our app's — confirmed: relaunching our app does NOT dismiss it (foreground focus changes but the modal stays on top). A hard uncaught crash can take down SceShellUI itself (`notifyAppCrash titleId=NPXS20001`). No homebrew API to inject a controller button or close another process's modal. So **dismissal isn't viable; prevention is the path**: clean-close (done) + fail-closed (done) + fix the GPU fault so the dialog never appears. User dismisses a stuck dialog with ✕, or we reboot (`ps4cast-reboot.bin`).

## Autonomous work queue (user directive, in priority order)
1. **TV remote via HDMI-CEC in-app** — LEAD FOUND: input (main.c:255-269) deliberately polls only `ORBIS_PAD_PORT_TYPE_STANDARD` and skips the SPECIAL port / `scePadReadStateExt` because that "console/TV remote path" BLOCKS the main loop. So the CEC remote is on the SPECIAL port. FIX: open the SPECIAL port + read it on a DEDICATED thread (blocking there is fine), map remote buttons (play/pause/stop/dpad/enter/back) → player controls (pause/stop/close). Needs on-device test with the user's TV remote.
2. **In-app close button** — UI element that triggers the clean close (LoadExec exit). Needs remote/pad to activate → pairs with #1.
3. **UI/UX overhaul** — web interface (web_ui.h) + in-app HUD: more intuitive, faster, easier.
4. **Compatibility sweep** — re-test all video configs (mp4/mkv/HLS/various codecs/resolutions/fps) to find remaining no-smooth/crash cases. NOW SAFE to do: fail-closed + clean-close + auto-recover + resilient klog catch/recover issues.
5. **Extended GFX-FAULT soak** — live-sim + klog over a long run to capture an actual GFX-FAULT and pin the GPU crash root cause. RUNNING (scripts/soak.sh). So far: NO crash caught in the soak window (the coredump in klog faults is residual from the earlier /forcecrash test). 
6. **Live-edge catch-up (smoothness)** — soak surfaced the app drifting far behind the live edge over time (lag grew to ~33s, q/ra starved, ~64% drops) and not snapping back to the edge. May be amplified by the local sim, but worth checking the live-jump/refresh logic in hls.c (refresh_live_playlist / g_resetGen) handles "fell behind" by jumping to the newest segment. soak.sh now re-casts when lag>15s to keep the soak at the edge.

## Fail-closed display path (v03.03) — IMPLEMENTED (user's idea)
On ANY display/GPU anomaly the app now fully exits cleanly with a logged reason, so it never lingers as "shadow playback" on a faulted system and we get a clean cause. `gfx_present` (gfx.c): checks `sceVideoOutSubmitFlip` return (was ignored) → `GFX-FAULT submitflip rc=..` + `_exit(0)`; polls flip completion with a 3s ceiling → `GFX-FAULT flip-stall` + `_exit(0)`. This catches the GPU fault at the flip API BEFORE the system makes the dialog. Verified: deployed, no false-trips, live path clean 60s (drop ~0.2/s). Now ARMED to capture the intermittent fault with a reason next time it fires.
**Full fail-closed coverage now:** signal crash → `CRASH sig=..` (fatal_signal); freeze/deadlock → `HANG stale=..` (watchdog); display/GPU fault → `GFX-FAULT ..` (gfx_present). All `_exit(0)` (clean, no dialog), all logged to /crashlog.

## Dev pipeline / crash auto-recovery (v03.02) — IMPLEMENTED
Goal: tolerate crashes during autonomous test loops without getting stuck.
- **Capture:** signal crashes write `CRASH v.. sig=.. addr=..` to /data; hangs now write `HANG v.. stale=..ms` (freeze-watchdog) — both readable via GET /crashlog. (Earlier gap: hangs left an empty log; fixed.)
- **Dialog:** the fatal-signal handler and the freeze-watchdog both `_exit(0)` (clean exit) → this PREVENTS the system CE-error dialog from appearing at all. So there's normally no dialog to dismiss. (A hard fault that bypasses the handler could still raise a modal needing a manual OK — rare; true pad-injection dismiss remains a future option if it ever shows up.)
- **Recover:** `scripts/auto-recover.sh` — detects down/hung app (NORESP + console alive), captures /crashlog, then force-kill (`ps4cast-kill.bin`) + relaunch (`ps4cast-launch.bin`) via :9090 with retries. **Verified end-to-end** (killed app → recovered on attempt 1 → plays). Use it in/around test loops to self-heal.
- **Remaining limit:** if GoldHEN `:9090` itself stops accepting (the flaky/single-shot loader, as happened once), auto-recover can't relaunch and reports "re-arm needed" — the one case still requiring you. A more persistent loader payload would close this gap.
# Resident deployment (current)

- The development installer is a resident GoldHEN payload on PS4 TCP `:9192`.
  `scripts/dev-deploy.sh` and `scripts/redeploy.sh` reuse it for close, uninstall,
  install, and verified launch cycles; do not probe GoldHEN `:9090`, because its
  loader is one-shot and a connect-only probe consumes it.
- The first deploy after a console reboot bootstraps the resident agent through
  `:9090`. Every later deploy in that boot goes directly to `:9192`, with no
  manual rearm. If the agent is deliberately replaced, shut it down cleanly so
  its listener closes before loading the replacement.
- The agent searches every documented BGFT subtype for the exact PS4 Cast
  content ID before registering. This clears interrupted PS4 Cast tasks without
  deleting unrelated downloads. It uses stable `sceAppInstUtilAppExists`
  observations for readiness; `sceAppInstUtilGetInstallProgressInfo` crashes the
  GoldHEN host process on this firmware and must not be restored.
- Chiaki development controls use the default map: Return=`Cross`,
  Backspace=`Circle`, arrows=`D-pad`, and Escape=`PS`. The helper
  `.devtools/ps4cast-send-key` posts those macOS keycodes to the exact Chiaki PID.
