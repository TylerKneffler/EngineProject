#!/usr/bin/env python3
"""Cook alpha-tested foliage textures so distant canopies keep their coverage.

The renderer is single-sample in real time (MSAA exists only for DX11 video
export), so alpha-to-coverage is not available; cutout foliage relies on its
mip chain instead. PNGs get a runtime 2x2 box-filtered chain
(Texture.cpp GenerateMipChain), which does not preserve the share of texels
above the material's alphaCutoff: with the oak pack's 0.32 cutoff the canopy
and impostor cutouts grow 14-60% by mip 4-7 (crowns bloat and pulse as the
mip changes), while the small sapling impostors drop to a quarter.

This script finds every Mask material whose base colour is a PNG, cooks the
PNG with TextureCook --alpha-cutoff <material cutoff> (push-pull colour fill
under transparent texels + per-mip alpha rescale to the level-0 coverage,
BC7) into a .ktx2 beside it, and points the material at the .ktx2. The PNG
stays as the source. Re-run after replacing a source PNG.

Usage: python Tools/cook_foliage_textures.py [--dry-run] [--check]
  --check   print coverage per mip (box filter vs level 0) and exit
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import woodland_assets as wa  # noqa: E402


def masked_png_materials():
    """{texture asset path: [(material file, cutoff)]}"""
    found: dict = {}
    for path in sorted(wa.ASSETS.rglob("*.material")):
        try:
            material = json.loads(path.read_text(encoding="utf-8"))
        except (ValueError, UnicodeDecodeError):
            continue
        if material.get("alphaMode") != "Mask":
            continue
        texture = material.get("baseColorTexture", "")
        if texture.lower().endswith(".png"):
            found.setdefault(texture, []).append((path, float(material.get("alphaCutoff", 0.5))))
    return found


def coverage_chain(alpha, cutoff, levels=8):
    chain = []
    level = alpha
    while min(level.shape) >= 8 and len(chain) < levels:
        chain.append(float((level >= cutoff).mean()))
        h, w = level.shape[0] // 2 * 2, level.shape[1] // 2 * 2
        level = wa.downsample(level[:h, :w], 2)
    return chain


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if wa.find_texture_cook() is None and not args.check:
        sys.exit("TextureCook is not built (cmake --build build --config RelWithDebInfo --target TextureCook)")
    for texture, users in masked_png_materials().items():
        source = wa.ASSETS / texture.removeprefix("Assets/")
        if not source.exists():
            print(f"missing {texture}")
            continue
        cutoff = min(c for _, c in users)
        if args.check:
            chain = coverage_chain(wa.read_png(source)[..., 3], cutoff)
            base = chain[0] or 1.0
            print(f"{texture} (cutoff {cutoff}): " + " ".join(f"{c / base:.2f}" for c in chain))
            continue
        output = source.with_suffix(".ktx2")
        print(f"{texture} -> {output.name} (cutoff {cutoff}, {len(users)} material(s))")
        if args.dry_run:
            continue
        wa.cook(source, output, "color", alpha_cutoff=cutoff)
        new_asset = texture[:-4] + ".ktx2"
        for material_path, _ in users:
            text = material_path.read_bytes().decode("utf-8")  # keep line endings
            pattern = re.compile(r'("baseColorTexture"\s*:\s*")' + re.escape(texture) + '"')
            text, count = pattern.subn(lambda m: m.group(1) + new_asset + '"', text)
            if count:
                material_path.write_bytes(text.encode("utf-8"))


if __name__ == "__main__":
    main()
