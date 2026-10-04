"""Test server for tests/hls-e2e: builds each HLS scenario (real AES-128-CBC
with PKCS#7, byte ranges, an encrypted separate-audio rendition, DRM) into
memory, writes the expected plaintext streams to OUT, then serves them.
Usage: server.py OUT_DIR PORT (0 = pick one; written to OUT_DIR/port)."""
import os, sys, http.server, socketserver, re
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import padding
OUT = sys.argv[1]; PORT = int(sys.argv[2])
os.makedirs(OUT, exist_ok=True)
files = {}          # path -> bytes (or callable)
ignore_range = set()
state = {"k_rot": 0}

def enc(key, iv, data):
    p = padding.PKCS7(128).padder(); d = p.update(data) + p.finalize()
    e = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor(); return e.update(d) + e.finalize()
def rnd(n, seed):
    import random; r = random.Random(seed); return bytes(r.getrandbits(8) for _ in range(n))
def expect(name, data): open(os.path.join(OUT, name), "wb").write(data)

# 1) AES-128 VOD: explicit IV, then key rotation with sequence-number IVs, then clear
K1, K2 = rnd(16, 1), rnd(16, 2); IV1 = rnd(16, 3)
segs = [rnd(188 * 50 + i * 7, 10 + i) for i in range(5)]   # odd lengths -> real padding
pl = ["#EXTM3U", "#EXT-X-TARGETDURATION:4", "#EXT-X-MEDIA-SEQUENCE:40",
      '#EXT-X-KEY:METHOD=AES-128,URI="keys/k1.bin",IV=0x' + IV1.hex()]
files["/aes/s0.ts"] = enc(K1, IV1, segs[0]); pl += ["#EXTINF:4,", "s0.ts"]
files["/aes/s1.ts"] = enc(K1, IV1, segs[1]); pl += ["#EXTINF:4,", "s1.ts"]
pl += ['#EXT-X-KEY:METHOD=AES-128,URI="/aes/keys/k2.bin"']
for i in (2, 3):
    files[f"/aes/s{i}.ts"] = enc(K2, (40 + i).to_bytes(16, "big"), segs[i]); pl += ["#EXTINF:4,", f"s{i}.ts"]
pl += ["#EXT-X-KEY:METHOD=NONE"]; files["/aes/s4.ts"] = segs[4]; pl += ["#EXTINF:4,", "s4.ts", "#EXT-X-ENDLIST"]
files["/aes/keys/k1.bin"] = K1; files["/aes/keys/k2.bin"] = K2
files["/aes/index.m3u8"] = "\n".join(pl).encode(); expect("aes.bin", b"".join(segs))

# 2/3) byte ranges into one file (+ init via MAP BYTERANGE), honoured and ignored
init = rnd(700, 20); parts = [rnd(4000 + 13 * i, 21 + i) for i in range(4)]
blob = init + b"".join(parts); files["/br/all.mp4"] = blob
o = len(init); lines = ["#EXTM3U", "#EXT-X-TARGETDURATION:4", f'#EXT-X-MAP:URI="all.mp4",BYTERANGE="{len(init)}@0"']
for i, p in enumerate(parts):
    lines += ["#EXTINF:4,", f"#EXT-X-BYTERANGE:{len(p)}" + (f"@{o}" if i in (0, 2) else ""), "all.mp4"]; o += len(p)
lines += ["#EXT-X-ENDLIST"]
files["/br/index.m3u8"] = "\n".join(lines).encode()
files["/brx/index.m3u8"] = files["/br/index.m3u8"]; files["/brx/all.mp4"] = blob; ignore_range.add("/brx/all.mp4")
expect("br.bin", blob)

# 4) encrypted init (MAP after KEY, IV given) + encrypted fMP4 segments, byte ranges on encrypted resources
K3 = rnd(16, 30); IV3 = rnd(16, 31)
einit = enc(K3, IV3, init); es = [enc(K3, IV3, p) for p in parts[:2]]
files["/efm/init.mp4"] = einit; files["/efm/pack.m4s"] = es[0] + es[1]
files["/efm/k"] = K3
files["/efm/index.m3u8"] = "\n".join(["#EXTM3U", "#EXT-X-TARGETDURATION:4",
    f'#EXT-X-KEY:METHOD=AES-128,URI="k",IV=0x{IV3.hex()}', '#EXT-X-MAP:URI="init.mp4"',
    "#EXTINF:4,", f"#EXT-X-BYTERANGE:{len(es[0])}@0", "pack.m4s",
    "#EXTINF:4,", f"#EXT-X-BYTERANGE:{len(es[1])}", "pack.m4s", "#EXT-X-ENDLIST"]).encode()
expect("efm.bin", init + parts[0] + parts[1])

# 5) master with 2 variants + encrypted separate audio rendition
KA = rnd(16, 40); aud = [rnd(3000 + i, 41 + i) for i in range(3)]
files["/ma/audio.m3u8"] = "\n".join(["#EXTM3U", "#EXT-X-TARGETDURATION:4", "#EXT-X-MEDIA-SEQUENCE:7",
    '#EXT-X-KEY:METHOD=AES-128,URI="data:text/plain;base64,' + __import__("base64").b64encode(KA).decode() + '"'] +
    sum([["#EXTINF:4,", f"a{i}.aac"] for i in range(3)], []) + ["#EXT-X-ENDLIST"]).encode()
for i in range(3): files[f"/ma/a{i}.aac"] = enc(KA, (7 + i).to_bytes(16, "big"), aud[i])
files["/ma/master.m3u8"] = "\n".join(["#EXTM3U",
    '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="aud",NAME="en",DEFAULT=YES,URI="audio.m3u8"',
    '#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS="avc1.42c01e,mp4a.40.2",AUDIO="aud"', "/aes/index.m3u8",
    '#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720,CODECS="avc1.64001f,mp4a.40.2",AUDIO="aud"', "/aes/index.m3u8"]).encode()
expect("ma_audio.bin", b"".join(aud))

# 6) plain control stream (httpsrc path)
pls = [rnd(5000 + i, 50 + i) for i in range(3)]
for i in range(3): files[f"/plain/p{i}.ts"] = pls[i]
files["/plain/index.m3u8"] = "\n".join(["#EXTM3U", "#EXT-X-TARGETDURATION:4"] + sum([["#EXTINF:4,", f"p{i}.ts"] for i in range(3)], []) + ["#EXT-X-ENDLIST"]).encode()
expect("plain.bin", b"".join(pls))

# 7) DRM only
files["/drm/index.m3u8"] = b'#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI="skd://abc",KEYFORMAT="com.apple.streamingkeydelivery",KEYFORMATVERSIONS="1"\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n'

# 8) first key response is bad (a transient wrong 16 bytes), later ones are right:
#    a cached key that fails the padding check is fetched once more
K4, BAD = rnd(16, 60), rnd(16, 59); r8 = [rnd(2000 + i, 62 + i) for i in range(3)]
for i in range(3): files[f"/rot/s{i}.ts"] = enc(K4, i.to_bytes(16, "big"), r8[i])
files["/rot/index.m3u8"] = "\n".join(["#EXTM3U", "#EXT-X-TARGETDURATION:4", '#EXT-X-KEY:METHOD=AES-128,URI="key"'] +
    sum([["#EXTINF:4,", f"s{i}.ts"] for i in range(3)], []) + ["#EXT-X-ENDLIST"]).encode()
expect("rot.bin", b"".join(r8))
K4a, K4b = BAD, K4

# 9) plain fMP4 whose origin refuses httpsrc's open-ended Range reads (the
#    VOD fallback path): init must stream exactly once, before segment 0
fi = rnd(900, 70); fs = [rnd(3000 + i, 71 + i) for i in range(3)]
files["/fb/init.mp4"] = fi
for i in range(3): files[f"/fb/s{i}.m4s"] = fs[i]
files["/fb/index.m3u8"] = "\n".join(["#EXTM3U", "#EXT-X-TARGETDURATION:4", '#EXT-X-MAP:URI="init.mp4"'] +
    sum([["#EXTINF:4,", f"s{i}.m4s"] for i in range(3)], []) + ["#EXT-X-ENDLIST"]).encode()
refuse_open_range = {"/fb/init.mp4", "/fb/s0.m4s", "/fb/s1.m4s", "/fb/s2.m4s"}
expect("fb.bin", fi + b"".join(fs))

files["/pm/master.m3u8"] = "\n".join(["#EXTM3U",
    '#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS="avc1.42c01e"', "/plain/index.m3u8",
    '#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720,CODECS="avc1.64001f"', "/plain/index.m3u8"]).encode()

# 10) master with several audio renditions (English default, French, German
#     named "Deutsch") plus one in another group that must never be offered
for code, seed in (("en", 80), ("fr", 81), ("de", 82), ("xx", 83)):
    a = [rnd(2500 + i, seed * 10 + i) for i in range(2)]
    for i in range(2): files[f"/ml/{code}{i}.aac"] = a[i]
    files[f"/ml/{code}.m3u8"] = "\n".join(["#EXTM3U", "#EXT-X-TARGETDURATION:4"] +
        sum([["#EXTINF:4,", f"{code}{i}.aac"] for i in range(2)], []) + ["#EXT-X-ENDLIST"]).encode()
    expect(f"ml_{code}.bin", b"".join(a))
files["/ml/master.m3u8"] = "\n".join(["#EXTM3U",
    '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="other",NAME="Decoy",LANGUAGE="fr",URI="xx.m3u8"',
    '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="aud",NAME="English",LANGUAGE="en",DEFAULT=YES,AUTOSELECT=YES,URI="en.m3u8"',
    '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="aud",NAME="Francais",LANGUAGE="fr",URI="fr.m3u8"',
    '#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="aud",NAME="Deutsch",LANGUAGE="de",URI="de.m3u8"',
    '#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS="avc1.42c01e,mp4a.40.2",AUDIO="aud"', "/plain/index.m3u8"]).encode()

# 11) streamed downloads (programme guide): Content-Length, chunked, and
#     no length at all (read to close); 3 MB each
big = rnd(3 * 1024 * 1024 + 17, 90)
files["/stream/known.bin"] = big
expect("stream.bin", big)
streamed = {"/stream/chunked.bin": "chunked", "/stream/eof.bin": "eof"}

# 12) programme guide: a gzip XMLTV file (times around 2026-09-28 12:00 UTC)
#     and a page that is not one
import gzip as _gz
files["/epg/guide.xml.gz"] = _gz.compress(b"""<?xml version="1.0" encoding="UTF-8"?>
<tv>
 <channel id="bbc1.uk"><display-name>BBC One</display-name></channel>
 <channel id="itv1.uk"><display-name>ITV 1</display-name></channel>
 <channel id="c5.uk"><display-name>Channel 5</display-name></channel>
 <programme start="20260928120000 +0000" stop="20260928123000 +0000" channel="bbc1.uk"><title>Noon News</title><desc>Headlines &amp; weather</desc></programme>
 <programme start="20260928123000 +0000" stop="20260928130000 +0000" channel="bbc1.uk"><title>Half past</title></programme>
 <programme start="20260928190000 +0000" stop="20260928200000 +0000" channel="bbc1.uk"><title>Evening</title></programme>
 <programme start="20260928113000 +0000" stop="20260928140000 +0000" channel="itv1.uk"><title>Film</title></programme>
 <programme start="20260928120000 +0000" stop="20260928130000 +0000" channel="c5.uk"><title>Unwanted</title></programme>
</tv>
""")
files["/epg/page.html"] = b"<!doctype html><html><body>Not found</body></html>" + b" " * 70000

# 13) channel logos: /logo/N.png, a 32x20 PNG in logo_driver.c's colour(N)
import struct as _st, zlib as _zl
def _png(w, h, rgb):
    def chunk(t, d): return _st.pack(">I", len(d)) + t + d + _st.pack(">I", _zl.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", _st.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", _zl.compress(raw)) + chunk(b"IEND", b""))
for _i in range(220):
    files[f"/logo/{_i}.png"] = _png(32, 20, ((_i * 37) & 255, (_i * 91) & 255, (_i * 53) & 255))

# 14) a stand-in for iptv-org's playlists (languages/, countries/); no fra
files["/iptv/languages/ara.m3u"] = b"""#EXTM3U
#EXTINF:-1 tvg-id="AsharqDocumentary.sa@SD" group-title="Documentary",Asharq Documentary (1080p)
https://s.example/asharq.m3u8
#EXTINF:-1 tvg-id="SpacetoonArabic.ae@SD" group-title="Animation;Kids",Spacetoon Arabic (720p)
https://s.example/spacetoon.m3u8
#EXTINF:-1 tvg-id="AlJazeera.qa@SD" group-title="News",Al Jazeera (1080p)
https://s.example/aj.m3u8
"""
files["/iptv/languages/eng.m3u"] = b"""#EXTM3U
#EXTINF:-1 tvg-id="NatureTime.us@SD" group-title="Documentary",Nature Time
https://s.example/nature.m3u8
"""
files["/iptv/countries/tn.m3u"] = b"""#EXTM3U
#EXTINF:-1 tvg-id="ElWatania1.tn@SD" group-title="General",El Watania 1 (1080i)
https://s.example/watania1.m3u8
#EXTINF:-1 tvg-id="JawharaTV.tn@SD" group-title="Music",Jawhara TV (720p) [Not 24/7]
https://s.example/jawhara.m3u8
"""

log = open(os.path.join(OUT, "server.log"), "w")
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        path = self.path.split("?")[0]
        if path in streamed:
            log.write(f"GET {path}\n"); log.flush()
            self.send_response(200)
            if streamed[path] == "chunked":
                self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
                off, step = 0, 1
                while off < len(big):
                    part = big[off:off + step]; off += len(part); step = step * 3 + 1 if step < 200000 else 7
                    self.wfile.write(b"%x;ext=1\r\n" % len(part) + part + b"\r\n")
                self.wfile.write(b"0\r\nX-Trailer: 1\r\n\r\n")
            else:
                self.send_header("Connection", "close"); self.end_headers()
                self.wfile.write(big); self.close_connection = True
            return
        if path == "/rot/key":
            state["k_rot"] += 1; body = K4a if state["k_rot"] == 1 else K4b
        else:
            body = files.get(path)
        rng = self.headers.get("Range")
        log.write(f"GET {path} range={rng}\n"); log.flush()
        if body is None:
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        if self.headers.get("Accept-Encoding") == "identity" and path in refuse_open_range:
            self.send_response(403); self.send_header("Content-Length", "0"); self.end_headers(); return
        m = re.match(r"bytes=(\d+)-(\d+)", rng or "")
        if m and path not in ignore_range:
            a, b = int(m.group(1)), min(int(m.group(2)), len(body) - 1)
            self.send_response(206); self.send_header("Content-Range", f"bytes {a}-{b}/{len(body)}")
            body = body[a:b + 1]
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True
srv = S(("127.0.0.1", PORT), H)
open(os.path.join(OUT, "port"), "w").write(str(srv.server_address[1]))
srv.serve_forever()
