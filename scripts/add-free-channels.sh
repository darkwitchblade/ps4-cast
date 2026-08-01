#!/usr/bin/env bash
# Add a curated set of FREE, officially-public IPTV channels to PS4 Cast,
# grouped into bouquets. Uses POST /channel/add, which APPENDS — your existing
# channels are kept (unlike loading an M3U, which replaces the whole list).
#
# Every URL here was probed and returned HTTP 200 at the time of writing; dead
# ones are deliberately left out rather than padding the list. Re-run any time —
# note it does not de-duplicate, so running twice adds twice.
#
# Usage: PS4_IP=192.168.1.4 scripts/add-free-channels.sh
set -uo pipefail
PS4=${PS4_IP:-192.168.1.4}

if ! curl -sS -m5 "http://$PS4:8080/status" >/dev/null 2>&1; then
  echo "PS4 Cast is not responding on $PS4:8080 — open the app first." >&2
  exit 1
fi

before=$(curl -sS -m5 "http://$PS4:8080/status" | sed -n 's/.*"chan_n":\([0-9]*\).*/\1/p')
n=0
while IFS=$'\t' read -r grp name url; do
  [ -z "${url:-}" ] && continue
  case "$grp" in \#*) continue;; esac
  if printf '%s\t%s\t%s' "$name" "$grp" "$url" \
       | curl -sS -m8 -X POST --data-binary @- "http://$PS4:8080/channel/add" >/dev/null; then
    n=$((n+1)); printf '  + %-14s %s\n' "$grp" "$name"
  else
    printf '  ! failed        %s\n' "$name"
  fi
done <<'EOF'
News	Al Jazeera Arabic	https://live-hls-web-aja.getaj.net/AJA/index.m3u8
News	Al Jazeera Mubasher	https://live-hls-web-ajm.getaj.net/AJM/index.m3u8
News	Al Arabiya	https://live.alarabiya.net/alarabiapublish/alarabiya.smil/playlist.m3u8
News	Al Hadath	https://live.alarabiya.net/alarabiapublish/alhadath.smil/playlist.m3u8
News	France 24 Arabic	https://static.france24.com/live/F24_AR_LO_HLS/live_web.m3u8
News	DW Arabic	https://dwamdstream103.akamaized.net/hls/live/2015526/dwstream103/index.m3u8
News	CGTN Arabic	https://arabic.cgtn.com/resource/live/arabic/cgtn-arabic.m3u8
News	RT Arabic	https://rt-arb.rttv.com/live/rtarab/playlist.m3u8
World News	France 24 English	https://static.france24.com/live/F24_EN_LO_HLS/live_web.m3u8
World News	France 24 Francais	https://static.france24.com/live/F24_FR_LO_HLS/live_web.m3u8
World News	DW English	https://dwamdstream102.akamaized.net/hls/live/2015525/dwstream102/index.m3u8
World News	CGTN English	https://news.cgtn.com/resource/live/english/cgtn-news.m3u8
World News	TV5Monde Info	https://ott.tv5monde.com/Content/HLS/Live/channel(info)/index.m3u8
World News	Arirang TV	https://amdlive-ch01-ctnd-com.akamaized.net/arirang_1ch/smil:arirang_1ch.smil/playlist.m3u8
Documentary	Al Jazeera Documentary	https://live-hls-web-ajd.getaj.net/AJD/index.m3u8
Documentary	France 24 Doc FR	https://static.france24.com/live/F24_FR_HI_HLS/live_web.m3u8
Science	NASA TV	https://ntv1.akamaized.net/hls/live/2014075/NASA-NTV1-HLS/master.m3u8
Sport	Red Bull TV	https://rbmn-live.akamaized.net/hls/live/590964/BoRB-AT/master.m3u8
EOF

after=$(curl -sS -m5 "http://$PS4:8080/status" | sed -n 's/.*"chan_n":\([0-9]*\).*/\1/p')
echo "added $n channels (${before:-?} -> ${after:-?})"
