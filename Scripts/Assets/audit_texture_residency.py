"""Estimate RGBA8 mip payloads for PNG assets; excludes GPU allocation padding.

Run with a texture directory and optional --max-dimension caps. Each file is
counted once; this is an asset inventory, not a scene residency measurement.
"""

import argparse
from pathlib import Path
import struct


def png_dimensions(path):
    with path.open("rb") as source:
        header = source.read(24)
    if len(header) != 24 or header[:16] != b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR":
        raise ValueError(f"Invalid PNG header: {path}")
    width, height = struct.unpack(">II", header[16:24])
    if not width or not height:
        raise ValueError(f"Invalid PNG dimensions: {path}")
    return width, height


def mip_payload(width, height, cap=None):
    while cap and max(width, height) > cap:
        width, height = max(1, width // 2), max(1, height // 2)
    total = 0
    while True:
        total += width * height * 4
        if width == height == 1:
            return total
        width, height = max(1, width // 2), max(1, height // 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--max-dimension", type=int, nargs="+", default=[2048, 1024])
    args = parser.parse_args()
    if not args.directory.is_dir():
        parser.error("directory must exist")
    if any(cap < 1 for cap in args.max_dimension):
        parser.error("maximum dimensions must be positive")
    paths = sorted(p for p in args.directory.rglob("*") if p.suffix.lower() == ".png")
    if not paths:
        parser.error("no PNG textures found")
    totals = [0] * (1 + len(args.max_dimension))
    print("| Texture | Dimensions | Full MiB | " + " | ".join(
        f"Cap {cap} MiB" for cap in args.max_dimension) + " |")
    print("|---|---|---:|" + "---:|" * len(args.max_dimension))
    for path in paths:
        width, height = png_dimensions(path)
        sizes = [mip_payload(width, height, cap) for cap in [None, *args.max_dimension]]
        totals = [total + size for total, size in zip(totals, sizes)]
        print(f"| {path.relative_to(args.directory).as_posix()} | {width} x {height} | " +
              " | ".join(f"{size / 1048576:.3f}" for size in sizes) + " |")
    print(f"| **Total ({len(paths)} files)** | | " +
          " | ".join(f"**{size / 1048576:.3f}**" for size in totals) + " |")
    print("\nRGBA8 full mip-chain payload estimates; excludes allocation padding, "
          "staging, CPU copies, duplicate color-space bindings and non-PNG assets.")


if __name__ == "__main__":
    main()
