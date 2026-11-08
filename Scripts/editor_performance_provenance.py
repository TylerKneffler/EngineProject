"""Build, content, settings and trajectory checks for editor route comparisons."""
import json
import math
from pathlib import Path
import platform
import subprocess

from run_forest_performance import content_identity, identity_digest, sha256

SETTINGS = ('graphics_backend', 'build_configuration', 'assertions_enabled',
            'render_scale', 'dynamic_render_scale', 'temporal_upscaling',
            'quality_preset', 'quality_geometry', 'quality_vegetation', 'quality_viewDistance',
            'quality_texture', 'quality_material', 'quality_shadows', 'quality_renderScale',
            'game_view_width', 'game_view_height', 'scene_views', 'game_views')


def machine_identity():
    query = subprocess.run(['powershell', '-NoProfile', '-Command',
        'Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,PNPDeviceID | ConvertTo-Json -Compress'],
        capture_output=True, text=True, check=True)
    return dict(node=platform.node(), system=platform.platform(), processor=platform.processor(),
                display_adapters=json.loads(query.stdout))


def build_identity(executable):
    executable = Path(executable).resolve()
    cache = executable.parent.parent / 'CMakeCache.txt'
    # Include the dynamically loaded scene scripts as well as the executable.
    libraries = set(executable.parent.glob('*.dll'))
    libraries.update((executable.parent.parent / 'lib' / executable.parent.name).glob('*.dll'))
    return dict(executable_path=str(executable), executable_sha256=sha256(executable),
                cmake_cache_sha256=sha256(cache) if cache.exists() else None,
                libraries={str(p): sha256(p) for p in sorted(libraries)})


def measured_rows(path):
    rows = [json.loads(line) for line in Path(path).read_text().splitlines()]
    if not any(row.get('type') == 'capture_complete' for row in rows):
        raise ValueError('Editor capture is incomplete')
    return [row for row in rows if row.get('playing') and row.get('measured') and 'frame' in row]


def workload_identity(path, fixed_step, requested_backend=None):
    rows = measured_rows(path)
    if not rows:
        raise ValueError('No measured Play frames')
    settings = {key: rows[0].get(key) for key in SETTINGS}
    if any(value is None for value in settings.values()) or settings['build_configuration'] == 'unknown':
        raise ValueError('Missing build/backend/settings identity')
    if requested_backend and settings['graphics_backend'].lower() != requested_backend:
        raise ValueError('Actual backend differs from requested backend')
    trajectory = []
    for row in rows:
        if any(row.get(key) != value for key, value in settings.items()):
            raise ValueError('Build/backend/render settings changed during capture')
        if fixed_step and not math.isclose(row.get('simulation_delta_ms', -1), fixed_step * 1000, abs_tol=.002):
            raise ValueError('Measured route did not use the requested simulation step')
        sample = [row.get('simulation_time_seconds'), row.get('game_camera_view'), row.get('game_camera_projection')]
        if any(value is None for value in sample):
            raise ValueError('Missing measured route time or camera matrices')
        trajectory.append(sample)
    return dict(settings=settings, frames=len(rows), fixed_step_seconds=fixed_step, trajectory=trajectory,
                trajectory_sha256=identity_digest(trajectory))


def load_capture(path):
    path = Path(path).resolve()
    manifest = json.loads(path.with_suffix('.inputs.json').read_text())
    provenance = manifest.get('provenance', {})
    if (not manifest.get('capture_succeeded') or not manifest.get('comparison', {}).get('unchanged') or
            not provenance.get('assets_unchanged') or not provenance.get('build_unchanged')):
        raise ValueError('Capture identity is incomplete or changed during recording')
    if provenance.get('schema') != 1 or any(provenance.get(key) is None for key in
            ('assets_sha256', 'platform', 'environment', 'route_sha256', 'explicit_inputs_sha256', 'build', 'workload')):
        raise ValueError('Missing complete editor comparison provenance')
    if provenance.get('recording_sha256') != sha256(path):
        raise ValueError('Editor recording changed after capture')
    actual = workload_identity(path, provenance.get('workload', {}).get('fixed_step_seconds'))
    if actual != provenance.get('workload'):
        raise ValueError('Editor workload differs from recorded identity')
    if not provenance.get('build', {}).get('cmake_cache_sha256'):
        raise ValueError('Missing build configuration cache identity')
    executable = Path(provenance['build'].get('executable_path', '')).resolve()
    if any(Path(row.get('executable_path', '')).resolve() != executable for row in measured_rows(path)):
        raise ValueError('Recorded executable differs from the captured build')
    return manifest


def compare_captures(current_path, baseline_path):
    current, baseline = (load_capture(p) for p in (current_path, baseline_path))
    a, b = current['provenance'], baseline['provenance']
    for key in ('assets_sha256', 'platform', 'environment', 'route_sha256', 'explicit_inputs_sha256'):
        if a.get(key) != b.get(key):
            raise ValueError(f'Cannot match editor captures: {key} differs')
    if a['build']['cmake_cache_sha256'] != b['build']['cmake_cache_sha256']:
        raise ValueError('Cannot match editor captures: build configuration differs')
    if current['layout_seed']['sha256'] != baseline['layout_seed']['sha256']:
        raise ValueError('Cannot match editor captures: layout seed differs')
    wa, wb = a['workload'], b['workload']
    if not wa['fixed_step_seconds'] or wa != wb:
        raise ValueError('Cannot match editor captures: simulation step, settings, frame budget or camera trajectory differs')
    def stats(path):
        import statistics
        rows = measured_rows(path)
        recurring = [row for row in rows if row.get('recurring_frame')]
        if not recurring:
            raise ValueError('No recurring measured frames')
        values = [row['editor_update_interval_ms'] for row in recurring]
        return dict(frames=len(rows), recurring_frames=len(recurring),
                    effective_fps=1000 / statistics.mean(values),
                    frame_median_ms=statistics.median(values),
                    executable_sha256=load_capture(path)['provenance']['build']['executable_sha256'])
    return dict(matched=True, baseline=stats(baseline_path), current=stats(current_path),
                trajectory_sha256=wa['trajectory_sha256'], settings=wa['settings'])
