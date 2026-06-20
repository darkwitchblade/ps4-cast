#!/usr/bin/env python3
import argparse
import http.server
import os
import re
import socket
import threading
import time
import urllib.parse
from pathlib import Path

SSDP_ADDR = ("239.255.255.250", 1900)
UUID = "uuid:ps4-cast-mac-dms"


def xml_escape(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def soap_value(body: str, name: str, default: str = "") -> str:
    match = re.search(rf"<(?:\w+:)?{name}>(.*?)</(?:\w+:)?{name}>", body, re.DOTALL)
    return match.group(1).strip() if match else default


def soap_action(body: str) -> str:
    match = re.search(r"<s:Body[^>]*>\s*<(?:\w+:)?([A-Za-z0-9_]+)", body, re.DOTALL)
    return match.group(1) if match else "unknown"


class State:
    def __init__(self, host: str, port: int, media: Path, title: str):
        self.host = host
        self.port = port
        self.media = media
        self.title = title

    @property
    def base(self) -> str:
        return f"http://{self.host}:{self.port}"

    @property
    def media_url(self) -> str:
        return f"{self.base}/media/{urllib.parse.quote(self.media.name)}"

    @property
    def item_didl(self) -> str:
        return f"""<item id="1" parentID="0" restricted="1">
<dc:title>{xml_escape(self.title)}</dc:title>
<upnp:class>object.item.videoItem.movie</upnp:class>
<res protocolInfo="http-get:*:video/mp4:DLNA.ORG_OP=01;DLNA.ORG_CI=0" size="{self.media.stat().st_size}">{xml_escape(self.media_url)}</res>
</item>"""

    @property
    def root_didl(self) -> str:
        return """<container id="0" parentID="-1" restricted="1" searchable="0" childCount="1">
<dc:title>PS4 Cast</dc:title>
<upnp:class>object.container.storageFolder</upnp:class>
</container>"""


class Handler(http.server.BaseHTTPRequestHandler):
    state: State

    def log_message(self, fmt, *args):
        print("%s - - [%s] %s" % (self.client_address[0], self.log_date_time_string(), fmt % args), flush=True)

    def send_bytes(self, body: bytes, ctype: str):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/description.xml":
            body = f"""<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <device>
    <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
    <friendlyName>PS4 Cast Server</friendlyName>
    <manufacturer>ps4-cast</manufacturer>
    <modelName>PS4 Cast DLNA Server</modelName>
    <UDN>{UUID}</UDN>
    <serviceList>
      <service>
        <serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:ContentDirectory</serviceId>
        <SCPDURL>/ContentDirectory.xml</SCPDURL>
        <controlURL>/ctl/ContentDirectory</controlURL>
        <eventSubURL>/evt/ContentDirectory</eventSubURL>
      </service>
      <service>
        <serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>
        <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>
        <SCPDURL>/ConnectionManager.xml</SCPDURL>
        <controlURL>/ctl/ConnectionManager</controlURL>
        <eventSubURL>/evt/ConnectionManager</eventSubURL>
      </service>
    </serviceList>
  </device>
</root>""".encode()
            self.send_bytes(body, "text/xml; charset=utf-8")
            return
        if path == "/ContentDirectory.xml":
            body = b"""<?xml version="1.0"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
  <specVersion><major>1</major><minor>0</minor></specVersion>
  <actionList>
    <action>
      <name>Browse</name>
      <argumentList>
        <argument><name>ObjectID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument>
        <argument><name>BrowseFlag</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_BrowseFlag</relatedStateVariable></argument>
        <argument><name>Filter</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Filter</relatedStateVariable></argument>
        <argument><name>StartingIndex</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Index</relatedStateVariable></argument>
        <argument><name>RequestedCount</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
        <argument><name>SortCriteria</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SortCriteria</relatedStateVariable></argument>
        <argument><name>Result</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>
        <argument><name>NumberReturned</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
        <argument><name>TotalMatches</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
        <argument><name>UpdateID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_UpdateID</relatedStateVariable></argument>
      </argumentList>
    </action>
    <action><name>GetSearchCapabilities</name><argumentList><argument><name>SearchCaps</name><direction>out</direction><relatedStateVariable>SearchCapabilities</relatedStateVariable></argument></argumentList></action>
    <action><name>GetSortCapabilities</name><argumentList><argument><name>SortCaps</name><direction>out</direction><relatedStateVariable>SortCapabilities</relatedStateVariable></argument></argumentList></action>
    <action><name>GetSystemUpdateID</name><argumentList><argument><name>Id</name><direction>out</direction><relatedStateVariable>SystemUpdateID</relatedStateVariable></argument></argumentList></action>
  </actionList>
  <serviceStateTable>
    <stateVariable sendEvents="yes"><name>SystemUpdateID</name><dataType>ui4</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>SearchCapabilities</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>SortCapabilities</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_ObjectID</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_Result</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_SearchCriteria</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_BrowseFlag</name><dataType>string</dataType><allowedValueList><allowedValue>BrowseMetadata</allowedValue><allowedValue>BrowseDirectChildren</allowedValue></allowedValueList></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_Filter</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_SortCriteria</name><dataType>string</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_Index</name><dataType>ui4</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_Count</name><dataType>ui4</dataType></stateVariable>
    <stateVariable sendEvents="no"><name>A_ARG_TYPE_UpdateID</name><dataType>ui4</dataType></stateVariable>
  </serviceStateTable>
</scpd>"""
            self.send_bytes(body, "text/xml; charset=utf-8")
            return
        if path == "/ConnectionManager.xml":
            body = b"""<?xml version="1.0"?><scpd xmlns="urn:schemas-upnp-org:service-1-0"><specVersion><major>1</major><minor>0</minor></specVersion><actionList><action><name>GetProtocolInfo</name><argumentList><argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument><argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument></argumentList></action></actionList><serviceStateTable><stateVariable sendEvents="no"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable><stateVariable sendEvents="no"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable></serviceStateTable></scpd>"""
            self.send_bytes(body, "text/xml; charset=utf-8")
            return
        if path.startswith("/media/"):
            fs = self.state.media
            size = fs.stat().st_size
            range_header = self.headers.get("Range", "")
            start = 0
            end = size - 1
            status = 200
            if range_header.startswith("bytes="):
                spec = range_header[6:].split(",", 1)[0].strip()
                if spec.startswith("-"):
                    suffix = int(spec[1:] or "0")
                    start = max(0, size - suffix)
                else:
                    first, _, last = spec.partition("-")
                    start = int(first or "0")
                    if last:
                        end = int(last)
                start = max(0, min(start, size - 1))
                end = max(start, min(end, size - 1))
                status = 206
            length = end - start + 1
            self.send_response(status)
            self.send_header("Content-Type", "video/mp4")
            self.send_header("Content-Length", str(length))
            self.send_header("Accept-Ranges", "bytes")
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            with fs.open("rb") as fh:
                fh.seek(start)
                remaining = length
                while remaining:
                    chunk = fh.read(min(1024 * 256, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
            return
        self.send_error(404)

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        length = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(length).decode("utf-8", "replace")
        if path == "/ctl/ContentDirectory":
            action = soap_action(body)
            object_id = soap_value(body, "ObjectID", "0")
            browse_flag = soap_value(body, "BrowseFlag", "")
            print(f"SOAP ContentDirectory action={action!r} object={object_id!r} flag={browse_flag!r}", flush=True)
            if "GetSearchCapabilities" in body:
                soap = b"""<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:GetSearchCapabilitiesResponse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1"><SearchCaps>dc:title,upnp:class</SearchCaps></u:GetSearchCapabilitiesResponse></s:Body></s:Envelope>"""
                self.send_bytes(soap, 'text/xml; charset="utf-8"')
                return
            if "GetSortCapabilities" in body:
                soap = b"""<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:GetSortCapabilitiesResponse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1"><SortCaps>dc:title</SortCaps></u:GetSortCapabilitiesResponse></s:Body></s:Envelope>"""
                self.send_bytes(soap, 'text/xml; charset="utf-8"')
                return
            if "GetSystemUpdateID" in body:
                soap = b"""<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:GetSystemUpdateIDResponse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1"><Id>1</Id></u:GetSystemUpdateIDResponse></s:Body></s:Envelope>"""
                self.send_bytes(soap, 'text/xml; charset="utf-8"')
                return
            if object_id == "0" and browse_flag == "BrowseMetadata":
                entries = self.state.root_didl
            elif object_id == "0":
                entries = self.state.item_didl
            elif object_id == "1":
                entries = self.state.item_didl
            else:
                entries = ""
            returned = 1 if entries else 0
            total = 1 if object_id in ("0", "1") else 0
            didl = f"""<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">
{entries}
</DIDL-Lite>"""
            result = xml_escape(didl)
            soap = f"""<?xml version="1.0"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">
<s:Body><u:BrowseResponse xmlns:u="urn:schemas-upnp-org:service:ContentDirectory:1">
<Result>{result}</Result><NumberReturned>{returned}</NumberReturned><TotalMatches>{total}</TotalMatches><UpdateID>1</UpdateID>
</u:BrowseResponse></s:Body></s:Envelope>""".encode()
            self.send_bytes(soap, 'text/xml; charset="utf-8"')
            return
        if path == "/ctl/ConnectionManager" and "GetProtocolInfo" in body:
            soap = b"""<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:GetProtocolInfoResponse xmlns:u="urn:schemas-upnp-org:service:ConnectionManager:1"><Source>http-get:*:video/mp4:*</Source><Sink></Sink></u:GetProtocolInfoResponse></s:Body></s:Envelope>"""
            self.send_bytes(soap, 'text/xml; charset="utf-8"')
            return
        self.send_error(404)


def ssdp_loop(state: State):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    sock.bind(("", 1900))
    group = socket.inet_aton(SSDP_ADDR[0])
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, group + socket.inet_aton("0.0.0.0"))
    targets = ["upnp:rootdevice", "urn:schemas-upnp-org:device:MediaServer:1", "urn:schemas-upnp-org:service:ContentDirectory:1"]
    while True:
        data, addr = sock.recvfrom(2048)
        text = data.decode("utf-8", "ignore").upper()
        if "M-SEARCH" not in text:
            continue
        for st in targets:
            if "SSDP:ALL" not in text and st.upper() not in text and "UPNP:ROOTDEVICE" not in text:
                continue
            resp = "\r\n".join([
                "HTTP/1.1 200 OK",
                "CACHE-CONTROL: max-age=1800",
                "EXT:",
                f"LOCATION: {state.base}/description.xml",
                "SERVER: macOS UPnP/1.0 PS4-Cast-DMS/1.0",
                f"ST: {st}",
                f"USN: {UUID}::{st}",
                "",
                "",
            ]).encode()
            sock.sendto(resp, addr)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.139")
    ap.add_argument("--port", type=int, default=8091)
    ap.add_argument("--media", type=Path, default=Path("app/assets/avtest.mp4"))
    ap.add_argument("--title", default="PS4 Cast Test Video")
    args = ap.parse_args()
    state = State(args.host, args.port, args.media.resolve(), args.title)
    Handler.state = state
    threading.Thread(target=ssdp_loop, args=(state,), daemon=True).start()
    print(f"DLNA server: {state.base}/description.xml", flush=True)
    print(f"Media URL:   {state.media_url}", flush=True)
    http.server.ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    raise SystemExit(main())
