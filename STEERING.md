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
