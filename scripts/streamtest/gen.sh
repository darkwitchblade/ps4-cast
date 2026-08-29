#!/usr/bin/env bash
# Generate a matrix of test clips (container x codec x resolution x bitrate),
# all 10s with AAC audio (for A/V sync checks), from a motion test pattern.
set -e
OUT="$(cd "$(dirname "$0")/../../dist" && pwd)/streamtest"
mkdir -p "$OUT"; cd "$OUT"
FF=/opt/homebrew/opt/ffmpeg/bin/ffmpeg; [ -x "$FF" ] || FF=/opt/homebrew/bin/ffmpeg
SRC=(-f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=10" -f lavfi -i "sine=frequency=440:duration=10")
SRC60=(-f lavfi -i "testsrc2=size=1920x1080:rate=60:duration=10" -f lavfi -i "sine=frequency=440:duration=10")
A=(-c:a aac -b:a 128k -shortest)
gen(){ local out="$1"; shift; echo "  $out"; "$FF" -hide_banner -loglevel error -y "${SRC[@]}" "$@" "${A[@]}" "$out" </dev/null; }

gen h264_360p.mp4   -vf scale=640:360  -c:v libx264 -preset veryfast -b:v 800k  -pix_fmt yuv420p -movflags +faststart
gen h264_720p.mp4   -vf scale=1280:720 -c:v libx264 -preset veryfast -b:v 2500k -pix_fmt yuv420p -movflags +faststart
gen h264_1080p_4mbps.mp4       -c:v libx264 -preset veryfast -b:v 4000k  -pix_fmt yuv420p -movflags +faststart
gen h264_1080p_high_10mbps.mp4 -c:v libx264 -preset veryfast -profile:v high -b:v 10000k -pix_fmt yuv420p -movflags +faststart
echo "  h264_720p_60fps.mp4"
"$FF" -hide_banner -loglevel error -y "${SRC60[@]}" -vf scale=1280:720 \
  -c:v libx264 -preset veryfast -b:v 4000k -pix_fmt yuv420p -movflags +faststart \
  "${A[@]}" h264_720p_60fps.mp4 </dev/null
echo "  h264_1080p60.mp4"
"$FF" -hide_banner -loglevel error -y "${SRC60[@]}" \
  -c:v libx264 -preset veryfast -profile:v high -b:v 8000k -pix_fmt yuv420p -movflags +faststart \
  "${A[@]}" h264_1080p60.mp4 </dev/null
gen h264_720p.mkv -vf scale=1280:720 -c:v libx264 -preset veryfast -b:v 2500k -pix_fmt yuv420p
gen h264_720p.ts  -vf scale=1280:720 -c:v libx264 -preset veryfast -b:v 2500k -pix_fmt yuv420p -f mpegts
gen mpeg4_480p.avi -vf scale=854:480 -c:v mpeg4 -q:v 4
gen hevc_720p.mp4 -vf scale=1280:720 -c:v libx265 -preset ultrafast -x265-params log-level=none -b:v 2000k -pix_fmt yuv420p -tag:v hvc1 -movflags +faststart
echo "  vp9_720p.webm"
"$FF" -hide_banner -loglevel error -y "${SRC[@]}" -vf scale=1280:720 \
  -c:v libvpx-vp9 -b:v 2000k -deadline realtime -cpu-used 8 -pix_fmt yuv420p \
  -c:a libopus -b:a 128k -shortest vp9_720p.webm </dev/null
gen h264_1080p_nonfast.mp4 -c:v libx264 -preset veryfast -b:v 4000k -pix_fmt yuv420p   # moov at end (no faststart)

# Multi-variant HLS (ABR) master + a single-variant HLS
mkdir -p hls && cd hls
"$FF" -hide_banner -loglevel error -y "${SRC[@]}" \
  -filter_complex "[0:v]split=3[v3][v7][v10];[v3]scale=640:360[v3o];[v7]scale=1280:720[v7o];[v10]scale=1920:1080[v10o]" \
  -map "[v3o]" -map "[v7o]" -map "[v10o]" -map 1:a -map 1:a -map 1:a \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p \
  -g 60 -keyint_min 60 -sc_threshold 0 \
  -b:v:0 800k -b:v:1 2500k -b:v:2 6000k -c:a aac -b:a 128k \
  -f hls -hls_time 2 -hls_playlist_type vod \
  -var_stream_map "v:0,a:0 v:1,a:1 v:2,a:2" \
  -master_pl_name master.m3u8 -hls_segment_filename "v%v_%03d.ts" "v%v.m3u8" </dev/null
echo "  hls/master.m3u8"
cd ..

# HLS master with an external audio rendition. This exercises the independent
# video/audio demuxers used by IPTV services and modern streaming origins.
mkdir -p hls_sep && cd hls_sep
"$FF" -hide_banner -loglevel error -y "${SRC[@]}" \
  -map 0:v -map 1:a \
  -c:v libx264 -preset veryfast -b:v 2500k -pix_fmt yuv420p \
  -g 60 -keyint_min 60 -sc_threshold 0 \
  -c:a aac -b:a 128k \
  -f hls -hls_time 2 -hls_playlist_type vod \
  -var_stream_map "v:0,agroup:aud a:0,agroup:aud,default:yes,name:stereo" \
  -master_pl_name master.m3u8 -hls_segment_filename "stream_%v_%03d.ts" \
  "stream_%v.m3u8" </dev/null
echo "  hls_sep/master.m3u8"
cd ..

# Fragmented MP4 HLS uses a MOV demuxer and init segment. Its seek path must
# reopen the demuxer and replay init.mp4 before the target media fragment.
mkdir -p hls_fmp4 && cd hls_fmp4
"$FF" -hide_banner -loglevel error -y "${SRC[@]}" \
  -map 0:v -map 1:a -c:v libx264 -preset veryfast -b:v 2500k -pix_fmt yuv420p \
  -g 60 -keyint_min 60 -sc_threshold 0 -c:a aac -b:a 128k \
  -f hls -hls_time 2 -hls_playlist_type vod -hls_segment_type fmp4 \
  -hls_fmp4_init_filename init.mp4 -hls_segment_filename "seg_%03d.m4s" \
  stream.m3u8 </dev/null
echo "  hls_fmp4/stream.m3u8"
cd ..
echo "=== generated ==="; ls -la *.mp4 *.mkv *.ts *.avi *.webm hls/master.m3u8 2>/dev/null | awk '{print $5, $9}'
