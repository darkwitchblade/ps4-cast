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
import http.client
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


def close_running_app(ps4: str, attempts: int = 5) -> bool:
    """Close PS4 Cast cleanly before install.

    GoldHEN's :9090 loader is ONE-SHOT per rearm. If the DPI payload runs while
    the old build is still up, the install fails, the resident :9192 agent never
    starts, and the rearm is burned -- the next attempt then needs a manual
    console-side rearm ("GoldHEN server sometimes unresponsive"). Closing first
    is what makes a bootstrap attempt reliable.
    """
    status_url = f"http://{ps4}:8080/status"
    for i in range(attempts):
        try:
            with urllib.request.urlopen(status_url, timeout=3) as resp:
                resp.read()
        except Exception:
            return True  # not running (or unreachable): safe to proceed
        print(f"app running; close attempt {i + 1}: POST /quit")
        try:
            req = urllib.request.Request(f"http://{ps4}:8080/quit", data=b"", method="POST")
            urllib.request.urlopen(req, timeout=5).read()
        except Exception:
            pass
        time.sleep(4)
    try:
        urllib.request.urlopen(status_url, timeout=3).read()
    except Exception:
        return True
    print("WARNING: app still responding on :8080 after close attempts", file=sys.stderr)
    return False


class InstallHTTPServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        # BGFT closes validation/keep-alive sockets without an HTTP shutdown.
        # That is normal transport behavior, not an installer failure.
        exc = sys.exc_info()[1]
        if isinstance(exc, (BrokenPipeError, ConnectionResetError)):
            return
        super().handle_error(request, client_address)


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
            protocol_version = "HTTP/1.1"

            def do_GET(self):  # noqa: N802
                if self.path != "/json/0.json":
                    self.send_error(404)
                    return
                parent.hit.set()
                body = json.dumps(manifest, separators=(",", ":")).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)
                self.wfile.flush()
                self.close_connection = True

            def log_message(self, fmt, *args):
                print(f"manifest: {self.address_string()} {fmt % args}")

        self.server = InstallHTTPServer((host, port), Handler)

    def run(self) -> None:
        self.server.serve_forever()

    def stop(self) -> None:
        self.server.shutdown()


class PackageServer(threading.Thread):
    def __init__(self, host: str, port: int, pkg: Path):
        super().__init__(daemon=True)
        self.pkg = pkg
        self.hit = threading.Event()
        self.done = threading.Event()
        self.bytes_sent = 0
        self.last_error = ""
        self.ranges: list[tuple[int, int]] = []
        self.lock = threading.Lock()
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

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
                self.send_header("Accept-Ranges", "none")
                self.send_header("Connection", "Keep-Alive")
                self.send_header(
                    "Content-Disposition", 'attachment; filename="app.pkg"'
                )
                self.send_header("Content-Length", str(length))
                if status == 206:
                    self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
                self.end_headers()

                if not send_body:
                    return
                complete = False
                sent = 0
                try:
                    self.connection.settimeout(30)
                    with parent.pkg.open("rb") as f:
                        f.seek(start)
                        remaining = length
                        while remaining > 0:
                            chunk = f.read(min(64 * 1024, remaining))
                            if not chunk:
                                break
                            self.wfile.write(chunk)
                            wrote = len(chunk)
                            sent += wrote
                            remaining -= wrote
                        self.wfile.flush()
                        complete = (remaining == 0)
                except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError) as exc:
                    with parent.lock:
                        parent.last_error = f"{type(exc).__name__}: {exc}"
                with parent.lock:
                    parent.bytes_sent += sent
                    if complete:
                        merged: list[tuple[int, int]] = []
                        for left, right in sorted(parent.ranges + [(start, end)]):
                            if not merged or left > merged[-1][1] + 1:
                                merged.append((left, right))
                            else:
                                merged[-1] = (merged[-1][0], max(merged[-1][1], right))
                        parent.ranges = merged
                        if len(merged) == 1 and merged[0] == (0, size - 1):
                            parent.done.set()
                print(
                    f"pkg body: sent {sent}/{length} bytes"
                    + (" (complete)" if complete else " (aborted)")
                )

            def do_HEAD(self):  # noqa: N802
                if self.path.split("?", 1)[0] != "/file/":
                    self.send_error(404)
                    return
                self.serve_pkg(False)

            def do_GET(self):  # noqa: N802
                if self.path.split("?", 1)[0] != "/file/":
                    self.send_error(404)
                    return
                self.serve_pkg(True)

            def log_message(self, fmt, *args):
                print(f"pkg: {self.address_string()} {fmt % args}")

        self.server = InstallHTTPServer((host, port), Handler)

    def run(self) -> None:
        self.server.serve_forever()

    def stop(self) -> None:
        self.server.shutdown()


class AgentUnavailable(ConnectionError):
    pass


def send_agent_command(
    ps4_ip: str,
    port: int,
    packet: bytes,
    connect_timeout: float,
    status_timeout: int,
) -> str:
    deadline = time.monotonic() + connect_timeout
    last_error: OSError | None = None
    sock: socket.socket | None = None
    while sock is None:
        try:
            sock = socket.create_connection(
                (ps4_ip, port), timeout=min(0.5, max(0.05, deadline - time.monotonic()))
            )
        except OSError as exc:
            last_error = exc
            if time.monotonic() >= deadline:
                raise AgentUnavailable(str(last_error)) from last_error
            time.sleep(0.2)

    with sock:
        print(f"agent: connected to {ps4_ip}:{port}")
        sock.sendall(packet)
        sock.shutdown(socket.SHUT_WR)
        sock.settimeout(status_timeout)
        chunks: list[bytes] = []
        while sum(len(chunk) for chunk in chunks) < 8192:
            data = sock.recv(4096)
            if not data:
                break
            chunks.append(data)
        if not chunks:
            raise ConnectionError("resident agent closed without install status")
        return b"".join(chunks).decode("utf-8", "replace").strip()


def post_payload(ps4_ip: str, payload: bytes) -> tuple[int | None, bytes]:
    req = urllib.request.Request(
        f"http://{ps4_ip}:9090/payload",
        data=payload,
        method="POST",
        headers={"Content-Type": "application/octet-stream"},
    )
    try:
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(req, timeout=15) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()
    except (urllib.error.URLError, http.client.HTTPException, TimeoutError, ConnectionError) as exc:
        # Some GoldHEN builds execute the uploaded payload and close the socket
        # without returning an HTTP response. The metadata callback below is the
        # authoritative acknowledgement.
        print(f"payload POST returned no HTTP acknowledgement: {exc}")
        return None, b""


def wait_for_goldhen(ps4: str, port: int, timeout_s: int) -> bool:
    """Wait until GoldHEN's payload listener accepts connections.

    The payload server is armed from the console UI and (on this setup) does
    not survive a title close -- so after the app-close pre-flight the port is
    often DOWN again even though it was up a minute ago. Poll instead of
    failing, prompting for one console-side rearm when needed.
    """
    deadline = time.time() + timeout_s
    prompted = False
    while time.time() < deadline:
        try:
            with socket.create_connection((ps4, port), timeout=1):
                return True
        except OSError:
            pass
        if not prompted:
            print(f"GoldHEN :{port} is down (it does not survive an app close).")
            print(">> On the PS4: Settings -> GoldHEN -> Payload Server -> enable/re-arm now.")
            prompted = True
        time.sleep(2)
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ps4", default="192.168.1.253")
    ap.add_argument("--host", default=None)
    ap.add_argument("--pkg", default=None)
    ap.add_argument("--pkg-port", type=int, default=8000)
    ap.add_argument("--manifest-port", type=int, default=9898)
    ap.add_argument("--agent-port", type=int, default=9192)
    ap.add_argument("--ready-timeout", type=int, default=180)
    ap.add_argument("--payload", type=Path, default=DEFAULT_PAYLOAD)
    ap.add_argument("--no-close", action="store_true",
                    help="skip the pre-bootstrap app-close (not recommended)")
    ap.add_argument("--rearm-wait", type=int, default=180,
                    help="seconds to wait for a console-side GoldHEN rearm")
    args = ap.parse_args()

    host_ip = args.host or find_host_ip()
    ver = current_version()
    pkg = Path(args.pkg or ROOT / "dist" / f"PS4-Cast-v{ver}.pkg")
    if not pkg.exists():
        raise SystemExit(f"missing package: {pkg}")
    if not args.payload.exists():
        raise SystemExit(f"missing DPI payload: {args.payload}")

    pkg_url = f"http://{host_ip}:{args.pkg_port}/file/"
    manifest_url = f"http://{host_ip}:{args.manifest_port}/json/0.json"
    size = pkg.stat().st_size
    digest = pkg_header_digest(pkg)

    pkg_server = PackageServer(host_ip, args.pkg_port, pkg)
    pkg_server.start()

    manifest = ManifestServer(host_ip, args.manifest_port, pkg_url, size, digest)
    manifest.start()

    print(f"pkg:      {pkg_url}")
    print(f"manifest: {manifest_url}")
    print(f"digest:   {digest}")
    print(f"agent:    {args.ps4}:{args.agent_port}")

    packet = metadata_packet(manifest_url, size)
    try:
        try:
            install_status = send_agent_command(
                args.ps4, args.agent_port, packet, 0.5, args.ready_timeout
            )
            print("agent: reused resident deployment service")
        except AgentUnavailable:
            if not args.no_close:
                print("agent: not resident; closing the running app before bootstrap")
                close_running_app(args.ps4)
            else:
                print("agent: not resident; --no-close set, skipping app-close pre-flight")
            if not wait_for_goldhen(args.ps4, 9090, args.rearm_wait):
                print("GoldHEN :9090 never came up; aborting without burning anything.", file=sys.stderr)
                return 1
            print("bootstrapping once through GoldHEN :9090")
            status, body = post_payload(args.ps4, args.payload.read_bytes())
            if status is not None:
                print(f"payload POST -> HTTP {status} {body[:120]!r}")
            install_status = send_agent_command(
                args.ps4, args.agent_port, packet, 20.0, args.ready_timeout
            )
            print("agent: resident deployment service bootstrapped")

        print(f"install status: {install_status}")
        if not install_status.startswith("READY "):
            print(f"install did not become ready: {install_status}", file=sys.stderr)
            return 1

        if not manifest.hit.wait(2):
            print("agent reported ready without fetching the manifest", file=sys.stderr)
            return 1
        print("manifest fetched by PS4/BGFT")
        if not pkg_server.hit.wait(2):
            print("agent reported ready without fetching the package", file=sys.stderr)
            return 1
        print("package fetched by PS4/BGFT")
        if not pkg_server.done.wait(2):
            with pkg_server.lock:
                sent = pkg_server.bytes_sent
                detail = pkg_server.last_error or "PS4 stopped reading"
            print(
                "package range coverage was not contiguous "
                f"({sent} bytes served; {detail}); AppInstUtil READY is authoritative"
            )
        else:
            print("package transfer complete")
        print(f"install ready: {install_status}")
        print("install request delivered and verified ready")
        return 0
    except (AgentUnavailable, ConnectionError, TimeoutError, OSError) as exc:
        print(f"resident agent unavailable: {exc}", file=sys.stderr)
        print(
            "Bootstrap requires one GoldHEN :9090 rearm after a PS4 reboot; "
            f"subsequent deploys reuse :{args.agent_port}.",
            file=sys.stderr,
        )
        return 1
    finally:
        manifest.stop()
        pkg_server.stop()


if __name__ == "__main__":
    raise SystemExit(main())
