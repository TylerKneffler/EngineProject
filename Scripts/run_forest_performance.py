"""Run the static forest diagnostic with content and process provenance."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def content_identity():
    # Conservative scope: every bundled asset and engine shader, not just
    # literal prefab references (which omit textures, models and shader includes).
    result = {}
    for directory in ('Engine/Core/Assets', 'Engine/Core/Shaders'):
        for path in sorted((ROOT / directory).rglob('*')):
            if path.is_file():
                result[path.relative_to(ROOT).as_posix()] = sha256(path)
    return result


def identity_digest(files):
    return hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--backend', choices=('dx11', 'dx12', 'vulkan'), required=True)
    parser.add_argument('--executable', type=Path, default=ROOT / 'build/Debug/Release/ForestPerformanceAudit.exe')
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--shadows', action='store_true')
    parser.add_argument('--counters', action='store_true')
    parser.add_argument('--cpu-profile', action='store_true')
    parser.add_argument('--material-only', action='store_true')
    parser.add_argument('--reverse', action='store_true', help='Reverse material case order')
    args = parser.parse_args()
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    if any(directory.iterdir()):
        parser.error('Use an empty output directory so captures cannot mix builds or settings')
    executable = args.executable.resolve()
    environment = {k: v for k, v in os.environ.items() if k.startswith(('ENGINE_', 'VK_', 'DX', 'D3D'))}
    for key, value in {
        'ENGINE_FOREST_AUDIT_OUTPUT': str(directory),
        'ENGINE_FOREST_AUDIT_WIDTH': str(args.width),
        'ENGINE_FOREST_AUDIT_SHADOWS': '1' if args.shadows else None,
        'ENGINE_GPU_WORKLOAD_COUNTERS': '1' if args.counters else None,
        'ENGINE_FOREST_AUDIT_CPU_PROFILE': '1' if args.cpu_profile else None,
        'ENGINE_FOREST_AUDIT_MATERIAL_ONLY': '1' if args.material_only else None,
        'ENGINE_FOREST_AUDIT_REVERSE': '1' if args.reverse else None,
    }.items():
        if value is None:
            environment.pop(key, None)
        else:
            environment[key] = value
    child_environment = {k: v for k, v in os.environ.items() if not k.startswith(('ENGINE_', 'VK_', 'DX', 'D3D'))}
    child_environment.update(environment)
    print('Fingerprinting bundled assets and shaders...', flush=True)
    adapters = []
    if os.name == 'nt':
        query = subprocess.run(['powershell', '-NoProfile', '-Command',
            'Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,PNPDeviceID | ConvertTo-Json -Compress'],
            capture_output=True, text=True, check=True)
        adapters = json.loads(query.stdout)
    before = content_identity()
    manifest = dict(schema=1, time_utc=datetime.now(timezone.utc).isoformat(),
                    executable_path=str(executable), executable_sha256=sha256(executable),
                    inputs_sha256=identity_digest(before), inputs=before,
                    platform=dict(node=platform.node(), system=platform.platform(), processor=platform.processor(), display_adapters=adapters),
                    environment={k: v for k, v in environment.items() if k != 'ENGINE_FOREST_AUDIT_OUTPUT'},
                    backend=args.backend, command=[str(executable), args.backend],
                    inputs_unchanged=False, executable_unchanged=False, returncode=None)
    cache = executable.parent.parent / 'CMakeCache.txt'
    manifest['cmake_cache_sha256'] = sha256(cache) if cache.exists() else None
    revision = subprocess.run(['git', '-c', f'safe.directory={ROOT.as_posix()}', 'rev-parse', 'HEAD'],
                              cwd=ROOT, capture_output=True, text=True)
    manifest['source_revision'] = revision.stdout.strip() if revision.returncode == 0 else None
    path = directory / 'provenance.json'
    path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    try:
        result = subprocess.run(manifest['command'], cwd=ROOT, env=child_environment, check=False)
        manifest['returncode'] = result.returncode
        manifest['recordings_sha256'] = {p.name: sha256(p) for p in directory.glob('*.jsonl')}
    finally:
        manifest['inputs_unchanged'] = before == content_identity()
        manifest['executable_unchanged'] = manifest['executable_sha256'] == sha256(executable)
        path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    if manifest['returncode'] or not manifest['inputs_unchanged'] or not manifest['executable_unchanged']:
        raise RuntimeError(f'Capture failed or inputs changed; see {path}')
    subprocess.run([os.sys.executable, str(ROOT / 'Scripts/summarize_forest_performance.py'), str(directory)], check=True)


if __name__ == '__main__':
    main()
