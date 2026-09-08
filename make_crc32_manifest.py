#!/usr/bin/env python3
"""Generate a CRC32 manifest for all .bin files under a directory."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import zlib


def file_crc32(path: Path) -> int:
    crc = 0
    with path.open("rb") as f:
        while True:
            block = f.read(1024 * 1024)
            if not block:
                break
            crc = zlib.crc32(block, crc)
    return crc & 0xFFFFFFFF


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="directory containing binary artifacts")
    ap.add_argument("output", help="manifest output file")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    output = Path(args.output)

    if not root.is_dir():
        ap.error(f"directory not found: {root}")

    files = sorted(
        (p for p in root.rglob("*.bin") if p.is_file()),
        key=lambda p: p.relative_to(root).as_posix().lower(),
    )

    if not files:
        ap.error(f"no .bin files found under: {root}")

    rows = []
    for path in files:
        rel = path.relative_to(root).as_posix()
        size = path.stat().st_size
        crc = file_crc32(path)
        rows.append((rel, size, crc))

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# DVPlogger binary CRC32 manifest\n")
        f.write("# CRC32 = zlib.crc32 / standard ZIP-Ethernet CRC-32\n")
        f.write("# FILE\tSIZE\tCRC32\n")
        for rel, size, crc in rows:
            f.write(f"{rel}\t{size}\t{crc:08X}\n")

    print(f"CRC32 manifest: {output}")
    for rel, size, crc in rows:
        print(f"  {rel:<32} {size:>9} bytes  {crc:08X}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
