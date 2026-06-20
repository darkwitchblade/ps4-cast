#!/usr/bin/env python3
"""
Advertise the running PS4 Cast app as a UPnP MediaRenderer from the Mac.

The PS4 app serves the real device description and AVTransport control URLs.
This proxy only handles SSDP discovery so we do not need the crash-prone in-app
SSDP listener while testing phone push-cast behavior.
"""

from __future__ import annotations

import argparse
import http.server
import socket
import struct
import threading
import time
import urllib.request


MCAST_GRP = "239.255.255.250"
MCAST_PORT = 1900
UUID = "uuid:8e8f8d8d-1390-4b55-9f3a-0000000c5702"
STS = (
    UUID,
    "upnp:rootdevice",
    "urn:schemas-upnp-org:device:MediaRenderer:1",
    "urn:schemas-upnp-org:service:ConnectionManager:1",
    "urn:schemas-upnp-org:service:AVTransport:1",
    "urn:schemas-upnp-org:service:RenderingControl:1",
)


def response(location: str, st: str) -> bytes:
    usn = UUID if st == UUID else f"{UUID}::{st}"
    return (
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=120\r\n"
        "DATE: Fri, 19 Jun 2026 00:00:00 GMT\r\n"
        "EXT:\r\n"
        f"LOCATION: {location}\r\n"
        "SERVER: macOS UPnP/1.0 PS4-Cast/1.0\r\n"
        f"ST: {st}\r\n"
        f"USN: {usn}\r\n"
        "BOOTID.UPNP.ORG: 1\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "\r\n"
    ).encode()


def notify(location: str, nts: str, st: str) -> bytes:
    usn = UUID if st == UUID else f"{UUID}::{st}"
    return (
        "NOTIFY * HTTP/1.1\r\n"
        f"HOST: {MCAST_GRP}:{MCAST_PORT}\r\n"
        "CACHE-CONTROL: max-age=120\r\n"
        f"LOCATION: {location}\r\n"
        "SERVER: macOS UPnP/1.0 PS4-Cast/1.0\r\n"
        f"NT: {st}\r\n"
        f"NTS: {nts}\r\n"
        f"USN: {usn}\r\n"
        "\r\n"
    ).encode()


def device_xml(host: str, port: int) -> bytes:
    return f"""<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>
    <friendlyName>PS4 Cast</friendlyName>
    <manufacturer>ps4-cast</manufacturer>
    <modelName>PS4 Cast Receiver</modelName>
    <UDN>{UUID}</UDN>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>
        <SCPDURL>/AVTransport.xml</SCPDURL>
        <controlURL>/upnp/control/AVTransport</controlURL>
        <eventSubURL>/upnp/event/AVTransport</eventSubURL>
      </service>
      <service>
        <serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>
        <SCPDURL>/RenderingControl.xml</SCPDURL>
        <controlURL>/upnp/control/RenderingControl</controlURL>
        <eventSubURL>/upnp/event/RenderingControl</eventSubURL>
      </service>
      <service>
        <serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>
        <SCPDURL>/ConnectionManager.xml</SCPDURL>
        <controlURL>/upnp/control/ConnectionManager</controlURL>
        <eventSubURL>/upnp/event/ConnectionManager</eventSubURL>
      </service>
    </serviceList>
  </device>
</root>""".encode()


AVTRANSPORT_XML = b"""<?xml version="1.0"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <actionList>
    <action><name>SetAVTransportURI</name></action>
    <action><name>Play</name></action>
    <action><name>Stop</name></action>
    <action><name>Pause</name></action>
    <action><name>Seek</name></action>
    <action><name>GetTransportInfo</name></action>
    <action><name>GetPositionInfo</name></action>
    <action><name>GetMediaInfo</name></action>
  </actionList>
  <serviceStateTable>
    <stateVariable sendEvents="yes"><name>TransportState</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>TransportStatus</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>AVTransportURI</name><dataType>string</dataType></stateVariable>
  </serviceStateTable>
</scpd>"""


RENDERING_XML = b"""<?xml version="1.0"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <actionList>
    <action><name>GetVolume</name></action>
    <action><name>SetVolume</name></action>
    <action><name>GetMute</name></action>
    <action><name>SetMute</name></action>
  </actionList>
  <serviceStateTable>
    <stateVariable sendEvents="yes"><name>Volume</name><dataType>ui2</dataType></stateVariable>
    <stateVariable sendEvents="yes"><name>Mute</name><dataType>boolean</dataType></stateVariable>
  </serviceStateTable>
</scpd>"""


CONNECTION_XML = b"""<?xml version="1.0"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <actionList>
    <action><name>GetProtocolInfo</name></action>
    <action><name>GetCurrentConnectionIDs</name></action>
    <action><name>GetCurrentConnectionInfo</name></action>
  </actionList>
  <serviceStateTable>
    <stateVariable sendEvents="yes"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="yes"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="yes"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>
  </serviceStateTable>
</scpd>"""


def soap_response(action: str, service: str, inner: str = "") -> bytes:
    return (
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        f"<s:Body><u:{action}Response xmlns:u=\"urn:schemas-upnp-org:service:{service}:1\">"
        f"{inner}</u:{action}Response></s:Body></s:Envelope>"
    ).encode()


class ControlProxy(threading.Thread):
    def __init__(self, host: str, port: int, ps4: str, ps4_port: int):
        super().__init__(daemon=True)
        self.location_host = host
        self.location_port = port
        self.ps4 = ps4
        self.ps4_port = ps4_port
        parent = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def send_body(self, ctype: str, body: bytes, status: int = 200) -> None:
                self.send_response(status)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):  # noqa: N802
                print(f"GET  {self.client_address[0]} {self.path}", flush=True)
                if self.path == "/description.xml":
                    self.send_body("text/xml; charset=utf-8", device_xml(parent.location_host, parent.location_port))
                elif self.path == "/AVTransport.xml":
                    self.send_body("text/xml; charset=utf-8", AVTRANSPORT_XML)
                elif self.path == "/RenderingControl.xml":
                    self.send_body("text/xml; charset=utf-8", RENDERING_XML)
                elif self.path == "/ConnectionManager.xml":
                    self.send_body("text/xml; charset=utf-8", CONNECTION_XML)
                else:
                    self.send_error(404)

            def do_POST(self):  # noqa: N802
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                soap = self.headers.get("SOAPACTION", "")
                print(f"SOAP {self.client_address[0]} {self.path} {soap}", flush=True)
                if self.path == "/upnp/control/ConnectionManager":
                    if "GetProtocolInfo" in soap:
                        inner = (
                            "<Source></Source>"
                            "<Sink>http-get:*:video/mp4:*,http-get:*:video/x-matroska:*,http-get:*:video/*:*</Sink>"
                        )
                        self.send_body("text/xml; charset=utf-8", soap_response("GetProtocolInfo", "ConnectionManager", inner))
                    elif "GetCurrentConnectionIDs" in soap:
                        self.send_body("text/xml; charset=utf-8", soap_response("GetCurrentConnectionIDs", "ConnectionManager", "<ConnectionIDs>0</ConnectionIDs>"))
                    else:
                        self.send_body("text/xml; charset=utf-8", soap_response("GetCurrentConnectionInfo", "ConnectionManager"))
                    return

                if self.path.startswith("/upnp/control/"):
                    url = f"http://{parent.ps4}:{parent.ps4_port}{self.path}"
                    req = urllib.request.Request(url, data=body, method="POST", headers={
                        "Content-Type": self.headers.get("Content-Type", "text/xml; charset=utf-8"),
                        "SOAPACTION": soap,
                    })
                    with urllib.request.urlopen(req, timeout=8) as resp:
                        self.send_body(resp.headers.get("Content-Type", "text/xml; charset=utf-8"), resp.read(), resp.status)
                    return
                self.send_error(404)

            def log_message(self, fmt, *args):
                return

        self.server = http.server.ThreadingHTTPServer((host, port), Handler)

    def run(self) -> None:
        self.server.serve_forever()


def wants(req: str, st: str) -> bool:
    upper = req.upper()
    target = st.upper()
    return "SSDP:DISCOVER" in upper and (
        "SSDP:ALL" in upper
        or "UPNP:ROOTDEVICE" in upper
        or UUID.upper() in upper
        or "CONNECTIONMANAGER" in upper
        or "MEDIARENDERER" in upper
        or "AVTRANSPORT" in upper
        or target in upper
    )


def requested_st(req: str) -> str:
    for line in req.splitlines():
        if line.upper().startswith("ST:"):
            return line.split(":", 1)[1].strip()
    return "ssdp:all"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ps4", default="192.168.1.253")
    ap.add_argument("--ps4-port", type=int, default=8080)
    ap.add_argument("--host", default="192.168.1.139")
    ap.add_argument("--port", type=int, default=8090)
    ap.add_argument("--interval", type=float, default=20.0)
    args = ap.parse_args()

    location = f"http://{args.host}:{args.port}/description.xml"
    control = ControlProxy(args.host, args.port, args.ps4, args.ps4_port)
    control.start()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except OSError:
        pass
    sock.bind(("", MCAST_PORT))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, struct.pack("4sl", socket.inet_aton(MCAST_GRP), socket.INADDR_ANY))
    sock.settimeout(1.0)

    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    out.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)

    print(f"Advertising PS4 Cast at {location}")
    print(f"Forwarding control to http://{args.ps4}:{args.ps4_port}")
    print("Leave this running, then scan/cast from the phone.")

    last_alive = 0.0
    while True:
        now = time.time()
        if now - last_alive >= args.interval:
            for st in STS:
                out.sendto(notify(location, "ssdp:alive", st), (MCAST_GRP, MCAST_PORT))
            last_alive = now

        try:
            data, addr = sock.recvfrom(4096)
        except socket.timeout:
            continue
        req = data.decode("utf-8", "ignore")
        if "M-SEARCH" not in req.upper():
            continue
        asked = requested_st(req)
        print(f"M-SEARCH {addr[0]}:{addr[1]} ST={asked}", flush=True)
        matched = [st for st in STS if wants(req, st)]
        if asked and asked.lower() not in ("ssdp:all", "upnp:rootdevice"):
            matched = [st for st in matched if st.lower() == asked.lower()] or matched
        for st in matched:
            sock.sendto(response(location, st), addr)
            print(f"{addr[0]}:{addr[1]} -> {st}", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
