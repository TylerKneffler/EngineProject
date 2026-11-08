"""Capture three backends and compare color-bias cutout coverage.

Motion deltas include projection changes; they are diagnostics, not a calibrated
shimmer score. Extended mode also isolates rendered shadows on a lit receiver.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


def read_image(path, magic, channels):
    header, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
    if (header != magic or dimensions != b"512 512" or maximum != b"255"
            or len(pixels) != 512 * 512 * channels):
        raise ValueError(f"Unexpected capture: {path}")
    return pixels


def analyze(directory, backend):
    report = json.loads((directory / f"{backend}.json").read_text())
    rows = report["coverage"]
    foliage_types=(0,) if report.get("material_study") else (0,1)
    if len(rows) != len(foliage_types)*54:
        raise ValueError(f"Incomplete {backend} capture: {len(rows)} rows")
    groups = {}
    comparisons = []
    for row in rows:
        key = row["foliage"], row["angle"], row["distance"]
        stem = (f'{backend}-f{key[0]}-a{key[1]}-d{key[2]:.6f}'
                f'-bias{row["preset"]}')
        mask = read_image(directory / (stem + ".pgm"), b"P5", 1)
        rgb = read_image(directory / (stem + ".ppm"), b"P6", 3)
        if sum(bool(p) for p in mask) != row["pixels"] or row["pixels"] <= 10:
            raise ValueError(f"Invalid coverage: {stem}")
        groups.setdefault(key, {})[row["preset"]] = (mask, rgb)
    for key, pair in groups.items():
        changed = sum(a != b for a, b in zip(pair[0][0], pair[1][0]))
        if changed:
            raise ValueError(f"Color bias changed {changed} mask pixels: {backend} {key}")
        comparisons.append(dict(foliage=key[0], angle=key[1], distance=key[2],
                                changed_mask_pixels=changed))
    motion = []
    previous = {}
    for key, pair in sorted(groups.items()):
        for biased, (mask, rgb) in pair.items():
            track = key[0], key[1], biased
            if track in previous:
                distance, old_mask, old_rgb = previous[track]
                union = sum(bool(a or b) for a, b in zip(mask, old_mask))
                xor = sum(a != b for a, b in zip(mask, old_mask))
                delta = sum(abs(a-b) for a, b in zip(rgb, old_rgb))
                motion.append(dict(foliage=key[0], angle=key[1], biased=biased,
                                   from_distance=distance, to_distance=key[2],
                                   mask_xor_over_union=xor / max(1, union),
                                   rgb_delta_per_union_channel=delta / max(1, union * 3)))
            previous[track] = key[2], mask, rgb
    return dict(backend=backend, identical_mask_pairs=len(comparisons),
                comparisons=comparisons, motion_diagnostics=motion)


def analyze_extended(directory, backend):
    report = json.loads((directory / f"{backend}.json").read_text())
    motion, shadows = report["motion"], report["shadows"]
    foliage_types=(0,) if report.get("material_study") else (0,1)
    if len(motion) != len(foliage_types)*360 or len(shadows) != len(foliage_types)*18:
        raise ValueError(f"Incomplete extended capture: {backend}")
    pairs = {}
    for row in motion:
        pairs.setdefault((row["foliage"], row["frame"]), {})[row["bias"]] = row
    if set(pairs) != {(f, frame) for f in foliage_types for frame in range(180)}:
        raise ValueError(f"Missing continuous motion frames: {backend}")
    for key, pair in pairs.items():
        for field in ("time", "angle", "distance", "coverage", "mask_xor", "mask_union"):
            if pair[0][field] != pair[1][field]:
                raise ValueError(f"Motion pair differs in {field}: {backend} {key}")
    material_effect=None
    if report.get('material_study'):
        # The policy uses per-fragment world distance, so leave room for the
        # card's sqrt(2) radius rather than classifying only its centre.
        near=[p[1]['paired_rgb_delta'] for p in pairs.values() if p[1]['distance']<=1.4]
        far=[p[1]['paired_rgb_delta'] for p in pairs.values() if p[1]['distance']>=9.5]
        if not near or not far or any(near) or not any(far):
            raise ValueError('Material study did not preserve the near endpoint or exercise cheaper far lighting')
        effect=[(p[1]['frame'],p[1]['paired_rgb_delta']/max(1,3*p[1]['coverage']))
                for _,p in sorted(pairs.items())]
        max_step=max(abs(effect[i][1]-effect[i-1][1]) for i in range(1,len(effect)) if effect[i][0]!=120)
        # Acceptance for this deterministic trajectory: the mean material-only
        # lighting difference must not jump by a full 8-bit code per frame.
        # Frame 120 deliberately teleports to the fixed-camera wind test.
        if max_step>1.0:
            raise ValueError(f'Material lighting effect changed abruptly: {max_step}')
        material_effect=dict(near_rgb_deltas=near,far_rgb_deltas=far,
            max_lighting_effect_step=max_step,lighting_effect_step_limit=1.0,
            max_paired_rgb_delta_per_covered_channel=max(p[1]['paired_rgb_delta']/max(1,3*p[1]['coverage']) for p in pairs.values()))
    temporal = []
    for foliage in foliage_types:
        for fixed in (False, True):
            for bias in (0, 1):
                rows = [r for r in motion if r["foliage"] == foliage and r["bias"] == bias
                        and (r["frame"] > 120 if fixed else 0 < r["frame"] < 120)]
                union = sum(r["mask_union"] for r in rows)
                common = sum(r["common_pixels"] for r in rows)
                if union <= 0 or common <= 0 or not any(r["mask_xor"] for r in rows):
                    raise ValueError(f"Motion test did not move visible foliage: {backend}")
                temporal.append(dict(foliage=foliage, fixed_camera=fixed, bias=bias,
                                     mask_turnover=sum(r["mask_xor"] for r in rows) / union,
                                     rgb_delta_per_common_channel=sum(r["rgb_delta"] for r in rows) / (3 * common)))
    shadow_pairs = {}
    for row in shadows:
        key = row["foliage"], row["angle"], row["distance"]
        stem = (f'{backend}-shadow-f{key[0]}-a{key[1]}-d{key[2]:.6f}'
                f'-b{row["bias"]}')
        mask = read_image(directory / (stem + ".pgm"), b"P5", 1)
        if (sum(bool(p) for p in mask) != row["masked_pixels"] or row["masked_pixels"] <= 10
                or row["opaque_pixels"] <= row["masked_pixels"] or row["cutout_holes"] <= 10
                or row["wind_changed_pixels"] != 0):
            raise ValueError(f"Invalid rendered shadow control: {stem}")
        shadow_pairs.setdefault(key, {})[row["bias"]] = mask
    for key, pair in shadow_pairs.items():
        if pair[0] != pair[1]:
            raise ValueError(f"Bias changed rendered shadow mask: {backend} {key}")
    if set(shadow_pairs) != {(f, a, d) for f in foliage_types for a in (0, 60, 80) for d in (3, 5.5, 8)}:
        raise ValueError(f"Missing rendered shadow views: {backend}")
    return dict(backend=backend, diagonal_shadow=report["diagonal_shadow"],
                material_effect=material_effect,
                identical_motion_pairs=len(pairs),
                identical_shadow_pairs=len(shadow_pairs), temporal_diagnostics=temporal,
                shadow_controls=shadows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, default=Path(".local/texture-sampling"))
    parser.add_argument("--reuse", action="store_true")
    parser.add_argument("--material-study", action="store_true", help="Compare lit Object material simplification at fixed mip bias")
    parser.add_argument("--extended", action="store_true",
                        help="continuous camera/wind motion and rendered shadow controls")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    for backend in ("dx11", "dx12", "vulkan"):
        if not args.reuse:
            with (args.output / f"{backend}.log").open("w") as log:
                command = [str(args.executable.resolve()), backend, str(args.output / backend)]
                if args.extended:
                    command.append("--extended")
                environment=os.environ.copy()
                if args.material_study: environment['ENGINE_MATERIAL_STUDY']='1'
                else: environment.pop('ENGINE_MATERIAL_STUDY',None)
                subprocess.run(command, stdout=log, env=environment,
                               stderr=subprocess.STDOUT, check=True, timeout=600)
        result = (analyze_extended if args.extended else analyze)(args.output, backend)
        if json.loads((args.output / f"{backend}.json").read_text()).get('material_study',False) != args.material_study:
            raise ValueError('Capture policy does not match requested study')
        results.append(result)
        if args.extended:
            print(f'{backend}: {result["identical_motion_pairs"]} identical motion pairs, '
                  f'{result["identical_shadow_pairs"]} identical rendered shadow pairs')
        else:
            print(f'{backend}: {result["identical_mask_pairs"]} identical mask pairs')
    (args.output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
