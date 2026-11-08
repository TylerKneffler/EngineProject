"""Summarize matched ScreenForest captures without approving a quality rollout."""
import argparse
import json
from pathlib import Path

import struct
import zlib

try:
    import numpy as np
except ImportError:
    np = None


def read_ppm(path):
    with path.open("rb") as stream:
        assert stream.readline().strip() == b"P6"
        width, height = map(int, stream.readline().split())
        assert stream.readline().strip() == b"255"
        pixels = stream.read()
    assert len(pixels) == width * height * 3
    return width, height, pixels


def compare(reference, candidate):
    width, height, first = read_ppm(reference)
    other_width, other_height, second = read_ppm(candidate)
    assert (width, height) == (other_width, other_height)
    if np is not None:
        delta = np.abs(np.frombuffer(first, dtype=np.uint8).astype(np.int16) -
                       np.frombuffer(second, dtype=np.uint8).astype(np.int16)).reshape(-1, 3)
        return {"changed_pixels": int(np.count_nonzero(np.any(delta > 3, axis=1))),
                "pixels": width * height, "maximum_channel_delta": int(delta.max()),
                "mean_channel_delta": float(delta.mean())}
    changed = maximum = total = 0
    for pixel in range(0, len(first), 3):
        delta = [abs(first[pixel + channel] - second[pixel + channel]) for channel in range(3)]
        changed += max(delta) > 3
        maximum = max(maximum, *delta)
        total += sum(delta)
    return {"changed_pixels": changed, "pixels": width * height,
            "maximum_channel_delta": maximum, "mean_channel_delta": total / len(first)}


def write_sheet(files, destination):
    pictures = [read_ppm(path) for path in files]
    rows = []
    for y in range(270):
        row = bytearray(b"\0")
        for width, height, pixels in pictures:
            source_y = y * height // 270
            for x in range(480):
                start = (source_y * width + x * width // 480) * 3
                row.extend(pixels[start:start + 3])
        rows.append(row)
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    payload = b"\x89PNG\r\n\x1a\n"
    payload += chunk(b"IHDR", struct.pack(">IIBBBBB", 480 * len(files), 270, 8, 2, 0, 0, 0))
    payload += chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b"")
    destination.write_bytes(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    results = []
    for reference in sorted(args.root.glob("*/*/*-distance.ppm")):
        if "ground" in reference.name:
            continue
        prefix = reference.name.removesuffix("-distance.ppm")
        row = {"backend": reference.parent.parent.name,
               "scale": reference.parent.name, "view": prefix}
        repeat = reference.with_name(prefix + "-distance-repeat.ppm")
        if repeat.exists():
            row["unchanged_repeat"] = compare(reference, repeat)
        files = [reference]
        for variant in ("override", "screen", "screen-cpu"):
            candidate = reference.with_name(prefix + "-" + variant + ".ppm")
            if candidate.exists():
                row[variant] = compare(reference, candidate)
                files.append(candidate)
        for suffix in ("ground-distance", "screen"):
            candidate = reference.with_name(prefix + "-" + suffix + ".ppm")
            other = reference.with_name(prefix + ("-ground-screen.ppm" if suffix.startswith("ground") else "-screen-cpu.ppm"))
            if candidate.exists() and other.exists():
                row["shadow" if suffix.startswith("ground") else "cpu_gpu"] = compare(candidate, other)
        ground = reference.with_name(prefix + "-ground-screen.ppm")
        ground_repeat = reference.with_name(prefix + "-ground-repeat.ppm")
        if ground.exists() and ground_repeat.exists():
            row["unchanged_ground_repeat"] = compare(ground, ground_repeat)
        write_sheet(files, reference.with_name(prefix + "-comparison.png"))
        results.append(row)
    output = args.root / "comparison.json"
    output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {len(results)} matched comparisons to {output}")


if __name__ == "__main__":
    main()
