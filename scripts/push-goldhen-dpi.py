#!/usr/bin/env python3
"""
Push the current PS4 Cast PKG through GoldHEN's HTTP payload server.

This mirrors DirectPackageInstaller's flow, but avoids the GUI/CLI mismatch on
newer GoldHEN builds:
  1. serve a BGFT JSON manifest on the Mac,
  2. patch DPI's installer payload with Mac IP + metadata callback port,
  3. POST the payload to GoldHEN /payload,
  4. send DPI's package metadata packet when the payload connects back.
"""

from __future__ import annotations

import argparse
import http.server
import json
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PAYLOAD = ROOT / "scripts" / "dpi-payload.bin"


def run(cmd: list[str]) -> str:
    return subprocess.check_output(cmd, text=True).strip()


def current_version() -> str:
    mk = (ROOT / "app/Makefile").read_text()
    for line in mk.splitlines():
        if line.startswith("VERSION"):
            return line.split(":=", 1)[1].strip()
    raise SystemExit("VERSION not found in app/Makefile")


def find_host_ip() -> str:
    for iface in ("en0", "en1"):
        try:
            ip = run(["ipconfig", "getifaddr", iface])
            if ip:
                return ip
        except Exception:
            pass
    return run(["route", "-n", "get", "default"]).split("interface:")[-1].split()[0]


def le_str(s: str) -> bytes:
    data = s.encode("utf-8")
    return struct.pack("<I", len(data)) + data


def pkg_header_digest(pkg: Path) -> str:
    # Matches LibOrbisPkg's HeaderDigest used by DirectPackageInstaller.
    with pkg.open("rb") as f:
        f.seek(0xFE0)
        digest = f.read(32)
    if len(digest) != 32:
        raise SystemExit(f"could not read PKG header digest from {pkg}")
    return digest.hex().upper()


def metadata_packet(manifest_url: str, size: int) -> bytes:
    return b"".join(
        [
            struct.pack("<I", 1),
            le_str(manifest_url),
            le_str(f"PS4 Cast {current_version()}"),
            le_str("IV0000-PCST00001_00-PS4CAST000000001"),
            le_str("PS4GD"),
            struct.pack("<Q", size),
            struct.pack("<I", 0),
        ]
    )


class ManifestServer(threading.Thread):
    def __init__(self, host: str, port: int, pkg_url: str, size: int, digest: str):
        super().__init__(daemon=True)
        manifest = {
            "originalFileSize": size,
            "packageDigest": digest,
            "numberOfSplitFiles": 1,
            "pieces": [
                {
                    "url": pkg_url,
                    "fileOffset": 0,
                    "fileSize": size,
                    "hashValue": "0" * 40,
                }
            ],
        }

        self.hit = threading.Event()

        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802
                if self.path != "/json/0.json":
                    self.send_error(404)
                    return
                parent.hit.set()
                body = json.dumps(manifest, separators=(",", ":")).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, fmt, *args):
                print(f"manifest: {self.address_string()} {fmt % args}")

        self.server = http.server.ThreadingHTTPServer((host, port), Handler)

    def run(self) -> None:
        self.server.serve_forever()

    def stop(self) -> None:
        self.server.shutdown()


class PackageServer(threading.Thread):
    def __init__(self, host: str, port: int, pkg: Path):
        super().__init__(daemon=True)
        self.pkg = pkg
        self.hit = threading.Event()
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def serve_pkg(self, send_body: bool) -> None:
                size = parent.pkg.stat().st_size
                start = 0
                end = size - 1
                status = 200

                rng = self.headers.get("Range", "")
                if rng.startswith("bytes="):
                    spec = rng[6:].split(",", 1)[0].strip()
                    left, _, right = spec.partition("-")
                    try:
                        if left:
                            start = int(left)
                            end = int(right) if right else size - 1
                        elif right:
                            suffix = int(right)
                            start = max(0, size - suffix)
                            end = size - 1
                        if start < 0 or end < start or start >= size:
                            self.send_response(416)
                            self.send_header("Content-Range", f"bytes */{size}")
                            self.end_headers()
                            return
                        end = min(end, size - 1)
                        status = 206
                    except ValueError:
                        self.send_error(400, "bad range")
                        return

                parent.hit.set()
                length = end - start + 1
                self.send_response(status)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Length", str(length))
                if status == 206:
                    self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
                self.end_headers()

                if not send_body:
                    return
                try:
                    with parent.pkg.open("rb") as f:
                        f.seek(start)
                        remaining = length
                        while remaining > 0:
                            chunk = f.read(min(1024 * 1024, remaining))
                            if not chunk:
                                break
                            self.wfile.write(chunk)
                            remaining -= len(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def do_HEAD(self):  # noqa: N802
                if self.path.split("?", 1)[0] != f"/{parent.pkg.name}":
                    self.send_error(404)
                    return
                self.serve_pkg(False)

            def do_GET(self):  # noqa: N802
                if self.path.split("?", 1)[0] != f"/{parent.pkg.name}":
                    self.send_error(404)
                    return
                self.serve_pkg(True)

            def log_message(self, fmt, *args):
                print(f"pkg: {self.address_string()} {fmt % args}")

        self.server = http.server.ThreadingHTTPServer((host, port), Handler)

    def run(self) -> None:
        self.server.serve_forever()

    def stop(self) -> None:
        self.server.shutdown()


class MetadataServer(threading.Thread):
    def __init__(self, packet: bytes):
        super().__init__(daemon=True)
        self.packet = packet
        self.ready = threading.Event()
        self.done = threading.Event()
        self.error: Exception | None = None
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]

    def run(self) -> None:
        self.ready.set()
        try:
            self.sock.settimeout(30)
            conn, addr = self.sock.accept()
            print(f"metadata: PS4 callback from {addr[0]}:{addr[1]}")
            with conn:
                conn.sendall(self.packet)
        except Exception as exc:
            self.error = exc
        finally:
            self.sock.close()
            self.done.set()


def patch_payload(payload_path: Path, host_ip: str, port: int) -> bytes:
    payload = bytearray(payload_path.read_bytes())
    marker = bytes([0xB4]) * 6
    off = payload.find(marker)
    if off < 0:
        raise SystemExit(f"payload marker not found in {payload_path}")
    payload[off : off + 4] = socket.inet_aton(host_ip)
    payload[off + 4 : off + 6] = struct.pack(">H", port)
    return bytes(payload)


def post_payload(ps4_ip: str, payload: bytes) -> tuple[int, bytes]:
    req = urllib.request.Request(
        f"http://{ps4_ip}:9090/payload",
        data=payload,
        method="POST",
        headers={"Content-Type": "application/octet-stream"},
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ps4", default="192.168.1.253")
    ap.add_argument("--host", default=None)
    ap.add_argument("--pkg", default=None)
    ap.add_argument("--pkg-port", type=int, default=8000)
    ap.add_argument("--manifest-port", type=int, default=9898)
    ap.add_argument("--keepalive", type=int, default=120)
    ap.add_argument("--payload", type=Path, default=DEFAULT_PAYLOAD)
    args = ap.parse_args()

    host_ip = args.host or find_host_ip()
    ver = current_version()
    pkg = Path(args.pkg or ROOT / "dist" / f"PS4-Cast-v{ver}.pkg")
    if not pkg.exists():
        raise SystemExit(f"missing package: {pkg}")
    if not args.payload.exists():
        raise SystemExit(f"missing DPI payload: {args.payload}")

    pkg_url = f"http://{host_ip}:{args.pkg_port}/{pkg.name}"
    manifest_url = f"http://{host_ip}:{args.manifest_port}/json/0.json"
    size = pkg.stat().st_size
    digest = pkg_header_digest(pkg)

    pkg_server = PackageServer(host_ip, args.pkg_port, pkg)
    pkg_server.start()

    manifest = ManifestServer(host_ip, args.manifest_port, pkg_url, size, digest)
    manifest.start()

    meta = MetadataServer(metadata_packet(manifest_url, size))
    meta.start()
    meta.ready.wait()

    payload = patch_payload(args.payload, host_ip, meta.port)

    print(f"pkg:      {pkg_url}")
    print(f"manifest: {manifest_url}")
    print(f"digest:   {digest}")
    print(f"callback: {host_ip}:{meta.port}")
    print(f"ps4:      {args.ps4}:9090/payload")

    status, body = post_payload(args.ps4, payload)
    print(f"payload POST -> HTTP {status} {body[:120]!r}")

    meta.done.wait(30)
    if meta.error:
        manifest.stop()
        pkg_server.stop()
        print(f"metadata callback failed: {meta.error}", file=sys.stderr)
        return 1
    if not meta.done.is_set():
        manifest.stop()
        pkg_server.stop()
        print("metadata callback timed out", file=sys.stderr)
        return 1

    print(f"metadata sent; keeping package + manifest servers alive up to {args.keepalive}s")
    if manifest.hit.wait(args.keepalive):
        print("manifest fetched by PS4/BGFT")
        if pkg_server.hit.wait(args.keepalive):
            print("package fetched by PS4/BGFT")
            print(f"keeping package server alive for {args.keepalive}s so BGFT can finish")
            time.sleep(args.keepalive)
        else:
            print("package was not fetched before timeout", file=sys.stderr)
            manifest.stop()
            pkg_server.stop()
            return 1
    else:
        print("manifest was not fetched before timeout", file=sys.stderr)
        manifest.stop()
        pkg_server.stop()
        return 1

    manifest.stop()
    pkg_server.stop()
    print("install request delivered; check PS4 notifications/downloads for progress")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
