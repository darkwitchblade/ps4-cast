#!/usr/bin/env python3
import argparse
import pathlib
import urllib.error
import urllib.request


def main() -> int:
    parser = argparse.ArgumentParser(description="Send a raw payload to GoldHEN's HTTP payload server.")
    parser.add_argument("payload", type=pathlib.Path)
    parser.add_argument("--ps4", default="192.168.1.253")
    parser.add_argument("--port", type=int, default=9090)
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    data = args.payload.read_bytes()
    req = urllib.request.Request(
        f"http://{args.ps4}:{args.port}/payload",
        data=data,
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(data)),
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=args.timeout) as res:
            body = res.read().decode("utf-8", "replace")
            print(f"payload POST -> HTTP {res.status}")
            if body:
                print(body)
            return 0 if 200 <= res.status < 300 else 1
    except urllib.error.HTTPError as exc:
        print(f"payload POST -> HTTP {exc.code}")
        print(exc.read().decode("utf-8", "replace"))
        return 1
    except urllib.error.URLError as exc:
        print(f"payload POST failed: {exc.reason}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
