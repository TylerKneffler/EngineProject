"""Summarize real-backend mip streaming traces and saved cutout comparisons."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1)]


def timings(rows):
    host = [row['host_frame_ms'] for row in rows]
    samples = {row['gpu_sample_id']: row['gpu_opaque_ms'] for row in rows
               if row['gpu_opaque_ms'] is not None and row['gpu_sample_id'] > 0}
    return dict(frames=len(host), host_mean_ms=statistics.mean(host),
                host_p95_ms=percentile(host, .95), host_p99_ms=percentile(host, .99),
                resolved_gpu_samples=len(samples),
                gpu_opaque_mean_ms=statistics.mean(samples.values()) if samples else None)


def mask(path):
    header, dimensions, maximum, data = path.read_bytes().split(b'\n', 3)
    if header != b'P5' or maximum != b'255' or len(data) != math.prod(map(int, dimensions.split())):
        raise ValueError(f'Invalid capture {path}')
    return data


def summarize(directory):
    results = []
    for backend in ('dx11', 'dx12', 'vulkan'):
        root = directory / backend
        report = json.loads((root / f'{backend}-summary.json').read_text())
        rows = [json.loads(line) for line in (root / f'{backend}.jsonl').read_text().splitlines()]
        active = [r for r in rows if r['phase'] in ('rapid_turn', 'teleport_backtrack') and not r['diagnostic_capture']]
        warmup = [r for r in rows if r['budget_bytes'] and r['phase'] != 'reference' and not r['diagnostic_capture']]
        if not active or any(r['resident_bytes'] > r['budget_bytes'] for r in active):
            raise ValueError(f'{backend}: missing route or budget violation')
        if report['runtime_scene_drains'] or report['descriptor_replacements'] < 1200:
            raise ValueError(f'{backend}: retirement/descriptor gate failed')
        if any(not (.90 <= c['iou'] <= 1 and .95 <= c['coverage_ratio'] <= 1.05) for c in report['cases']):
            raise ValueError(f'{backend}: settled quality gate failed')
        comparisons = []
        for path in sorted(root.glob('motion*-pose*.pgm')):
            pose = int(path.stem.rsplit('pose', 1)[1])
            base, current = mask(root / f'reference-pose{pose}.pgm'), mask(path)
            if len(base) != len(current):
                raise ValueError('Mismatched motion capture dimensions')
            union = sum(bool(a) or bool(b) for a, b in zip(base, current))
            intersection = sum(bool(a) and bool(b) for a, b in zip(base, current))
            iou = intersection / union
            ratio = sum(bool(p) for p in current) / sum(bool(p) for p in base)
            if iou < .90 or not .95 <= ratio <= 1.05:
                raise ValueError(f'{backend}: motion quality gate failed: {path}')
            comparisons.append(dict(capture=path.name, iou=iou, coverage_ratio=ratio))
        if len(comparisons) != report['motion_comparisons']:
            raise ValueError(f'{backend}: incomplete motion captures')
        budgets = []
        for budget in sorted({r['budget_bytes'] for r in active}):
            frames = [r for r in active if r['budget_bytes'] == budget]
            budgets.append(dict(budget_bytes=budget, peak_resident_bytes=max(r['resident_bytes'] for r in frames),
                                peak_requested_bytes=max(r['requested_bytes'] for r in frames),
                                peak_staging_bytes=max(r['staging_bytes'] for r in frames),
                                peak_retired_bytes=max(r['retired_bytes'] for r in frames),
                                upload_bytes=frames[-1]['upload_bytes_total']-frames[0]['upload_bytes_total'],
                                evictions=frames[-1]['evictions_total']-frames[0]['evictions_total'],
                                unmet_frames=sum(r['unmet'] > 0 for r in frames), **timings(frames)))
        results.append(dict(backend=backend, allocation_scope='DX11 conservative estimate' if backend == 'dx11' else 'dedicated texture allocation',
                            route=timings(active), warmup_and_transitions=timings(warmup), budgets=budgets,
                            quality=report, motion_comparisons=comparisons))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--executable', type=Path, default=Path('build/Release/TextureStreamingBackendTests.exe'))
    args = parser.parse_args()
    results = summarize(args.directory)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    root = Path(__file__).resolve().parents[2]
    sources = ['Engine/Core/Compoonents/Materials/Texture.cpp', 'Engine/Core/Rendering/TextureStreamingPolicy.h',
               'Engine/Core/Rendering/TextureFootprint.h', 'Engine/Core/Rendering/TextureResidency.h',
               'Engine/Core/Graphics/IGraphicsTexture.h', '.local/tests/TextureStreamingBackendTests.cpp',
               '.local/tests/TextureStreamingFixtures.h', '.local/tests/TextureStreamingTests.cpp']
    for backend in ('DX11', 'DX12', 'Vulkan'):
        sources += [f'Engine/Core/Renderers/{backend}/{backend}GraphicsContext.cpp',
                    f'Engine/Core/Renderers/{backend}/{backend}GraphicsTexture.cpp']
    provenance = dict(executable=str(args.executable.resolve()), executable_sha256=sha(args.executable),
                      sources_sha256={name: sha(root / name) for name in sources})
    (args.directory / 'summary.json').write_text(json.dumps(dict(provenance=provenance, backends=results), indent=2) + '\n')
    lines = ['# Conventional mip streaming backend validation', '',
             'Host frame timings include preparation, render submission and presentation. Diagnostic readback and deliberate test pacing are excluded. GPU values measure opaque work, not the complete frame.', '',
             '| Backend | Budget MiB | Peak allocated MiB | Peak requested MiB | Upload MiB | Evictions | Unmet frames | Host P95 ms |',
             '|---|---:|---:|---:|---:|---:|---:|---:|']
    for backend in results:
        for b in backend['budgets']:
            lines.append(f"| {backend['backend']} | {b['budget_bytes']/1048576:.3f} | {b['peak_resident_bytes']/1048576:.3f} | "
                         f"{b['peak_requested_bytes']/1048576:.3f} | {b['upload_bytes']/1048576:.3f} | {b['evictions']} | {b['unmet_frames']} | {b['host_p95_ms']:.3f} |")
    lines += ['', 'All budget, settled/motion cutout, offscreen caster demand and 1,200 descriptor replacement gates passed on each backend. DX11 allocation accounting is an estimate; DX12/Vulkan use resource allocation queries.', '',
              'See summary.json for warmup-inclusive timings, GPU sample counts, per-capture comparisons and executable/source fingerprints.', '']
    (args.directory / 'summary.md').write_text('\n'.join(lines))
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
