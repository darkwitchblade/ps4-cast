#!/usr/bin/env bash
# Cast each test clip to the PS4 and measure decode fps / drops / errors / state.
PS4=${PS4_IP:-192.168.1.253}; HOST=${HOST_IP:-192.168.1.139}
BASE="http://$HOST:8000/streamtest"
ST(){ curl -sS -m5 "http://$PS4:8080/status" 2>/dev/null; }
field(){ echo "$1" | sed -E "s/.*$2=([0-9-]+).*/\1/"; }

CLIPS=(
  h264_360p.mp4 h264_720p.mp4 h264_1080p_4mbps.mp4 h264_1080p_high_10mbps.mp4
  h264_720p_60fps.mp4 h264_1080p_nonfast.mp4 h264_720p.mkv h264_720p.ts
  mpeg4_480p.avi hevc_720p.mp4 vp9_720p.webm hls/master.m3u8
)

printf "%-28s %-10s %6s %6s %5s %-10s\n" "CLIP" "RES" "FPS" "DROP" "ERR" "STATE"
printf "%-28s %-10s %6s %6s %5s %-10s\n" "----" "---" "---" "----" "---" "-----"
for c in "${CLIPS[@]}"; do
  curl -sS -m5 -X POST "http://$PS4:8080/stop" >/dev/null 2>&1; sleep 2
  curl -sS -m6 -X POST "http://$PS4:8080/avplay" --data-binary "$BASE/$c" >/dev/null 2>&1
  sleep 3
  s1=$(ST); f1=$(field "$s1" fr); t1=$(date +%s)
  sleep 7
  s2=$(ST); f2=$(field "$s2" fr); t2=$(date +%s)
  res=$(echo "$s2" | sed -E 's/.*ff[^ ]* \| ([0-9]+x[0-9]+).*/\1/')
  drop=$(field "$s2" drop); err=$(field "$s2" er)
  state=$(echo "$s2" | sed -E 's/.*"status":"([^"]*)".*/\1/' | cut -c1-10)
  dt=$((t2-t1)); [ "$dt" -lt 1 ] && dt=1
  if [ -n "$f1" ] && [ -n "$f2" ] && [ "$f1" -ge 0 ] 2>/dev/null; then fps=$(( (f2-f1)/dt )); else fps="-"; fi
  printf "%-28s %-10s %6s %6s %5s %-10s\n" "$c" "${res:-?}" "${fps:-?}" "${drop:-?}" "${err:-?}" "${state:-?}"
done
curl -sS -m5 -X POST "http://$PS4:8080/stop" >/dev/null 2>&1
