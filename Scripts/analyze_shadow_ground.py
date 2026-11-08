"""Measure fixed terrain samples in OakGroundCapture artifacts (requires numpy).

Reports temporal changes; it does not declare a visual-quality pass. Terrain
ray tests reject occluded samples, and matched unshadowed images control for
texture resampling and view-dependent shading. RGB deficits are display-space
measurements, not linear shadow visibility.
"""
import argparse
import json
from pathlib import Path

import numpy as np


def read_ppm(path):
    with path.open("rb") as stream:
        if stream.readline() != b"P6\n":
            raise ValueError(f"Expected generated P6 image: {path}")
        width, height = map(int, stream.readline().split())
        if stream.readline() != b"255\n":
            raise ValueError(f"Expected 8-bit image: {path}")
        return np.frombuffer(stream.read(), dtype=np.uint8).reshape(height, width, 3)


def project(points, matrix, width, height):
    matrix = np.asarray(matrix, dtype=float).reshape(4, 4, order="F")
    clip = np.column_stack((points, np.ones(len(points)))) @ matrix.T
    with np.errstate(divide="ignore", invalid="ignore"):
        ndc = clip[:, :3] / clip[:, 3:4]
    pixels = np.column_stack(((ndc[:, 0] + 1) * width / 2 - .5,
                              (1 - ndc[:, 1]) * height / 2 - .5))
    valid = (clip[:, 3] > 0) & (ndc[:, 2] > 0) & (ndc[:, 2] < 1)
    valid &= np.isfinite(pixels).all(axis=1)
    valid &= (pixels[:, 0] >= 3) & (pixels[:, 0] < width - 4)
    valid &= (pixels[:, 1] >= 3) & (pixels[:, 1] < height - 4)
    return pixels, valid


def terrain_visible(point, eye, triangles):
    # Segment parameter t=1 is the sample surface. Reject closer terrain;
    # accept a small contact tolerance for exported float geometry.
    origin = triangles[:, 0]
    edge1 = triangles[:, 1] - origin
    edge2 = triangles[:, 2] - origin
    direction = point - eye
    p = np.cross(direction, edge2)
    determinant = np.einsum("ij,ij->i", edge1, p)
    valid = np.abs(determinant) > 1.e-10
    inverse = np.zeros_like(determinant)
    inverse[valid] = 1 / determinant[valid]
    offset = eye - origin
    u = np.einsum("ij,ij->i", offset, p) * inverse
    q = np.cross(offset, edge1)
    v = (q @ direction) * inverse
    t = np.einsum("ij,ij->i", edge2, q) * inverse
    blocked = valid & (u >= 0) & (v >= 0) & (u + v <= 1)
    blocked &= (t > 1.e-5) & (t < 1 - 1.e-4)
    return not np.any(blocked)


def sample_bilinear(image, pixels):
    base = np.floor(pixels).astype(int)
    fraction = pixels - base
    x, y = base.T
    fx, fy = fraction.T
    return (image[y, x] * ((1-fx)*(1-fy))[:, None]
            + image[y, x+1] * (fx*(1-fy))[:, None]
            + image[y+1, x] * ((1-fx)*fy)[:, None]
            + image[y+1, x+1] * (fx*fy)[:, None])


def sample_patch(image, pixels):
    result = np.zeros((len(pixels), 3), dtype=float)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            result += sample_bilinear(image, pixels + (dx, dy)) / 9
    return result


def terrain_patches(triangles, radius):
    """Nine fixed points on each triangle's plane, within its actual surface.

    Return a mask rather than shrinking patches at triangle edges: every
    accepted patch must retain the requested physical extent across all frames.
    """
    if not np.isfinite(radius) or radius <= 0:
        raise ValueError("World patch radius must be finite and positive")
    edge1, edge2 = triangles[:, 1]-triangles[:, 0], triangles[:, 2]-triangles[:, 0]
    normal = np.cross(edge1, edge2)
    lengths = np.linalg.norm(normal, axis=1)
    valid = lengths > 1.e-10
    tangent = edge1 / np.maximum(np.linalg.norm(edge1, axis=1), 1.e-10)[:, None]
    normal = normal / np.maximum(lengths, 1.e-10)[:, None]
    bitangent = np.cross(normal, tangent)
    offsets = np.asarray([(x, y) for x in (-1, 0, 1) for y in (-1, 0, 1)]) * radius
    points = (triangles.mean(axis=1)[:, None, :]
              + offsets[None, :, 0:1]*tangent[:, None, :]
              + offsets[None, :, 1:2]*bitangent[:, None, :])
    offset = points-triangles[:, None, 0]
    a = np.einsum("ij,ij->i", edge1, edge1)
    b = np.einsum("ij,ij->i", edge1, edge2)
    c = np.einsum("ij,ij->i", edge2, edge2)
    d = np.einsum("ikj,ij->ik", offset, edge1)
    e = np.einsum("ikj,ij->ik", offset, edge2)
    denominator = np.maximum(a*c-b*b, 1.e-20)[:, None]
    u, v = (c[:, None]*d-b[:, None]*e)/denominator, (a[:, None]*e-b[:, None]*d)/denominator
    valid &= ((u >= -1.e-8) & (v >= -1.e-8) & (u+v <= 1+1.e-8)).all(axis=1)
    return points, valid


def projected_patch_minor_axis(pixels):
    """Approximate minor projected diameter, independent of screen rotation."""
    axes = np.stack((pixels[:, 7]-pixels[:, 1], pixels[:, 5]-pixels[:, 3]), axis=2)
    return np.linalg.svd(axes, compute_uv=False)[:, -1]


def temporal_statistics(values):
    changes = np.abs(np.diff(values, axis=0)).max(axis=2)
    excursion = np.ptp(values, axis=0).max(axis=1)
    return dict(mean_step_rgb=float(changes.mean()),
                p95_step_rgb=float(np.percentile(changes, 95)),
                maximum_step_rgb=float(changes.max()),
                samples_excursion_over_8=int((excursion > 8).sum()),
                p95_excursion_rgb=float(np.percentile(excursion, 95)))


def matched_phases(rows):
    phases = {phase: [r for r in rows if r["phase"] == phase and r["frame"] % 4 == 0]
              for phase in dict.fromkeys(r["phase"] for r in rows)}
    if "frozen" not in phases or "animated" not in phases or len(phases["frozen"]) < 2:
        raise ValueError("Need matched frozen and animated sequences")
    for phase, frames in phases.items():
        if len(frames) != len(phases["frozen"]):
            raise ValueError(f"Unmatched phase length: {phase}")
        for first, second in zip(phases["frozen"], frames):
            if phases["frozen"][0].get("walk_view") != second.get("walk_view"):
                raise ValueError(f"Unmatched walk_view: {phase}")
            fields = ["frame", "camera", "view_projection_column_major", "width", "height"]
            if phase != "animated":
                fields.append("time")
            for field in fields:
                if first[field] != second[field]:
                    raise ValueError(f"Unmatched {field}: {phase}")
    return phases


def paired_statistics(first, second):
    difference = np.abs(first - second).max(axis=2)
    return dict(mean_rgb=float(difference.mean()),
                p95_rgb=float(np.percentile(difference, 95)),
                maximum_rgb=float(difference.max()),
                samples_over_8=int((difference.max(axis=0) > 8).sum()))


def check_retained_reference(result, maximum_error=8.):
    key = "unrestricted_vs_unrestricted-full"
    if key not in result["paired_comparisons"]:
        raise ValueError("Missing matched unrestricted retained/full reference")
    if not result.get("partial_depth_clear_supported", False):
        return False
    error = result["paired_comparisons"][key]["all_samples"]["maximum_rgb"]
    if error > maximum_error:
        raise ValueError(f"Retained shadow reference error {error:.3f} exceeds {maximum_error:.3f} RGB levels")
    return True


def matched_resolution_phases(rows, reference_rows):
    phases, reference = matched_phases(rows), matched_phases(reference_rows)
    if phases.keys() != reference.keys():
        raise ValueError("Unmatched resolution phases")
    fields = ("frame", "camera", "view_projection_column_major", "time", "walk_view",
              "forest_caster_range", "forest_caster_fade_width", "oak_wind_meshes", "forced_full_refresh")
    for phase, frames in phases.items():
        if len(frames) != len(reference[phase]):
            raise ValueError("Unmatched resolution frame count")
        for row, other in zip(frames, reference[phase]):
            if row["width"]*other["height"] != row["height"]*other["width"]:
                raise ValueError("Unmatched resolution aspect ratio")
            for field in fields:
                if field not in row or field not in other or row[field] != other[field]:
                    raise ValueError(f"Unmatched resolution {field}: {phase}")
    return phases


def analyze(root, backend, maximum_samples, world_radius=None, reference_root=None):
    rows = [json.loads(line) for line in (root/f"{backend}-walk-ground.jsonl").read_text().splitlines()]
    phases = matched_phases(rows)
    reference_result = None
    if reference_root is not None:
        if world_radius is None:
            raise ValueError("Resolution comparison requires fixed world patches")
        reference_rows = [json.loads(line) for line in (reference_root/f"{backend}-walk-ground.jsonl").read_text().splitlines()]
        phases = matched_resolution_phases(rows, reference_rows)
        if json.loads((root/f"{backend}-ground-geometry.json").read_text()) != json.loads(
                (reference_root/f"{backend}-ground-geometry.json").read_text()):
            raise ValueError("Unmatched resolution terrain geometry")
        reference_result = analyze(reference_root, backend, maximum_samples, world_radius)
    frozen = phases["frozen"]
    geometry = json.loads((root/f"{backend}-ground-geometry.json").read_text())
    triangles = np.concatenate([np.asarray(m["vertices"], dtype=float)[
        np.asarray(m["indices"], dtype=int).reshape(-1, 3)] for m in geometry])
    centers = triangles.mean(axis=1)
    valid = np.ones(len(centers), dtype=bool)
    for row in frozen:
        _, visible = project(centers, row["view_projection_column_major"], row["width"], row["height"])
        valid &= visible
    candidates = centers[valid]
    candidate_triangles = triangles[valid]
    if reference_result is not None:
        indices = []
        for point in reference_result["sample_world_positions"]:
            distances = np.linalg.norm(centers-point, axis=1)
            index = int(np.argmin(distances))
            if distances[index] > 1.e-6 or not valid[index]:
                raise ValueError("Resolution reference sample missing or outside view")
            indices.append(index)
        candidates, candidate_triangles = centers[indices], triangles[indices]
    # Spread candidates over the original mesh order; never select by observed
    # temporal stability or discard samples merely because their shadows vary.
    if reference_result is None and len(candidates) > maximum_samples:
        indices = np.linspace(0, len(candidates)-1, maximum_samples, dtype=int)
        candidates, candidate_triangles = candidates[indices], candidate_triangles[indices]
    visible = [all(terrain_visible(p, np.asarray(r["camera"]), triangles) for r in frozen)
               for p in candidates]
    points = candidates[visible]
    patches = None
    if world_radius is not None:
        patches, patch_valid = terrain_patches(candidate_triangles[visible], world_radius)
        for row in frozen:
            _, in_view = project(patches.reshape(-1, 3), row["view_projection_column_major"], row["width"], row["height"])
            patch_valid &= in_view.reshape(-1, 9).all(axis=1)
        for index in np.flatnonzero(patch_valid):
            patch_valid[index] = all(terrain_visible(p, np.asarray(r["camera"]), triangles)
                                     for r in frozen for p in patches[index])
        points, patches = points[patch_valid], patches[patch_valid]
    if len(points) < 16:
        raise ValueError(f"Only {len(points)} visible fixed terrain samples")
    if reference_result is not None and (len(points) != len(reference_result["sample_world_positions"]) or
            not np.allclose(points, reference_result["sample_world_positions"], rtol=0, atol=1.e-6)):
        raise ValueError("Resolution reference patch set changed")
    colors = {}
    display_colors, display_controls = {}, []
    projected_patch_spans = []
    projected_minor_axes = []
    controls = []
    bands = []
    for phase, frames in phases.items():
        samples = []
        display_samples = []
        for row in frames:
            frame = row["frame"]
            pixels, _ = project(points, row["view_projection_column_major"], row["width"], row["height"])
            image = read_ppm(root/f"{backend}-walk-ground-{phase}-{frame}.ppm")
            if image.shape[:2] != (row["height"], row["width"]):
                raise ValueError("Trace and image dimensions differ")
            display_samples.append(sample_patch(image, pixels))
            patch_pixels = None
            if patches is not None:
                patch_pixels, valid = project(patches.reshape(-1, 3), row["view_projection_column_major"], row["width"], row["height"])
                if not valid.all():
                    raise ValueError("World patch left matched view")
                samples.append(sample_bilinear(image, patch_pixels).reshape(-1, 9, 3).mean(axis=1))
            else:
                samples.append(display_samples[-1])
            if phase == "frozen":
                control = read_ppm(root/f"{backend}-walk-ground-control-{frame}.ppm")
                if control.shape != image.shape:
                    raise ValueError("Control image dimensions differ")
                display_controls.append(sample_patch(control, pixels))
                if patches is not None:
                    controls.append(sample_bilinear(control, patch_pixels).reshape(-1, 9, 3).mean(axis=1))
                    projected_patch_spans.append(np.ptp(patch_pixels.reshape(-1, 9, 2), axis=1))
                    projected_minor_axes.append(projected_patch_minor_axis(patch_pixels.reshape(-1, 9, 2)))
                else:
                    controls.append(display_controls[-1])
                distances = np.linalg.norm(points - row["camera"], axis=1)
                bands.append(np.searchsorted([c["split"] for c in row["cascades"]], distances))
        colors[phase] = np.asarray(samples)
        display_colors[phase] = np.asarray(display_samples)
    controls = np.asarray(controls)
    deficits = {phase: controls - values for phase, values in colors.items()}
    shadowed = deficits["frozen"].max(axis=(0, 2)) > 2
    if shadowed.sum() < 8:
        raise ValueError("Insufficient samples with measurable ground shadows")
    control_excursion = np.ptp(controls, axis=0).max(axis=1)
    frozen_excursion = np.ptp(deficits["frozen"], axis=0).max(axis=1)
    control_stable_shadowed = shadowed & (control_excursion <= 2)
    footprint_comparison = None
    if patches is not None:
        display_controls = np.asarray(display_controls)
        display_deficits = {phase: display_controls-values for phase, values in display_colors.items()}
        common_stable = (control_stable_shadowed
                         & (np.ptp(display_controls, axis=0).max(axis=1) <= 2)
                         & (display_deficits["frozen"].max(axis=(0, 2)) > 2))
        at_least_one_pixel = np.min(projected_minor_axes, axis=0) >= 1
        pixel_stable = at_least_one_pixel & common_stable
        footprint_comparison = dict(
            common_control_stable_shadowed_samples=int(common_stable.sum()),
            patches_minor_axis_below_one_pixel=int((~at_least_one_pixel).sum()),
            common_control_stable_at_least_one_pixel_samples=int(pixel_stable.sum()),
            display_phase_statistics={phase: temporal_statistics(values) for phase, values in display_deficits.items()},
            common_control_stable_phase_statistics={phase: dict(
                display=temporal_statistics(display_deficits[phase][:, common_stable]),
                world=temporal_statistics(values[:, common_stable]))
                for phase, values in deficits.items()} if common_stable.any() else {},
            common_control_stable_at_least_one_pixel_phase_statistics={phase: dict(
                display=temporal_statistics(display_deficits[phase][:, pixel_stable]),
                world=temporal_statistics(values[:, pixel_stable]))
                for phase, values in deficits.items()} if pixel_stable.any() else {},
            projected_world_patch_minor_axis_min_pixels=float(np.min(projected_minor_axes)),
            projected_world_patch_span_pixels_min=np.min(projected_patch_spans, axis=(0, 1)).tolist(),
            projected_world_patch_span_pixels_max=np.max(projected_patch_spans, axis=(0, 1)).tolist())
    worst = np.argsort(frozen_excursion)[-10:][::-1]
    pairs = (("frozen", "unrestricted"), ("frozen", "fully-refreshed"),
             ("unrestricted", "unrestricted-full"), ("fully-refreshed", "unrestricted-full"),
             ("frozen", "frozen-repeat"), ("fade4", "fade4-full"), ("fade8", "fade8-full"),
             ("frozen", "fade4"), ("frozen", "fade8"))
    comparisons = {f"{first}_vs_{second}": dict(
        all_samples=paired_statistics(colors[first], colors[second]),
        baseline_shadowed=paired_statistics(colors[first][:, shadowed], colors[second][:, shadowed]))
        for first, second in pairs if first in colors and second in colors}
    resolution_comparison = None
    if reference_result is not None:
        reference_controls = np.asarray(reference_result["control_rgb"])
        reference_deficit = np.asarray(reference_result["frozen_shadow_deficit_rgb"])
        common = (control_stable_shadowed & (np.ptp(reference_controls, axis=0).max(axis=1) <= 2)
                  & (reference_deficit.max(axis=(0, 2)) > 2))
        resolution_comparison = dict(
            reference_directory=str(reference_root),
            reference_size=[reference_rows[0]["width"], reference_rows[0]["height"]],
            current_size=[frozen[0]["width"], frozen[0]["height"]],
            matched_world_samples=len(points),
            reference_phase_statistics=reference_result["phase_statistics"],
            reference_footprint_comparison=reference_result["footprint_comparison"],
            common_control_stable_shadowed_samples=int(common.sum()),
            common_control_stable_frozen=(dict(
                reference=temporal_statistics(reference_deficit[:, common]),
                current=temporal_statistics(deficits["frozen"][:, common])) if common.any() else None))
    return dict(backend=backend,terrain_triangles=len(triangles),samples=len(points),
                resolution_comparison=resolution_comparison,
                sampling="world" if patches is not None else "display",
                world_patch_radius_metres=world_radius,
                footprint_comparison=footprint_comparison,
                walk_view=frozen[0].get("walk_view", "unspecified"),
                advanced_clock_oak_wind_meshes=(min(r["oak_wind_meshes"] for r in phases["animated"])
                    if all("oak_wind_meshes" in r for r in phases["animated"]) else None),
                animation_scope="The animated walk advances the clock with authored wind settings; it does not establish moving tree shadows. Explicit wind-policy captures separately test visible oak deformation and undeformed shadow casters.",
                partial_depth_clear_supported=all(r.get("partial_depth_clear_supported", False) for r in frozen),
                phase_statistics={phase: dict(all_samples=temporal_statistics(values),
                    baseline_shadowed=temporal_statistics(values[:, shadowed]),
                    control_stable_shadowed=(temporal_statistics(values[:, control_stable_shadowed])
                        if control_stable_shadowed.any() else None))
                    for phase, values in deficits.items()},
                paired_comparisons=comparisons,
                shadowed_samples=int(shadowed.sum()),
                receiver_band_crossings=int((np.ptp(bands, axis=0) > 0).sum()),
                control=temporal_statistics(controls),
                frozen_deficit=temporal_statistics(deficits["frozen"]),
                animated_deficit=temporal_statistics(deficits["animated"]),
                frozen_shadowed_deficit=temporal_statistics(deficits["frozen"][:, shadowed]),
                animated_shadowed_deficit=temporal_statistics(deficits["animated"][:, shadowed]),
                control_stable_shadowed_samples=int(control_stable_shadowed.sum()),
                frozen_control_stable_shadowed_deficit=(temporal_statistics(
                    deficits["frozen"][:, control_stable_shadowed])
                    if control_stable_shadowed.any() else None),
                worst_frozen_samples=[dict(world=points[i].tolist(),
                    shadow_excursion_rgb=float(frozen_excursion[i]),
                    control_excursion_rgb=float(control_excursion[i]),
                    receiver_bands=np.asarray(bands)[:, i].tolist()) for i in worst],
                sample_world_positions=points.tolist(),
                frame_indices=[r["frame"] for r in frozen],
                receiver_bands=np.asarray(bands).tolist(),
                control_rgb=controls.tolist(),
                frozen_shadow_deficit_rgb=deficits["frozen"].tolist(),
                animated_shadow_deficit_rgb=deficits["animated"].tolist(),
                limitations="Display-space RGB reconstructed from raster images; nine world points fix sample positions but are not an area integral and cannot undo pixel filtering/aliasing, especially when subpixel. A one-pixel patch diameter does not ensure adequate sampling. Six frames at 0.8 m intervals; diagnostic comparisons are not a visual pass threshold.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--backend", choices=("dx11", "dx12", "vulkan"), required=True)
    parser.add_argument("--max-samples", type=int, default=256)
    parser.add_argument("--world-radius", type=float,
                        help="Use nine terrain-plane samples in a square of this half-width (metres); write a separate world analysis")
    parser.add_argument("--reference-directory", type=Path,
                        help="Require matching route/time/settings/geometry and reuse this capture's exact world sample set")
    parser.add_argument("--check-retained-reference", action="store_true",
                        help="Require unrestricted retained/full captures to agree within 8 RGB levels")
    args = parser.parse_args()
    if args.max_samples < 16:
        parser.error("--max-samples must be at least 16")
    if args.world_radius is not None and (not np.isfinite(args.world_radius) or args.world_radius <= 0):
        parser.error("--world-radius must be finite and positive")
    if args.reference_directory is not None and args.world_radius is None:
        parser.error("--reference-directory requires --world-radius")
    result = analyze(args.directory, args.backend, args.max_samples, args.world_radius, args.reference_directory)
    suffix = "-world" if args.world_radius is not None else ""
    output = args.directory/f"{args.backend}{suffix}-ground-analysis.json"
    output.write_text(json.dumps(result, indent=2))
    if args.check_retained_reference:
        if not check_retained_reference(result):
            print("Skipped retained reference: backend requires full-atlas refresh")
            raise SystemExit(77)
    print(json.dumps({k:v for k,v in result.items() if not isinstance(v, list)}, indent=2))


if __name__ == "__main__":
    main()
