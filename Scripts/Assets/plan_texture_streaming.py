"""Offline conventional mip policy model; does not upload or evict GPU resources.

Input JSON: textures [{id,width,height,bpp?}], frames [{demands:{id:pixels}}].
pixels is the projected length of one complete UV repeat, accounting for UV
tiling and the most demanding visible instance/view/shadow pass. Unknown IDs
are rejected. Output is JSON with per-frame payload and mip decisions.
"""
import argparse
import json
import math
from pathlib import Path


def chain(texture):
    w, h = texture['width'], texture['height']
    bpp = texture.get('bpp', 4)
    if any(type(x) is not int or x < 1 for x in (w, h, bpp)):
        raise ValueError('dimensions and bpp must be positive integers')
    sizes = []
    while True:
        sizes.append(w * h * bpp)
        if w == h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)
    return [sum(sizes[i:]) for i in range(len(sizes))]


def simulate(trace, budget, upload_budget, hold_frames=30, prefetch_mips=1):
    if budget < 0 or upload_budget < 0 or hold_frames < 0 or prefetch_mips < 0:
        raise ValueError('budgets and policy parameters must be nonnegative')
    textures = {t['id']: t for t in trace['textures']}
    if len(textures) != len(trace['textures']):
        raise ValueError('duplicate texture identity (include color space in IDs)')
    sizes = {key: chain(t) for key, t in textures.items()}
    resident = {key: len(s) - 1 for key, s in sizes.items()}
    minimum = sum(s[-1] for s in sizes.values())
    if minimum > budget:
        raise ValueError(f'budget below pinned mip floor: {minimum}')
    # Initial fallback uploads count too. No imaginary free initial residency.
    if minimum > upload_budget:
        raise ValueError(f'upload budget below initial fallback payload: {minimum}')
    recent = []
    rows = []
    for frame_index, frame in enumerate(trace['frames']):
        demands = frame['demands']
        if demands.keys() - textures.keys():
            raise ValueError('unknown texture demand')
        desired = {key: len(s) - 1 for key, s in sizes.items()}
        for key, pixels in demands.items():
            if not math.isfinite(pixels) or pixels < 0:
                raise ValueError('projected pixels must be finite and nonnegative')
            if pixels == 0:
                continue
            t = textures[key]
            lod = math.log2(max(t['width'], t['height']) / pixels)
            desired[key] = min(len(sizes[key]) - 1, max(0, math.floor(lod) - prefetch_mips))
        requested = sum(sizes[k][m] for k, m in desired.items())
        # Keep recent high detail through rapid turns. Budget pressure may break
        # this hold immediately; offscreen data loses to current visible demand.
        targets = dict(desired)
        recent.append(dict(desired))
        recent = recent[-(hold_frames + 1):]
        for key in targets:
            targets[key] = min(d[key] for d in recent)
        planned = sum(sizes[k][m] for k, m in targets.items())
        while planned > budget:
            candidates = [k for k in targets if targets[k] < len(sizes[k]) - 1]
            # Deterministic, offscreen first, then smaller projected footprints.
            key = min(candidates, key=lambda k: (demands.get(k, 0), k))
            old = targets[key]
            targets[key] += 1
            planned -= sizes[key][old] - sizes[key][old + 1]
        evictions = 0
        evicted_bytes = 0
        for key, mip in targets.items():
            if mip > resident[key]:
                evicted_bytes += sizes[key][resident[key]] - sizes[key][mip]
                resident[key] = mip
                evictions += 1
        uploaded = minimum if frame_index == 0 else 0
        # Model whole mip-tail replacement, including old+new overlap. Real
        # allocation/staging padding and fence retirement need extra headroom.
        peak = sum(sizes[k][m] for k, m in resident.items())
        used = peak
        for key in sorted(targets, key=lambda k: (-demands.get(k, 0), k)):
            target = targets[key]
            while target < resident[key]:
                cost = sizes[key][target]
                if uploaded + cost <= upload_budget and used + cost <= budget:
                    peak = max(peak, used + cost)
                    used += cost - sizes[key][resident[key]]
                    resident[key] = target
                    uploaded += cost
                    break
                target += 1
        rows.append(dict(frame=frame_index, requested_bytes=requested,
                         resident_bytes=used, peak_replacement_bytes=peak,
                         upload_bytes=uploaded, evictions=evictions,
                         evicted_bytes=evicted_bytes, demand_mips=desired,
                         resident_mips=dict(resident),
                         unmet_textures=sum(resident[k] > desired[k] for k in desired)))
    return dict(model='CPU policy simulation; no GPU validation', budget_bytes=budget,
                pinned_floor_bytes=minimum, frames=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--budget-mib', type=float, required=True)
    parser.add_argument('--upload-mib', type=float, default=16)
    parser.add_argument('--hold-frames', type=int, default=30)
    parser.add_argument('--prefetch-mips', type=int, default=1)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = simulate(json.loads(args.trace.read_text()), int(args.budget_mib * 1048576),
                      int(args.upload_mib * 1048576), args.hold_frames, args.prefetch_mips)
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
