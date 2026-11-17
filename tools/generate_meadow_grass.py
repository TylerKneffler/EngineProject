#!/usr/bin/env python3
"""Generate clumped meadow grass and low ground-cover shrubs for GrassField.

Writes Engine/Core/Assets/Mesh/Grass/<plant>.obj plus <plant>_lod1.obj and
<plant>_lod2.obj:

  meadow_tussock   short meadow patch: a few dense tussocks over a low sward
                   that closes the ground between them (GrassField.shortMesh)
  field_tussock    tall grass tussock with seed stalks (GrassField.tallMesh)
  meadow_shrub     low, lumpy small-leaved shrub (GrassField.shrubMeshes)
  meadow_bramble   arching canes with leaves and a few flowers
  meadow_fern      bracken-like fronds

The scattered-blade patches (meadow_short / field_tall) spread single blades
evenly, so the soil shows between every blade. Real meadow grass grows in
tussocks: many narrow blades from one crown, fanning outward, with a short
sward filling the gaps. These patches spend the same kind of triangle budget
on that structure instead.

Conventions match tools/generate_meadow_accents.py (metres, Y up, root at the
origin, linear vertex colours, v = height / plant height, u across each
ribbon). The vertex colour alpha is the plant's wind response: 1 bends like a
grass blade, lower values stiffen woody plants (GrassFootBend.hlsl scales the
wind and foot-bend height by it for instanced grass draws).

Every plant draws its layout from a fixed seed before any detail decision, so
the three LODs keep the same silhouette. Coarser LODs drop blades and leaves
and widen the ones they keep, so coverage holds with distance.

Usage: python tools/generate_meadow_grass.py
"""
from __future__ import annotations

import math
import random
from pathlib import Path

import generate_meadow_accents as accents
from generate_meadow_accents import (UP, add, cross, horizontal, mix, mul, norm,
                                     perpendicular, sub)

OUT = accents.OUT


class Mesh(accents.Mesh):
    """Accent mesh with a per-plant wind response in vertex alpha."""

    def __init__(self, wind_response=1.0):
        super().__init__()
        self.wind_response = wind_response

    def write(self, path: Path, header: str):
        triangles = super().write(path, header)
        if self.wind_response >= 1.0:
            return triangles
        alpha = f" {self.wind_response:.3f}"
        lines = path.read_text(encoding="ascii").splitlines()
        lines = [line + alpha if line.startswith("v ") else line for line in lines]
        lines.insert(2, f"# Vertex alpha {self.wind_response:.3f}: wind response (1 = grass blade).")
        path.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
        return triangles


# --- grass ----------------------------------------------------------------

BLADE_ROOT = (0.045, 0.12, 0.025)
BLADE_MID = (0.13, 0.28, 0.055)
BLADE_TIP = (0.25, 0.40, 0.10)
DRY_TIP = (0.50, 0.44, 0.22)
SWARD_ROOT = (0.05, 0.13, 0.03)
SWARD_MID = (0.11, 0.25, 0.05)
SWARD_TIP = (0.20, 0.36, 0.08)


def blade(mesh, base, yaw, length_, rise, droop, width, segments,
          root=BLADE_ROOT, mid=BLADE_MID, tip=BLADE_TIP, twist=0.0):
    """A narrow blade rising at `rise` (radians from horizontal) and arching
    over; the last row closes to a point, so it costs 2 * segments - 1
    triangles."""
    direction = horizontal(yaw)
    start = add(mul(direction, math.cos(rise)), mul(UP, math.sin(rise)))
    points, sides, widths, colors = [], [], [], []
    position = base
    heading = start
    step = length_ / segments
    for i in range(segments + 1):
        t = i / segments
        points.append(position)
        side = horizontal(yaw + math.pi * 0.5 + twist * t)
        sides.append(perpendicular(heading, side))
        widths.append(width * (1.0 - t ** 1.6) if i < segments else 0.0)
        colors.append(mix(root, mid, t / 0.5) if t < 0.5 else mix(mid, tip, (t - 0.5) / 0.5))
        # Bend the heading toward the ground a little more each step.
        heading = norm(add(heading, mul(add(direction, mul(UP, -1.0)), droop / segments)))
        position = add(position, mul(heading, step))
    rows = []
    for i, (p, s, w, c) in enumerate(zip(points, sides, widths, colors)):
        if i == segments:
            rows.append([mesh.vertex(p, c, 0.5)])
        else:
            rows.append([mesh.vertex(add(p, mul(s, -0.5 * w)), c, 0.0),
                         mesh.vertex(add(p, mul(s, 0.5 * w)), c, 1.0)])
    for a, b in zip(rows, rows[1:]):
        if len(b) == 1:
            mesh.triangle(a[0], b[0], a[1])
        else:
            mesh.quad(a[0], a[1], b[0], b[1])
    return points[-1]


# Detail levels keep these shares of the blades and widen the survivors.
GRASS_KEEP = (1.0, 0.55, 0.32)
GRASS_WIDEN = (1.0, 1.5, 2.3)
GRASS_SEGMENTS = (4, 3, 2)


def tussock(mesh, rng, center, blades, height, crown, detail, dry_share=0.1, width=0.017):
    """Blades from one crown: inner ones upright, outer ones leaning out."""
    for _ in range(blades):
        radius, angle = math.sqrt(rng.random()), rng.random() * 2.0 * math.pi
        keep, length_jitter, rise_jitter = rng.random(), rng.random(), rng.random()
        droop, dry, twist = 0.08 + 0.3 * rng.random(), rng.random(), (rng.random() - 0.5) * 1.2
        yaw = angle + (rng.random() - 0.5) * 0.9
        if keep >= GRASS_KEEP[detail]:
            continue
        base = add(center, (crown * radius * math.cos(angle), 0.0, crown * radius * math.sin(angle)))
        # Upright core, splayed rim: the tussock reads as a fountain.
        rise = math.radians(86.0 - 34.0 * radius ** 1.5 - 12.0 * rise_jitter)
        length_ = height * (0.6 + 0.45 * length_jitter) * (1.08 - 0.2 * radius)
        tip = DRY_TIP if dry < dry_share else BLADE_TIP
        blade(mesh, base, yaw, length_, rise, droop, width * GRASS_WIDEN[detail],
              GRASS_SEGMENTS[detail], tip=tip, twist=twist)


def sward(mesh, rng, count, radius, detail, height=0.22, width=0.02):
    """Short, low-lying blades over the whole patch; they hide the soil."""
    keep_share = (1.0, 0.5, 0.35)[detail]
    widen = (1.0, 1.35, 1.9)[detail]
    for base in accents.scatter(rng, count, radius):
        keep, yaw, length_, rise = rng.random(), rng.random() * 6.283, rng.random(), rng.random()
        if keep >= keep_share:
            continue
        blade(mesh, base, yaw, height * (0.45 + 0.55 * length_), math.radians(22.0 + 38.0 * rise),
              0.45, width * widen, (2, 2, 1)[detail], root=SWARD_ROOT, mid=SWARD_MID, tip=SWARD_TIP)


def meadow_tussock(detail):
    rng = random.Random(5101)
    mesh = Mesh()
    layout = [(rng.uniform(-0.4, 0.4), rng.uniform(-0.4, 0.4), rng.random()) for _ in range(4)]
    for index, (x, z, size) in enumerate(layout):
        crown_rng = random.Random(5200 + index)
        tussock(mesh, crown_rng, (x, 0.0, z), 16 + int(7 * size), 0.3 + 0.14 * size,
                0.06 + 0.04 * size, detail)
    sward(mesh, random.Random(5300), 84, 0.6, detail)
    return mesh


SEED_STEM = (0.18, 0.26, 0.08)
SEED_HEAD = (0.46, 0.40, 0.22)
SEED_HEAD_TIP = (0.58, 0.50, 0.30)


def seed_stalk(mesh, base, yaw, height, lean, detail):
    top, axis = accents.stem(mesh, base, yaw, height, lean, 0.0028, (4, 3, 2)[detail],
                             root=BLADE_ROOT, top=SEED_STEM)
    # Panicle: crossed tapering ribbons continuing the stem.
    spike = [add(top, mul(axis, 0.07 * i / 3)) for i in range(4)]
    for turn in ((0.0, 1.0) if detail < 2 else (0.0,)):
        side = perpendicular(axis, horizontal(yaw + math.pi * (0.5 + 0.5 * turn)))
        mesh.ribbon(spike, [side] * 4, [0.010, 0.016, 0.012, 0.004],
                    [SEED_HEAD, SEED_HEAD, SEED_HEAD_TIP, SEED_HEAD_TIP])


def field_tussock(detail):
    rng = random.Random(5401)
    mesh = Mesh()
    layout = [(rng.uniform(-0.18, 0.18), rng.uniform(-0.18, 0.18), rng.random()) for _ in range(2)]
    for index, (x, z, size) in enumerate(layout):
        crown_rng = random.Random(5500 + index)
        tussock(mesh, crown_rng, (x, 0.0, z), 26 + int(8 * size), 0.6 + 0.2 * size,
                0.06 + 0.03 * size, detail, dry_share=0.16, width=0.015)
    stalks = random.Random(5600)
    for base in accents.scatter(stalks, 5, 0.12):
        yaw, height, lean, keep = stalks.random() * 6.283, 0.6 + 0.2 * stalks.random(), \
            0.06 + 0.12 * stalks.random(), stalks.random()
        if keep < (1.0, 0.7, 0.45)[detail]:
            seed_stalk(mesh, base, yaw, height, lean, detail)
    sward(mesh, random.Random(5700), 20, 0.35, detail)
    return mesh


# --- shrubs ---------------------------------------------------------------

def leaf_card(mesh, base, outward, up_hint, length_, width, color_base, color_tip, fold=0.0):
    """An ovate leaf: base, two shoulders and a tip (two triangles, four with
    a fold). outward points from base to tip."""
    side = perpendicular(outward, cross(up_hint, outward) if abs(
        sum(a * b for a, b in zip(up_hint, outward))) < 0.98 else (1.0, 0.0, 0.0))
    normal = norm(cross(side, outward))
    shoulder = add(base, mul(outward, 0.42 * length_))
    lift = mul(normal, fold * width)
    a = mesh.vertex(base, color_base, 0.5)
    b = mesh.vertex(add(shoulder, mul(side, -0.5 * width)), mix(color_base, color_tip, 0.45), 0.0)
    c = mesh.vertex(add(shoulder, mul(side, 0.5 * width)), mix(color_base, color_tip, 0.45), 1.0)
    d = mesh.vertex(add(base, mul(outward, length_)), color_tip, 0.5)
    if fold > 0.0:
        m = mesh.vertex(add(shoulder, lift), mix(color_base, color_tip, 0.5), 0.5)
        for x, y in ((a, b), (b, d), (d, c), (c, a)):
            mesh.triangle(x, y, m)
    else:
        mesh.triangle(a, b, d)
        mesh.triangle(a, d, c)


TWIG = (0.20, 0.15, 0.09)
TWIG_DARK = (0.10, 0.08, 0.05)
SHRUB_LEAF_SHADE = (0.025, 0.075, 0.02)
SHRUB_LEAF_SUN = (0.09, 0.22, 0.05)
LEAF_KEEP = (1.0, 0.46, 0.2)
LEAF_GROW = (1.0, 1.45, 2.15)


def branch(mesh, base, tip, width, segments, color_base=TWIG_DARK, color_tip=TWIG):
    axis = sub(tip, base)
    length_ = math.sqrt(sum(a * a for a in axis))
    axis = norm(axis)
    side = perpendicular(axis, horizontal(math.atan2(axis[2], axis[0]) + math.pi * 0.5))
    points = [add(base, mul(axis, length_ * i / segments)) for i in range(segments + 1)]
    widths = [width * (1.0 - 0.6 * i / segments) for i in range(segments + 1)]
    colors = [mix(color_base, color_tip, i / segments) for i in range(segments + 1)]
    mesh.ribbon(points, [side] * len(points), widths, colors)


def lobed_dome(rng, lobes, radius, height):
    """Overlapping ellipsoids: a lumpy, ground-hugging crown."""
    result = []
    for _ in range(lobes):
        angle, reach = rng.random() * 6.283, rng.random()
        result.append(((radius * 0.55 * reach * math.cos(angle), 0.0, radius * 0.55 * reach * math.sin(angle)),
                       radius * (0.55 + 0.25 * rng.random()), height * (0.7 + 0.3 * rng.random())))
    return result


def meadow_shrub(detail):
    """Small-leaved mound (bilberry / box-like) about 0.4 m tall."""
    rng = random.Random(6101)
    mesh = Mesh(wind_response=0.3)
    radius, height = 0.6, 0.6
    lobes = lobed_dome(rng, 4, radius, height)
    # Woody stems from the root to each lobe; hidden by leaves up close but
    # they give coarse LODs a dark core.
    for center, lobe_radius, lobe_height in lobes:
        top = add(center, (0.0, lobe_height * 0.6, 0.0))
        branch(mesh, (0.0, 0.0, 0.0), top, 0.012, (3, 2, 1)[detail])
    for _ in range(300):
        lobe = lobes[int(rng.random() * len(lobes))]
        u, v, keep, size, tint, spin = (rng.random() for _ in range(6))
        if keep >= LEAF_KEEP[detail]:
            continue
        center, lobe_radius, lobe_height = lobe
        # Points on the upper part of the lobe's ellipsoid, a little inside.
        azimuth = u * 2.0 * math.pi
        elevation = math.asin(0.05 + 0.95 * v)
        shell = 0.6 + 0.4 * size ** 0.5
        normal = (math.cos(elevation) * math.cos(azimuth), math.sin(elevation),
                  math.cos(elevation) * math.sin(azimuth))
        base = add(center, (normal[0] * lobe_radius * shell, normal[1] * lobe_height * shell,
                            normal[2] * lobe_radius * shell))
        outward = norm(add(normal, (math.cos(spin * 6.283) * 0.6, 0.25, math.sin(spin * 6.283) * 0.6)))
        sun = 0.35 + 0.65 * normal[1]
        base_color = mix(SHRUB_LEAF_SHADE, SHRUB_LEAF_SUN, sun * 0.7)
        tip_color = mix(SHRUB_LEAF_SHADE, SHRUB_LEAF_SUN, min(1.0, sun * (0.85 + 0.3 * tint)))
        grow = LEAF_GROW[detail]
        leaf_card(mesh, base, outward, UP, 0.11 * grow * (0.8 + 0.4 * size),
                  0.078 * grow * (0.8 + 0.4 * size), base_color, tip_color)
    return mesh


BRAMBLE_CANE = (0.26, 0.11, 0.08)
BRAMBLE_CANE_TIP = (0.22, 0.20, 0.08)
BRAMBLE_LEAF_BASE = (0.03, 0.085, 0.025)
BRAMBLE_LEAF_TIP = (0.08, 0.19, 0.04)


def meadow_bramble(detail):
    """Arching canes that root again at their tips: a low, wide tangle."""
    rng = random.Random(6201)
    mesh = Mesh(wind_response=0.45)
    accents.head_scale = accents.HEAD_SCALES[detail] * 0.8
    for _ in range(9):
        yaw, reach, peak, keep = rng.random() * 6.283, 0.55 + 0.35 * rng.random(), \
            0.36 + 0.22 * rng.random(), rng.random()
        leaves = [(rng.random(), rng.random(), rng.random(), rng.random()) for _ in range(11)]
        flower = rng.random()
        segments = (8, 5, 3)[detail]
        direction = horizontal(yaw)
        points = []
        for i in range(segments + 1):
            t = i / segments
            # A parabola from the root, over the peak, back to the ground.
            points.append(add(mul(direction, reach * t), (0.0, 4.0 * peak * t * (1.0 - t) + 0.01, 0.0)))
        side = horizontal(yaw + math.pi * 0.5)
        mesh.ribbon(points, [side] * len(points), [0.007 * (1.0 - 0.5 * i / segments) for i in range(segments + 1)],
                    [mix(BRAMBLE_CANE, BRAMBLE_CANE_TIP, i / segments) for i in range(segments + 1)])
        for at, turn, size, keep_leaf in leaves:
            if keep_leaf >= LEAF_KEEP[detail]:
                continue
            t = 0.12 + 0.8 * at
            index = min(segments, int(round(t * segments)))
            base = points[index]
            outward = norm(add(horizontal(yaw + (turn - 0.5) * 2.6), (0.0, 0.55, 0.0)))
            grow = LEAF_GROW[detail]
            # Trifoliate: a centre leaflet and two side ones.
            for offset, scale in ((0.0, 1.0), (0.8, 0.75), (-0.8, 0.75)):
                if detail == 2 and offset != 0.0:
                    continue
                leaflet = norm(add(outward, mul(horizontal(yaw + offset + turn * 3.0), 0.5)))
                leaf_card(mesh, base, leaflet, UP, 0.095 * scale * grow * (0.8 + 0.4 * size),
                          0.065 * scale * grow * (0.8 + 0.4 * size), BRAMBLE_LEAF_BASE, BRAMBLE_LEAF_TIP)
        if flower < 0.5 and detail < 2:
            top = points[segments // 2]
            accents.petal_ring(mesh, add(top, (0.0, 0.012, 0.0)), UP, 5, 0.003, 0.010, 0.009, 0.3,
                               (0.80, 0.74, 0.72), (0.92, 0.86, 0.86), 1, flower * 12.0)
            accents.disc(mesh, add(top, (0.0, 0.014, 0.0)), UP, 0.004, 0.002, 5,
                         (0.70, 0.62, 0.20), (0.60, 0.55, 0.20))
    return mesh


FERN_RACHIS = (0.10, 0.20, 0.05)
FERN_PINNA_BASE = (0.05, 0.14, 0.03)
FERN_PINNA_TIP = (0.17, 0.35, 0.07)


def meadow_fern(detail):
    """Bracken: arching fronds with paired pinnae."""
    rng = random.Random(6301)
    mesh = Mesh(wind_response=0.85)
    for k in range(7):
        yaw = k * 0.9 + rng.random() * 0.6
        length_, rise, droop = 0.62 + 0.25 * rng.random(), math.radians(52 + 22 * rng.random()), \
            0.5 + 0.4 * rng.random()
        segments = (8, 5, 3)[detail]
        direction = horizontal(yaw)
        side = horizontal(yaw + math.pi * 0.5)
        points, tangents = [], []
        position = (0.0, 0.0, 0.0)
        heading = add(mul(direction, math.cos(rise)), mul(UP, math.sin(rise)))
        for i in range(segments + 1):
            points.append(position)
            tangents.append(heading)
            heading = norm(add(heading, mul(add(direction, mul(UP, -1.0)), droop / segments)))
            position = add(position, mul(heading, length_ / segments))
        mesh.ribbon(points, [side] * len(points), [0.006 * (1 - 0.6 * i / segments) for i in range(segments + 1)],
                    [FERN_RACHIS] * len(points))
        if detail == 2:
            # One broad tapering blade stands in for the pinnae.
            normals = [norm(cross(side, t)) for t in tangents]
            widths = [0.24 * math.sin(math.pi * min(1.0, (0.15 + i / segments) * 0.9)) for i in range(segments + 1)]
            mesh.ribbon([add(p, mul(n, 0.004)) for p, n in zip(points, normals)], [side] * len(points),
                        widths, [mix(FERN_PINNA_BASE, FERN_PINNA_TIP, i / segments) for i in range(segments + 1)],
                        columns=3, fold=-0.15, normals=normals)
            continue
        pairs = (12, 7)[detail]
        for p in range(pairs):
            t = 0.22 + 0.74 * p / pairs
            index = t * segments
            low = int(index)
            frac = index - low
            high = min(segments, low + 1)
            at = add(mul(points[low], 1 - frac), mul(points[high], frac))
            tangent = norm(add(mul(tangents[low], 1 - frac), mul(tangents[high], frac)))
            span = 0.19 * math.sin(math.pi * min(1.0, 0.25 + 0.85 * (1.0 - t))) * (1.0 if detail == 0 else 1.15)
            for sign in (-1.0, 1.0):
                out = norm(add(mul(side, sign), mul(tangent, 0.35)))
                out = norm(add(out, (0.0, -0.18, 0.0)))
                pinna = [add(at, mul(out, span * i / 2)) for i in range(3)]
                across = perpendicular(out, tangent)
                mesh.ribbon(pinna, [across] * 3, [0.055 * (1.0 if detail == 0 else 1.6),
                                                  0.045 * (1.0 if detail == 0 else 1.6), 0.0],
                            [FERN_PINNA_BASE, mix(FERN_PINNA_BASE, FERN_PINNA_TIP, 0.6), FERN_PINNA_TIP])
    return mesh


PLANTS = {
    "meadow_tussock": (meadow_tussock, "four grass tussocks over a low sward"),
    "field_tussock": (field_tussock, "tall grass tussocks with seed stalks"),
    "meadow_shrub": (meadow_shrub, "low lumpy small-leaved shrub"),
    "meadow_bramble": (meadow_bramble, "arching bramble canes with leaves and flowers"),
    "meadow_fern": (meadow_fern, "bracken-like fern fronds"),
}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, (build, description) in PLANTS.items():
        counts = []
        for detail, suffix in enumerate(("", "_lod1", "_lod2")):
            accents.head_scale = accents.HEAD_SCALES[detail]
            mesh = build(detail)
            counts.append(mesh.write(OUT / f"{name}{suffix}.obj", f"{name}: {description}"))
        print(f"{name}: triangles LOD0/1/2 = {counts[0]}/{counts[1]}/{counts[2]}")


if __name__ == "__main__":
    main()
