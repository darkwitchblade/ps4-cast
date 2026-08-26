#!/usr/bin/env python3
"""Turn a DevTools "Copy as cURL" into a PS4 Cast channel URL.

A blob: URL cannot be streamed -- it is a browser-local handle to a MediaSource
buffer, not a network address. The real media is fetched by JS as an HLS/DASH
manifest plus segments; this converts that request (with the headers the CDN
checks) into the "url|Key=Value" form PS4 Cast understands.

Usage:
  pbpaste | scripts/curl2ps4.py            # macOS, after Copy as cURL
  scripts/curl2ps4.py < request.txt
"""
import re, shlex, sys

KEEP = ("referer", "origin", "user-agent", "cookie")

def main() -> int:
    raw = sys.stdin.read().strip()
    if not raw:
        print("nothing on stdin -- copy the manifest request as cURL first", file=sys.stderr)
        return 2
    # DevTools emits line continuations; flatten them before tokenising.
    raw = raw.replace("\\\n", " ").replace("^\n", " ")
    try:
        toks = shlex.split(raw)
    except ValueError as e:
        print(f"could not parse: {e}", file=sys.stderr)
        return 2

    url, headers = None, {}
    i = 0
    while i < len(toks):
        t = toks[i]
        if t in ("-H", "--header") and i + 1 < len(toks):
            k, _, v = toks[i + 1].partition(":")
            k = k.strip().lower(); v = v.strip()
            if k in KEEP and v:
                headers[k] = v
            i += 2
            continue
        if t in ("-b", "--cookie") and i + 1 < len(toks):
            headers["cookie"] = toks[i + 1]; i += 2; continue
        if t.startswith("http"):
            url = t
        elif t == "--url" and i + 1 < len(toks):
            url = toks[i + 1]; i += 1
        i += 1

    if not url:
        print("no http URL found in that cURL", file=sys.stderr)
        return 1
    if url.startswith("blob:"):
        print("that is a blob: URL -- it cannot be fetched. Filter the Network tab\n"
              "for m3u8/mpd and copy THAT request instead.", file=sys.stderr)
        return 1

    kind = ".m3u8 (HLS - supported)" if ".m3u8" in url else (
           ".mpd (DASH - NOT yet supported by the app)" if ".mpd" in url else
           "no manifest extension - may still be HLS, check the body starts with #EXTM3U")
    opts = "&".join(f"{k.title().replace('Agent','Agent')}={v}"
                    for k, v in (("Referer", headers.get("referer")),
                                 ("User-Agent", headers.get("user-agent")),
                                 ("Cookie", headers.get("cookie"))) if v)
    # Title-casing above must not mangle the canonical names the app accepts.
    opts = opts.replace("Referer=", "Referer=").replace("User-Agent=", "User-Agent=")

    print(f"# detected: {kind}")
    if "expires=" in url or "token=" in url or "hdnts=" in url:
        print("# NOTE: this URL carries a signed token -- it will expire, often within minutes.")
    print()
    print(url + ("|" + opts if opts else ""))
    return 0

if __name__ == "__main__":
    sys.exit(main())
