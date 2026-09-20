"""Validate the local partition table and package only the Launcher app image."""
import hashlib
import json
import shutil
import struct
from pathlib import Path

VERSION = "1.8"
ROOT = Path(__file__).resolve().parent
BUILD = ROOT / ".pio/build/s3ai-dotmic"
DIST = ROOT / "dist"


def main():
    # The version lives in the firmware too; refuse to ship a mismatched name.
    if f"DOTMIC v{VERSION}" not in (ROOT / "src/main.cpp").read_text(encoding="utf-8"):
        raise SystemExit(f"src/main.cpp does not report v{VERSION}; bump it or fix VERSION")
    image = (BUILD / "firmware.bin").read_bytes()
    if image[0] != 0xE9:
        raise ValueError("Not an ESP application image")
    end = 0
    app_size = None
    for line in (ROOT / "partitions.csv").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        name, kind, subtype, offset, size, *_ = line.split(",")
        offset, size = int(offset, 0), int(size, 0)
        assert offset % 4096 == size % 4096 == 0
        assert offset >= end and offset + size <= 16 * 1024 * 1024
        if kind == "app":
            assert offset % 65536 == 0
            app_size = size
        end = offset + size
        print(f"{name}: offset=0x{offset:x} size=0x{size:x}")
    # Verify the generated binary table as well as the source CSV.
    entries = []
    table = (BUILD / "partitions.bin").read_bytes()
    for pos in range(0, len(table), 32):
        magic, kind, subtype, offset, size, label, flags = struct.unpack("<HBBII16sI", table[pos:pos+32])
        if magic != 0x50AA:
            break
        entries.append((kind, offset, size))
    assert entries == [(1, 0x9000, 0x5000), (1, 0xE000, 0x2000), (0, 0x10000, 0x300000)]
    assert app_size and len(image) <= app_size
    DIST.mkdir(exist_ok=True)
    name = f"DotMic-v{VERSION}.bin"
    (DIST / name).write_bytes(image)
    digest = hashlib.sha256(image).hexdigest()
    (DIST / "SHA256SUMS.txt").write_text(f"{digest}  {name}\n")
    (DIST / "manifest.json").write_text(json.dumps({
        "name": "DotMic", "version": VERSION, "chip": "ESP32-S3",
        "file": name, "bytes": len(image), "sha256": digest,
        "format": "application-only", "build_partition_bytes": app_size,
    }, indent=2) + "\n")
    shutil.copyfile(ROOT / "lib/wifi-portal/LICENSE", DIST / "wifi-portal-MIT.txt")
    print(f"PASS: {name}, {len(image)} bytes, SHA256 {digest}")


if __name__ == "__main__":
    main()
