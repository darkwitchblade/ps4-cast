# PS4 Cast — items needing your steering

Things the autonomous loop can't decide or do alone. Revisit these together.

## Blocking the autonomous loop
- **GoldHEN `:9090` must be armed on the console** for the deploy/test pipeline to run. When it's not armed (or the app is closed), I can't install/launch/test — I fall back to offline code work + build verification. To let me run end-to-end autonomously, leave the GoldHEN payload loader armed and the PS4 on.
- **My Mac's LAN IP keeps changing** (DHCP: .139 → .160 → .157…). The deploy scripts default to stale IPs; I override with `PS4_IP`/`HOST_IP` each run. Consider a DHCP reservation for both the Mac and PS4 so addresses are stable.

## Decisions that need you (risk / product calls)
- **Live HLS buffer is capped at the server's window** (the test stream publishes only ~3 segments ≈ 9s). We can't buffer more than the live edge exposes. ~9s + keep-alive is enough to ride dips; just know true "minutes of headstart" is only possible for VOD playlists, not this live stream.

## Engine status (verified live, v02.99)
Robust HW+SW decode engine across formats, all tested on-device:
- **mp4 (H264)** — HW decode, `drop≈0`, audio OK (w3schools faststart clip). ✓
- **mkv (H264+AAC)** — HW decode, `drop=0`, audio OK (remuxed clip over LAN HTTP). ✓
- **m3u8 / live HLS** — HW decode + segment read-ahead, `rb=0`, audio underruns frozen, no micro-cuts, survives discontinuities. ✓
- **SW fallback** — gate falls back to software for non-H264 (HEVC/VP9) or if `vdec_hw_open` fails; `/hwdecode` toggles HW at runtime.

## ⚠️ NEEDS YOU: app down — VOD-HLS cast crashed it, :9090 won't relaunch
During an autonomous format-compatibility sweep (v02.99), casting a **VOD HLS** stream (a locally-generated TS media playlist with `#EXT-X-ENDLIST`, H264+AAC, served over LAN HTTP) crashed the app: `:8080` went down right after the cast. Console is healthy (ping + klog `:3232` OK, SceShellCore heap normal — **not** a GPU hang, no power-cycle needed). But auto-relaunch failed: GoldHEN's `:9090` payload loader stopped accepting POSTs (the known flaky/single-shot behavior). **To recover: re-arm the GoldHEN payload loader on the console, or just reopen PS4 Cast manually.** Once it's back, FIRST thing to do is `curl :8080/crashlog` to read the recorded signal+addr — the crash happened on the VOD-HLS path and the fault is captured on-device at /data/ps4cast_crash.log.
- **Suspected bug:** the VOD HLS path (non-live → AVIO byte-stream `hls_read` + software decode `decode_thread_main`, NOT the new seg-demux path) crashed on this content. My recent changes don't touch that path (gate refactor leaves VOD HLS on the old AVIO/SW path), so this looks **pre-existing** — but unconfirmed until /crashlog is read. Do NOT re-cast VOD HLS until diagnosed.
- **Lesson for unattended loops:** casting untested content types can crash the app, and when `:9090` is unarmed I can't recover it autonomously. Keep risky content-compat probes for when you're present / `:9090` is reliably armed.

## Needs you present (GPU/display risk): live-HLS 40fps→30fps present-drop
Diagnosed (v02.99): the test stream is genuinely **40fps** (ffprobe: 120 frames / 3.009s, r_frame_rate=40/1). Decode delivers all 40fps (HW), `q=24/24`, `rb=0`, but we present ~30fps and drop ~10/s. Root cause is NOT scale/blit cost — it's the display pipeline: `gfx_present` (gfx.c:70-91) uses **2 framebuffers + a blocking wait-for-flip** (`sceVideoOutGetFlipStatus` loop), which serializes the CPU NV12→BGRA convert with scanout → effective ~30Hz even though `sceVideoOutSetFlipRate(...,0)` allows 60Hz.
Fix options (each touches the core display path → can GPU-hang/black-screen and need a physical power-cycle, so do these with the user present):
1. **Triple buffering + pipelined present** — register 3 buffers, submit flip and immediately convert the next frame into the next buffer instead of blocking on flip completion. Lets all 40fps show.
2. **HW-scaled 720p framebuffer** — render the convert at native 720p and let the video-out compositor upscale to 1080p; cuts per-frame convert cost ~2.2× so render+flip fits one vblank. Also a buffer-attribute change.
Until then: 40fps→30fps is a minor judder on 40fps sources; everything else is smooth.

## Open compatibility gap: some HTTPS CDNs read 0 body bytes (httpsrc)
Direct mp4 AND mkv from `test-videos.co.uk` fail the same way: httpsrc connects, parses the `206` header (gets size), but reads **0 body bytes** (`fill=0KB seek=0 serve=0`) → `avformat_open_input` fails → "stopped". w3schools mp4 and LAN HTTP work fine, so it's server-specific (HTTP/2-only? a TLS/HTTP-read quirk BearSSL/httpsrc mishandles, or index-at-end seek over that server). Pre-existing (unrelated to the HW/read-ahead work). **Next investigation:** klog the httpsrc read loop against a failing CDN; check redirect/HTTP-2/chunked handling in httpsrc.c. *(separate from the HLS aseg path, which works.)*

## Open investigations
- **`SceShellUI` crashed once with `SYSTEM_VM_RUNTIME` (0xa0028401)** during repeated m3u8 crash+relaunch cycles — a GPU/video-memory runtime fault in the *system UI*, likely GPU/direct-memory not fully released across our app's crash cycles. Shell auto-recovered. Watch for recurrence; may need stricter GPU/dmem release on teardown.

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

## Dismissing the crash dialog automatically
You asked if a crash dialog can be auto-dismissed. The app can't dismiss the *system* crash dialog from inside (it's already dead). Options to explore: a control payload that sends the "OK/close" to the shell, or preventing the dialog entirely by always exiting cleanly (the watchdog + fatal-signal handler aim for this). Needs your input on acceptable approach.
