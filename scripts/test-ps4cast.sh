#!/usr/bin/env bash
set -euo pipefail

PS4=${PS4_IP:-192.168.1.253}
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"/ps4-api.sh

status="$(curl -sS -m 5 "http://$PS4:8080/status")"
printf '%s' "$status"
echo

token="$(printf '%s' "$status" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
[ -n "$token" ] || { echo "status did not include a pairing token" >&2; exit 1; }
ui_code="$(curl -sS -o /dev/null -w '%{http_code}' -m 5 "http://$PS4:8080/?t=$token")"
[ "$ui_code" = 200 ] || { echo "QR URL returned HTTP $ui_code, expected 200" >&2; exit 1; }

for token in TESTA TESTA_5 TESTB TESTB_5 TESTB_NL TESTB_NP TESTB_S TESTM_5 TESTH_5; do
  echo "== $token"
  ps4_post "play" -m5 --data-binary "$token"
  sleep 4
  curl -sS -m 5 "http://$PS4:8080/status"
  echo
done
