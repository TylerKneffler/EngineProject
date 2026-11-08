"""Run real-backend oak uploads and compare resident-cap leaf silhouettes."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def mask(path):
    header, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
    if header != b"P5" or maximum != b"255" or len(pixels) != 512 * 512:
        raise ValueError(f"Unexpected mask: {path}")
    return pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, default=Path(".local/texture-residency-validation"))
    parser.add_argument("--backends", nargs="+", default=["dx11", "dx12", "vulkan"])
    parser.add_argument("--reuse", action="store_true", help="analyze existing captures without launching GPUs")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    failed = False
    for backend in args.backends:
        for cap in (0, 2048, 1024):
            prefix = args.output / f"{backend}-{cap}"
            env = dict(os.environ, ENGINE_TEXTURE_MAX_DIMENSION=str(cap), ENGINE_TEXTURE_STREAMING_BUDGET_MB="0")
            returncode = 0
            if not args.reuse:
                with prefix.with_suffix(".log").open("w") as log:
                    completed = subprocess.run([str(args.executable.resolve()), backend, str(prefix)],
                                               env=env, stdout=log, stderr=subprocess.STDOUT, timeout=240)
                returncode = completed.returncode
            if returncode or not prefix.with_suffix(".json").exists():
                results.append(dict(backend=backend, cap=cap, error="backend audit failed; see log"))
                failed = True
                continue
            result = json.loads(prefix.with_suffix(".json").read_text())
            result["cap"] = cap
            if backend == "dx11":
                # DX11 has no per-resource allocation query. A zero from the
                # harness means unavailable, never a zero-byte GPU allocation.
                result["allocation_bytes"] = None
                result["allocation_passed"] = None
                failed = True
            else:
                baseline_allocation = next((r.get("allocation_bytes", 0) for r in results
                                            if r["backend"] == backend and r["cap"] == 0),
                                           result["allocation_bytes"] if cap == 0 else 0)
                limit = {0: 1.0, 2048: 0.40, 1024: 0.15}[cap]
                allocation_passed = (baseline_allocation > 0 and
                                     0 < result["allocation_bytes"] <= baseline_allocation * limit)
                result["allocation_passed"] = allocation_passed
                failed |= not allocation_passed
            comparisons = []
            for distance in (3, 8, 20):
                for lod in (0, 1):
                    suffix = f"-d{distance}-lod{lod}.pgm"
                    reference_path = args.output / f"{backend}-0{suffix}"
                    if not reference_path.exists():
                        failed = True
                        continue
                    baseline = mask(reference_path)
                    current = mask(Path(str(prefix) + suffix))
                    intersection = sum(bool(a) and bool(b) for a, b in zip(baseline, current))
                    union = sum(bool(a) or bool(b) for a, b in zip(baseline, current))
                    base_count, count = sum(bool(a) for a in baseline), sum(bool(a) for a in current)
                    ratio = count / base_count
                    iou = intersection / union
                    # A declared quality gate, not an assertion of identical images.
                    passed = 0.95 <= ratio <= 1.05 and iou >= 0.90
                    failed |= not passed
                    comparisons.append(dict(distance=distance, lod=lod, coverage_ratio=ratio,
                                            silhouette_iou=iou, passed=passed))
            result["comparisons"] = comparisons
            results.append(result)
            print(backend, cap, "allocation bytes", result["allocation_bytes"],
                  "min IoU", min((c["silhouette_iou"] for c in comparisons), default=0), flush=True)
    (args.output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
