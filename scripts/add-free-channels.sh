#!/usr/bin/env bash
# Add a curated set of FREE, officially-public IPTV channels to PS4 Cast,
# grouped into bouquets. Uses POST /channel/add, which APPENDS — your existing
# channels are kept (unlike loading an M3U, which replaces the whole list).
#
# Every entry here is verified TWICE: the master playlist resolves AND at least
# one of its variants serves real segments. Entries whose variants were all dead
# (France 24 EN/FR/Doc, NASA TV, CGTN) are omitted — a master that returns 200
# while every variant 400s is useless, and following redirects with `curl -L`
# hides that (CGTN "passed" by redirecting to its own 404 page).
#
# On-device results are authoritative over host-side probing: France 24 Arabic
# plays with hardware decode on the PS4 even though its variants refuse requests
# from this network. Re-run any time — it does not de-duplicate.
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
News	RT Arabic	https://rt-arb.rttv.com/live/rtarab/playlist.m3u8
World News	DW English	https://dwamdstream102.akamaized.net/hls/live/2015525/dwstream102/index.m3u8
World News	TV5Monde Info	https://ott.tv5monde.com/Content/HLS/Live/channel(info)/index.m3u8
World News	Arirang TV	https://amdlive-ch01-ctnd-com.akamaized.net/arirang_1ch/smil:arirang_1ch.smil/playlist.m3u8
Documentary	Al Jazeera Documentary	https://live-hls-web-ajd.getaj.net/AJD/index.m3u8
Sport	Red Bull TV	https://rbmn-live.akamaized.net/hls/live/590964/BoRB-AT/master.m3u8
EOF

after=$(curl -sS -m5 "http://$PS4:8080/status" | sed -n 's/.*"chan_n":\([0-9]*\).*/\1/p')
echo "added $n channels (${before:-?} -> ${after:-?})"
