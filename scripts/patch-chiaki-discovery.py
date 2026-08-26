#!/usr/bin/env python3
"""Patch chiaki-ng 1.10.0's null system-version discovery crash.

FW 11 can answer Remote Play discovery without a system-version header. The
pinned macOS build passes that NULL field to atoi(). This dedicated development
copy only targets a PS4, so return Chiaki's PS4 10+ protocol target directly.
"""

import hashlib
import pathlib
import shutil
import sys


EXPECTED_SHA256 = "370d44be0eec63761ac37e94d9505cbca8299a6d35d03399863c20b669d4ef54"
FUNCTION_OFFSET = 0xD48AC
EXPECTED = bytes.fromhex("f657bda9f44f01a9")
# mov w0, #1000 (CHIAKI_TARGET_PS4_10); ret
PATCHED = bytes.fromhex("007d8052c0035fd6")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} /path/to/chiaki", file=sys.stderr)
        return 2

    binary = pathlib.Path(sys.argv[1])
    data = bytearray(binary.read_bytes())
    current = bytes(data[FUNCTION_OFFSET : FUNCTION_OFFSET + len(PATCHED)])
    if current == PATCHED:
        print("chiaki-ng discovery guard is already installed")
        return 0

    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_SHA256 or current != EXPECTED:
        print(
            "Refusing to patch an unknown chiaki-ng binary "
            f"(sha256={digest}, bytes={current.hex()}).",
            file=sys.stderr,
        )
        return 1

    backup = binary.parents[3] / "chiaki-ng-v1.10.0.unpatched"
    if not backup.exists():
        shutil.copy2(binary, backup)

    data[FUNCTION_OFFSET : FUNCTION_OFFSET + len(PATCHED)] = PATCHED
    binary.write_bytes(data)
    print("Installed chiaki-ng FW 11 discovery null guard")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
