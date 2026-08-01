# FFmpeg version staging

The app statically links FFmpeg from `portlibs/ffmpeg/`, so swapping versions is a
file-level operation with no runtime ABI risk — the risk is **playback behaviour**,
which is why the old build is kept until the new one proves equal or better.

## Current state

| | Version | Where |
|---|---|---|
| **Installed** (what `build.sh` links) | **6.1.6** | `portlibs/ffmpeg/` |
| **Known-good rollback** | 6.1.1 | `portlibs/ffmpeg-611-known-good/` |

Also preserved:
- git tag `ffmpeg-6.1.1-known-good` (app v03.78, last build on 6.1.1)
- `dist/PS4-Cast-KNOWN-GOOD-ffmpeg611.pkg`

6.1.6 is the newest 6.1.x release and carries parser/decoder security fixes over
6.1.1, which matters because the app feeds arbitrary internet media into these
parsers.

## Roll back to 6.1.1

```bash
cd portlibs
rm -rf ffmpeg && cp -R ffmpeg-611-known-good ffmpeg
cd .. && ./build.sh
```

## Rebuild 6.1.6 from source

```bash
cd portlibs && ./build-ffmpeg-616.sh      # sources in src/ffmpeg-6.1.6
```

## Acceptance matrix (run before trusting 6.1.6)

6.1.6 is only "promoted" once it is **equal or better** than 6.1.1 on all of:

- [ ] Direct MP4 (H.264 progressive) — plays, HW path, 0 audio drop
- [ ] MKV / TS direct
- [ ] Live HLS TS via seg-demux (IPTV) — `ff/HW/hls-seg`, drift ~0, lag < 40ms
- [ ] 1080i HLS — HW decode + bob deinterlace
- [ ] VOD HLS
- [ ] Audio: no `drop=` growth, `und=` not climbing in steady state
- [ ] Seek: resumes, no permanent desync, no freeze
- [ ] Channel switch: teardown does not exceed the watchdog grace
- [ ] Sustained playback (10+ min) with no crash-log entry

If any regress, roll back — do not ship a version that is worse on playback for a
security uplift alone; stage a fix instead.
