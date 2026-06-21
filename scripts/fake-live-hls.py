#!/usr/bin/env python3
"""Local LIVE HLS simulator — serves a sliding-window media playlist (no
EXT-X-ENDLIST) over a directory of TS segments, so PS4 Cast treats it as a live
stream and exercises the live seg-demux + read-ahead + HW-decode path (the one
the user actually casts, which the dead public test stream used to cover).

Generate segments first, e.g.:
  ffmpeg -i in.mp4 -c:v libx264 -g 80 -c:a aac -f segment -segment_time 2 \
         -segment_format mpegts seg_%03d.ts

Run:
  scripts/fake-live-hls.py --dir /tmp/livesim --host 192.168.1.157 --port 8010 \
       --window 4 --target 2
Cast:  http://<host>:8010/live.m3u8
The window advances 1 segment every <target> seconds and wraps forever; the
media sequence keeps incrementing so the client sees a normal live edge.
"""
import argparse, glob, os, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ARGS = None
SEGS = []          # sorted segment filenames
T0 = None          # base monotonic-ish start (set in main from args, not Date.now)


def media_seq():
    # how many segment-durations have elapsed since start = live edge position
    return int((time.time() - T0) / ARGS.target)


def playlist():
    base = media_seq()
    lines = ["#EXTM3U", "#EXT-X-VERSION:3",
             f"#EXT-X-TARGETDURATION:{ARGS.target}",
             f"#EXT-X-MEDIA-SEQUENCE:{base}"]
    for i in range(base, base + ARGS.window):
        lines.append(f"#EXTINF:{float(ARGS.target):.3f},")
        lines.append(SEGS[i % len(SEGS)])           # wrap forever
    return ("\n".join(lines) + "\n").encode()


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        path = self.path.split("?", 1)[0].lstrip("/")
        if path in ("live.m3u8", "index.m3u8"):
            body = playlist()
            self.send_response(200)
            self.send_header("Content-Type", "application/vnd.apple.mpegurl")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(body)
            return
        fp = os.path.join(ARGS.dir, os.path.basename(path))
        if path.endswith(".ts") and os.path.isfile(fp):
            data = open(fp, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "video/mp2t")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        self.send_response(404); self.end_headers()


def main():
    global ARGS, SEGS, T0
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="/tmp/livesim")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8010)
    ap.add_argument("--window", type=int, default=4)
    ap.add_argument("--target", type=int, default=2)
    ap.add_argument("--start", type=float, default=time.time())
    ARGS = ap.parse_args()
    T0 = ARGS.start
    SEGS = sorted(os.path.basename(p) for p in glob.glob(os.path.join(ARGS.dir, "*.ts")))
    if not SEGS:
        raise SystemExit(f"no .ts segments in {ARGS.dir}")
    print(f"live-sim: {len(SEGS)} segs, window={ARGS.window}, target={ARGS.target}s "
          f"-> http://{ARGS.host}:{ARGS.port}/live.m3u8")
    ThreadingHTTPServer((ARGS.host, ARGS.port), H).serve_forever()


if __name__ == "__main__":
    main()
