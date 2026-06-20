#!/usr/bin/env bash
set -euo pipefail

PS4=${PS4_IP:-192.168.1.253}

curl -sS -m 5 "http://$PS4:8080/status"
echo

for token in TESTA TESTA_5 TESTB TESTB_5 TESTB_NL TESTB_NP TESTB_S TESTM_5 TESTH_5; do
  echo "== $token"
  curl -sS -m 5 -X POST "http://$PS4:8080/play" --data-binary "$token"
  sleep 4
  curl -sS -m 5 "http://$PS4:8080/status"
  echo
done
