# PS4 Cast — items needing your steering

Things the autonomous loop can't decide or do alone. Revisit these together.

## Blocking the autonomous loop
- **GoldHEN `:9090` must be armed on the console** for the deploy/test pipeline to run. When it's not armed (or the app is closed), I can't install/launch/test — I fall back to offline code work + build verification. To let me run end-to-end autonomously, leave the GoldHEN payload loader armed and the PS4 on.
- **My Mac's LAN IP keeps changing** (DHCP: .139 → .160 → .157…). The deploy scripts default to stale IPs; I override with `PS4_IP`/`HOST_IP` each run. Consider a DHCP reservation for both the Mac and PS4 so addresses are stable.

## Decisions that need you (risk / product calls)
- **Hardware decode for HLS** (`player_ff.c` line ~404, `!g_isHls` guard): biggest smoothness win + frees the CPU, but it was deliberately disabled — likely because it crashed. You chose "safe fixes first, then attempt HW-for-HLS separately." Pending the safe round being verified stable.
- **Live HLS buffer is capped at the server's window** (the test stream publishes only ~3 segments ≈ 9s). We can't buffer more than the live edge exposes. ~9s + keep-alive is enough to ride dips; just know true "minutes of headstart" is only possible for VOD playlists, not this live stream.

## Open investigations
- **`SceShellUI` crashed once with `SYSTEM_VM_RUNTIME` (0xa0028401)** during repeated m3u8 crash+relaunch cycles — a GPU/video-memory runtime fault in the *system UI*, likely GPU/direct-memory not fully released across our app's crash cycles. Shell auto-recovered. Watch for recurrence; may need stricter GPU/dmem release on teardown.
- **The m3u8 ~9s crash** — our clean-exit handler hides the fault; v02.94 added `/crashlog` (signal+addr persisted on crash) to capture it on the next on-device repro. **Do this first when the console is back:** cast the m3u8, let it crash, `curl /crashlog`.

### Code-review crash candidates (from offline audit — confirm with /crashlog before fixing)
The stream is live muxed TS with `EXT-X-DISCONTINUITY-SEQUENCE`; crash hits ~9s (first discontinuity). Ranked:
1. **Decoders not reopened on a param change at a discontinuity.** `g_vdec`/`g_adec`/`g_swr` are opened once from the first segment; the seg-demux feeds every per-segment `sfmt`'s packets into them. If a discontinuity changes resolution (video) or channel count/rate (AAC), `apply_hls_reset` re-inits swr with the *old* config and the decoders aren't reopened → `swr_convert`/`sws_scale` over-read. FIX (needs device test): when each segment's `sfmt` codecpar differs from the open decoder, reopen `g_vdec`/`g_adec`+`g_swr` from the new params. *(player_ff.c decode_segment_thread_main ~1143)*
2. **`g_resetGen` may not fire on `DISCONTINUITY-SEQUENCE` change** (only on negative index per the audit) → the (good) `apply_hls_reset` machinery never runs at the discontinuity. FIX: bump resetGen when DISCONTINUITY-SEQUENCE changes between live refreshes. *(hls.c parse_media / refresh_live_playlist)*
3. **aseg keep-alive doesn't handle `Transfer-Encoding: chunked`** — if a CDN serves a `.ts` chunked (no Content-Length), the read-to-EOF returns chunk-framing bytes as segment data → demuxer corruption. (This S3 stream is Content-Length, so not the cause here, but a compatibility gap.) FIX: de-chunk or force-close on chunked. *(aseg.c do_request/aseg_fetch_inner)*

**Already fixed (safe, build-verified, no device test needed):** the `g_sws` over-read — it now rebuilds when the SOURCE resolution/format changes, not just the output size (player_ff.c build_scaled). This is a real memory-safety bug independent of the above.

## Dismissing the crash dialog automatically
You asked if a crash dialog can be auto-dismissed. The app can't dismiss the *system* crash dialog from inside (it's already dead). Options to explore: a control payload that sends the "OK/close" to the shell, or preventing the dialog entirely by always exiting cleanly (the watchdog + fatal-signal handler aim for this). Needs your input on acceptable approach.
