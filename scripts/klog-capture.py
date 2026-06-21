#!/usr/bin/env python3
"""Capture the PS4 kernel/system log (GoldHEN klog, TCP :3232) with host
timestamps, and flag lines that look like faults — GPU exceptions, VM-runtime
errors, app crashes, the 'error has occurred' dialog triggers, panics.

This is the visibility channel /status can't give us: it sees DISPLAY/GPU faults
and system-level app crashes even when the app's own HTTP threads stay alive.

Usage:
  scripts/klog-capture.py --ps4 192.168.1.33 [--out /tmp/ps4klog.log] [--seconds N]
  # runs until --seconds elapse (default: forever / until Ctrl-C or killed)
Faults are printed to stdout prefixed with '!! FAULT'; everything goes to --out.
"""
import argparse, socket, sys, time, re

FAULT_PATTERNS = [
    r"SYSTEM_VM_RUNTIME", r"0xa0028401",
    r"\bGPU\b.*(fault|exception|hang|timeout|lockup)", r"gpu.*(fault|exception)",
    r"PGRAPH", r"submitDone", r"SUBMITDONE", r"GnmSubmit",
    r"VM[_ ]fault", r"page fault", r"access violation",
    r"abnormal", r"exception", r"core dump", r"coredump", r"\bpanic\b", r"Fatal",
    r"CE-\d", r"error has occurred", r"SceShellUI.*error", r"crash",
    r"CrashReport", r"coredump_", r"NotificationRequest.*rror",
    r"PCST00001", r"ps4cast", r"IV0000",          # our app's ids
    r"killed by signal", r"SIGSEGV", r"SIGBUS", r"SIGILL", r"Trap",
]
FAULT_RE = re.compile("|".join(FAULT_PATTERNS), re.IGNORECASE)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ps4", default="192.168.1.33")
    ap.add_argument("--port", type=int, default=3232)
    ap.add_argument("--out", default="/tmp/ps4klog.log")
    ap.add_argument("--seconds", type=float, default=0.0)  # 0 = run until killed
    args = ap.parse_args()

    deadline = (time.time() + args.seconds) if args.seconds > 0 else None
    try:
        s = socket.create_connection((args.ps4, args.port), timeout=6)
    except OSError as e:
        print(f"klog connect failed: {e}", file=sys.stderr)
        return 2
    s.settimeout(2.0)
    nfault = 0
    with open(args.out, "a", buffering=1) as f:
        f.write(f"==== klog capture start {time.strftime('%H:%M:%S')} ps4={args.ps4} ====\n")
        buf = b""
        while deadline is None or time.time() < deadline:
            try:
                d = s.recv(8192)
                if not d:
                    f.write("==== klog socket closed ====\n"); break
                buf += d
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    txt = line.decode("utf-8", "replace").rstrip("\r")
                    ts = time.strftime("%H:%M:%S")
                    f.write(f"[{ts}] {txt}\n")
                    if FAULT_RE.search(txt):
                        nfault += 1
                        print(f"!! FAULT [{ts}] {txt}")
            except socket.timeout:
                continue
            except OSError as e:
                f.write(f"==== klog recv error: {e} ====\n"); break
    s.close()
    print(f"klog capture ended; faults flagged: {nfault}; log: {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
