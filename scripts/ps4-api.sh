#!/usr/bin/env bash
# Shared PS4 Cast HTTP helpers. Source from dev scripts:
#   source "$(dirname "$0")/ps4-api.sh"
#   ps4_post play "http://..."        -> POST /play with the pairing token
#   ps4_post stop
# The receiver gates state-changing endpoints behind the pairing token shown on
# the TV; /status (read-only) exposes it so local tooling can self-provision.
PS4=${PS4:-192.168.1.4}

ps4_token() {
    curl -sS -m3 "http://$PS4:8080/status" 2>/dev/null |
        sed -n 's/.*"token":"\([A-Z2-9]*\)".*/\1/p'
}

ps4_post() {  # ps4_post <endpoint-without-slash> [curl args...]
    local ep="$1"; shift
    local tok; tok="$(ps4_token)"
    local q=""
    [ -n "$tok" ] && q="?t=$tok"
    curl -sS -m5 -X POST "http://$PS4:8080/$ep$q" "$@"
}

ps4_status() { curl -sS -m3 "http://$PS4:8080/status" 2>/dev/null; }
