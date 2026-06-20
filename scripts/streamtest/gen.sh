#!/usr/bin/env bash
# Generate a matrix of test clips (container x codec x resolution x bitrate),
# all 10s with AAC audio (for A/V sync checks), from a motion test pattern.
set -e
OUT="$(cd "$(dirname "$0")/../../dist" && pwd)/streamtest"
mkdir -p "$OUT"; cd "$OUT"
FF=/opt/homebrew/opt/ffmpeg/bin/ffmpeg; [ -x "$FF" ] || FF=/opt/homebrew/bin/ffmpeg
SRC=(-f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=10" -f lavfi -i "sine=frequency=440:duration=10")
A=(-c:a aac -b:a 128k -shortest)
gen(){ local out="$1"; shift; echo "  $out"; "$FF" -hide_banner -loglevel error -y "${SRC[@]}" "$@" "${A[@]}" "$out" </dev/null; }

gen h264_360p.mp4   -vf scale=640:360  -c:v libx264 -preset veryfast -b:v 800k  -pix_fmt yuv420p -movflags +faststart
gen h264_720p.mp4   -vf scale=1280:720 -c:v libx264 -preset veryfast -b:v 2500k -pix_fmt yuv420p -movflags +faststart
gen h264_1080p_4mbps.mp4       -c:v libx264 -preset veryfast -b:v 4000k  -pix_fmt yuv420p -movflags +faststart
gen h264_1080p_high_10mbps.mp4 -c:v libx264 -preset veryfast -profile:v high -b:v 10000k -pix_fmt yuv420p -movflags +faststart
gen h264_720p_60fps.mp4 -vf scale=1280:720 -r 60 -c:v libx264 -preset veryfast -b:v 4000k -pix_fmt yuv420p -movflags +faststart
gen h264_720p.mkv -vf scale=1280:720 -c:v libx264 -preset veryfast -b:v 2500k -pix_fmt yuv420p
gen h264_720p.ts  -vf scale=1280:720 -c:v libx264 -preset veryfast -b:v 2500k -pix_fmt yuv420p -f mpegts
gen mpeg4_480p.avi -vf scale=854:480 -c:v mpeg4 -q:v 4
gen hevc_720p.mp4 -vf scale=1280:720 -c:v libx265 -preset ultrafast -x265-params log-level=none -b:v 2000k -pix_fmt yuv420p -tag:v hvc1 -movflags +faststart
gen vp9_720p.webm -vf scale=1280:720 -c:v libvpx-vp9 -b:v 2000k -deadline realtime -cpu-used 8 -pix_fmt yuv420p
gen h264_1080p_nonfast.mp4 -c:v libx264 -preset veryfast -b:v 4000k -pix_fmt yuv420p   # moov at end (no faststart)

# Multi-variant HLS (ABR) master + a single-variant HLS
mkdir -p hls && cd hls
"$FF" -hide_banner -loglevel error -y "${SRC[@]}" \
  -filter_complex "[0:v]split=3[v3][v7][v10];[v3]scale=640:360[v3o];[v7]scale=1280:720[v7o];[v10]scale=1920:1080[v10o]" \
  -map "[v3o]" -map "[v7o]" -map "[v10o]" -map 1:a -map 1:a -map 1:a \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p \
  -b:v:0 800k -b:v:1 2500k -b:v:2 6000k -c:a aac -b:a 128k \
  -f hls -hls_time 2 -hls_playlist_type vod \
  -var_stream_map "v:0,a:0 v:1,a:1 v:2,a:2" \
  -master_pl_name master.m3u8 -hls_segment_filename "v%v_%03d.ts" "v%v.m3u8" </dev/null
echo "  hls/master.m3u8"
cd ..
echo "=== generated ==="; ls -la *.mp4 *.mkv *.ts *.avi *.webm hls/master.m3u8 2>/dev/null | awk '{print $5, $9}'
