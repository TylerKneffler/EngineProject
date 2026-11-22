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
from generate_meadow_accents import (UP, add, cross, dot, horizontal, mix, mul, norm,
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

# Linear albedo near measured grass (~0.06/0.12/0.03 mid-blade): the earlier
# palette was about twice as bright and far more saturated, which read as
# cartoon lime (.md/todo.md Foliage Phase 1d, "cartoonish" report).
BLADE_ROOT = (0.03, 0.065, 0.018)
BLADE_MID = (0.065, 0.125, 0.032)
BLADE_TIP = (0.115, 0.175, 0.055)
DRY_TIP = (0.30, 0.26, 0.13)
SWARD_ROOT = (0.03, 0.07, 0.02)
SWARD_MID = (0.058, 0.115, 0.03)
SWARD_TIP = (0.10, 0.16, 0.048)


def smoothstep(edge0, edge1, x):
    t = min(1.0, max(0.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


def blade_width(t):
    """Grass leaf outline along its length (0 root, 1 tip): a narrow sheath,
    widest about a fifth of the way up, long parallel margins, then a long
    tapering tip. A straight root-to-tip taper reads as a flat triangle."""
    sheath = 0.55 + 0.45 * smoothstep(0.0, 0.2, t)
    taper = 1.0 if t < 0.55 else max(0.0, (1.0 - t) / 0.45) ** 0.75
    return sheath * taper


def blade(mesh, base, yaw, length_, rise, droop, width, segments,
          root=BLADE_ROOT, mid=BLADE_MID, tip=BLADE_TIP, twist=0.0,
          columns=2, fold=0.0, shade=1.0):
    """A narrow blade rising at `rise` (radians from horizontal) and arching
    over; the last row closes to a point. columns=3 folds it along a sunken
    midrib (a V channel, as real blades are) so it catches light unevenly
    instead of shading as one flat strip. Costs (columns - 1) * (2 * segments
    - 1) triangles. shade scales the blade's brightness."""
    direction = horizontal(yaw)
    start = add(mul(direction, math.cos(rise)), mul(UP, math.sin(rise)))
    points, sides, faces, widths, colors = [], [], [], [], []
    position = base
    heading = start
    step = length_ / segments
    for i in range(segments + 1):
        t = i / segments
        points.append(position)
        side = perpendicular(heading, horizontal(yaw + math.pi * 0.5 + twist * t))
        sides.append(side)
        faces.append(norm(cross(side, heading)))
        widths.append(width * blade_width(t))
        color = mix(root, mid, t / 0.5) if t < 0.5 else mix(mid, tip, (t - 0.5) / 0.5)
        colors.append(mul(color, shade))
        # Bend the heading toward the ground a little more each step.
        heading = norm(add(heading, mul(add(direction, mul(UP, -1.0)), droop / segments)))
        position = add(position, mul(heading, step))
    rows = []
    for i, (p, s, f, w, c) in enumerate(zip(points, sides, faces, widths, colors)):
        if i == segments:
            rows.append([mesh.vertex(p, c, 0.5)])
            continue
        row = []
        for k in range(columns):
            u = k / (columns - 1)
            offset = mul(s, (u - 0.5) * w)
            color = c
            if columns == 3 and k == 1:
                # Sunken, slightly paler midrib.
                offset = add(offset, mul(f, -fold * w))
                color = mul(c, 1.07)
            row.append(mesh.vertex(add(p, offset), color, u))
        rows.append(row)
    for a, b in zip(rows, rows[1:]):
        for k in range(columns - 1):
            if len(b) == 1:
                mesh.triangle(a[k], b[0], a[k + 1])
            else:
                mesh.quad(a[k], a[k + 1], b[k], b[k + 1])
    return points[-1]


def tube(mesh, points, tangents, radii, colors, sides=3):
    """A thin round stem: a sides-gon prism along points. Vertex normals are
    averaged around each ring, so even a triangular section shades round,
    and unlike a flat ribbon it never vanishes edge-on."""
    rings = []
    for p, axis, r, c in zip(points, tangents, radii, colors):
        a = perpendicular(axis, (1.0, 0.0, 0.0) if abs(axis[0]) < 0.9 else (0.0, 0.0, 1.0))
        b = norm(cross(axis, a))
        ring = []
        for k in range(sides):
            angle = 2.0 * math.pi * k / sides
            offset = add(mul(a, r * math.cos(angle)), mul(b, r * math.sin(angle)))
            ring.append(mesh.vertex(add(p, offset), c, k / sides))
        rings.append(ring)
    for low, high in zip(rings, rings[1:]):
        for k in range(sides):
            j = (k + 1) % sides
            mesh.quad(low[k], low[j], high[k], high[j])


# Detail levels keep these shares of the blades and widen the survivors.
GRASS_KEEP = (1.0, 0.55, 0.32)
GRASS_WIDEN = (1.0, 1.5, 2.3)
GRASS_SEGMENTS = (5, 4, 3)
GRASS_COLUMNS = (3, 3, 2)
GRASS_FOLD = 0.22  # midrib depth as a share of the blade width


def tussock(mesh, rng, center, blades, height, crown, detail, dry_share=0.22, width=0.017):
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
        # Per-blade brightness from values already drawn, so the layout and
        # every LOD stay identical.
        shade = 0.88 + 0.24 * ((length_jitter * 7.31 + rise_jitter * 3.17) % 1.0)
        blade(mesh, base, yaw, length_, rise, droop, width * GRASS_WIDEN[detail],
              GRASS_SEGMENTS[detail], tip=tip, twist=twist,
              columns=GRASS_COLUMNS[detail], fold=GRASS_FOLD, shade=shade)


def sward(mesh, rng, count, radius, detail, height=0.26, width=0.026):
    """Short, low-lying blades over the whole patch; they hide the soil."""
    keep_share = (1.0, 0.5, 0.35)[detail]
    widen = (1.0, 1.35, 1.9)[detail]
    for base in accents.scatter(rng, count, radius):
        keep, yaw, length_, rise = rng.random(), rng.random() * 6.283, rng.random(), rng.random()
        if keep >= keep_share:
            continue
        blade(mesh, base, yaw, height * (0.45 + 0.55 * length_), math.radians(22.0 + 38.0 * rise),
              0.45, width * widen, (3, 2, 1)[detail], root=SWARD_ROOT, mid=SWARD_MID, tip=SWARD_TIP,
              shade=0.9 + 0.2 * ((length_ * 5.71 + rise * 2.39) % 1.0))


def meadow_tussock(detail):
    rng = random.Random(5101)
    mesh = Mesh()
    layout = [(rng.uniform(-0.4, 0.4), rng.uniform(-0.4, 0.4), rng.random()) for _ in range(4)]
    for index, (x, z, size) in enumerate(layout):
        crown_rng = random.Random(5200 + index)
        tussock(mesh, crown_rng, (x, 0.0, z), 16 + int(7 * size), 0.3 + 0.14 * size,
                0.06 + 0.04 * size, detail)
    # A dense low sward closes the ground between the tussocks.
    sward(mesh, random.Random(5300), 150, 0.65, detail)
    return mesh


SEED_STEM = (0.18, 0.26, 0.08)
SEED_HEAD = (0.46, 0.40, 0.22)
SEED_HEAD_TIP = (0.58, 0.50, 0.30)


def seed_stalk(mesh, base, yaw, height, lean, detail):
    if detail < 2:
        # Round culm: a thin tube that tapers a little toward the head.
        segments = (5, 3)[detail]
        points, tangents = accents.stem_curve(base, yaw, height, lean, segments)
        tube(mesh, points, tangents,
             [0.0017 * (1.0 - 0.35 * i / segments) for i in range(segments + 1)],
             [mix(BLADE_ROOT, SEED_STEM, i / segments) for i in range(segments + 1)])
        top, axis = points[-1], tangents[-1]
    else:
        top, axis = accents.stem(mesh, base, yaw, height, lean, 0.0028, 2,
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

# Leaf outlines per detail level. Read up close, a 4-vertex diamond is the
# "square" look of a low-poly bush, so detail 0 is an ovate leaf with rounded
# sides and a drawn-out tip (10 triangles), detail 1 a coarser one (6) and
# detail 2 the diamond (2). Each entry: midrib points (t along the leaf,
# base and tip implied), margin points (t, half width as a share of the leaf
# width), and one half's triangles over s<i> (midrib incl. base s0 and tip)
# and e<i> (margin), wound base -> margin -> forward for the left half.
LEAF_SHAPES = (
    ((0.3, 0.58), ((0.16, 0.34), (0.42, 0.5), (0.72, 0.34)),
     (("s0", "e0", "s1"), ("e0", "e1", "s1"), ("s1", "e1", "s2"), ("e1", "e2", "s2"), ("s2", "e2", "s3"))),
    ((0.45,), ((0.28, 0.44), (0.62, 0.38)),
     (("s0", "e0", "s1"), ("e0", "e1", "s1"), ("s1", "e1", "s2"))),
    ((), ((0.42, 0.5),),
     (("s0", "e0", "s1"),)),
)


def leaf_card(mesh, base, outward, up_hint, length_, width, color_base, color_tip, fold=0.0,
              detail=2, curl=0.0):
    """An ovate leaf from base to tip along `outward`. The midrib is raised by
    `fold` (a share of the width) so the halves form a shallow V, and the leaf
    curls back along its normal by `curl` (a share of its length) toward the
    tip. u runs across the leaf (0 left margin, 0.5 midrib, 1 right margin)."""
    side = perpendicular(outward, cross(up_hint, outward) if abs(
        sum(a * b for a, b in zip(up_hint, outward))) < 0.98 else (1.0, 0.0, 0.0))
    normal = norm(cross(side, outward))
    midrib, margin, half_triangles = LEAF_SHAPES[detail]

    def point(t, across, raise_):
        return add(add(base, mul(outward, t * length_)),
                   add(mul(side, across * width), mul(normal, raise_ * width - curl * length_ * t * t)))

    spine_t = (0.0,) + tuple(midrib) + (1.0,)
    spine = [mesh.vertex(point(t, 0.0, fold if 0.0 < t < 1.0 else 0.0),
                         mix(color_base, color_tip, t), 0.5) for t in spine_t]
    for sign, u in ((-1.0, 0.0), (1.0, 1.0)):
        edge = [mesh.vertex(point(t, sign * half, 0.0), mix(color_base, color_tip, t), u)
                for t, half in margin]
        pick = lambda name: (spine if name[0] == "s" else edge)[int(name[1:])]
        for x, y, z in half_triangles:
            # The right half mirrors the left, so its winding flips.
            mesh.triangle(pick(x), pick(y), pick(z)) if sign < 0 else                 mesh.triangle(pick(x), pick(z), pick(y))


TWIG = (0.20, 0.15, 0.09)
TWIG_DARK = (0.10, 0.08, 0.05)
SHRUB_LEAF_SHADE = (0.025, 0.075, 0.02)
SHRUB_LEAF_SUN = (0.065, 0.14, 0.04)
# Coarse LODs keep fewer, larger leaves; coverage (keep x grow^2) stays ~0.9.
# Few, very large leaves (20 % at 2.15x) read as big squares from 13 m on.
LEAF_KEEP = (1.0, 0.6, 0.35)
LEAF_GROW = (1.0, 1.25, 1.6)
# Midrib fold and back-curl of leaf cards (shares of width / length).
LEAF_FOLD = 0.12
LEAF_CURL = 0.18


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


CORE_SHADE = (0.012, 0.03, 0.01)
CORE_SUN = (0.025, 0.07, 0.018)
CORE_SUBDIVIDE = (2, 1, 1)


def crown_core(mesh, center, rx, ry, rz, detail, lump_seed):
    """A closed, lumpy ellipsoid of dark foliage colour inside a crown lobe.

    Leaf cards alone leave gaps that show the ground (and coarse LODs drop
    most leaves); the core makes the bush read as one solid mass from every
    angle. Vertices below the ground are lifted to it, flattening the base.
    """
    vertices = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]
    faces = [(0, 4, 2), (4, 1, 2), (1, 5, 2), (5, 0, 2), (4, 0, 3), (1, 4, 3), (5, 1, 3), (0, 5, 3)]
    for _ in range(CORE_SUBDIVIDE[detail]):
        cache = {}

        def midpoint(i, j):
            key = (min(i, j), max(i, j))
            if key not in cache:
                vertices.append(norm(mul(add(vertices[i], vertices[j]), 0.5)))
                cache[key] = len(vertices) - 1
            return cache[key]
        refined = []
        for a, b, c in faces:
            ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
            refined += [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
        faces = refined
    indices = []
    for v in vertices:
        # Smooth lumps from the direction, identical at every LOD.
        lump = 1.0 + 0.12 * math.sin(v[0] * 5.3 + lump_seed) * math.sin(v[2] * 4.7 + lump_seed * 1.7) \
            + 0.06 * math.sin(v[1] * 6.1 + lump_seed * 2.3)
        p = (center[0] + v[0] * rx * lump, max(0.02, center[1] + v[1] * ry * lump),
             center[2] + v[2] * rz * lump)
        indices.append(mesh.vertex(p, mix(CORE_SHADE, CORE_SUN, 0.5 + 0.5 * v[1]), 0.5))
    for a, b, c in faces:
        mesh.triangle(indices[a], indices[c], indices[b])


def bush_leaves(mesh, rng, count, lobes, detail, size, shade=SHRUB_LEAF_SHADE, sun=SHRUB_LEAF_SUN):
    """Leaf cards on the surfaces of ellipsoidal crown lobes (center, rx, ry,
    rz), facing outward and a little up. Draws the same random values at
    every LOD so the silhouette holds while coarse LODs keep fewer, larger
    leaves."""
    for _ in range(count):
        lobe = lobes[int(rng.random() * len(lobes))]
        u, v, keep, scale, tint, spin = (rng.random() for _ in range(6))
        if keep >= LEAF_KEEP[detail]:
            continue
        center, rx, ry, rz = lobe
        azimuth = u * 2.0 * math.pi
        elevation = math.asin(-0.35 + 1.35 * v)
        normal = (math.cos(elevation) * math.cos(azimuth), math.sin(elevation),
                  math.cos(elevation) * math.sin(azimuth))
        shell = 0.92 + 0.16 * scale
        base = (center[0] + normal[0] * rx * shell, max(0.03, center[1] + normal[1] * ry * shell),
                center[2] + normal[2] * rz * shell)
        outward = norm(add(normal, (math.cos(spin * 6.283) * 0.5, 0.3, math.sin(spin * 6.283) * 0.5)))
        light = 0.3 + 0.7 * (0.5 + 0.5 * normal[1])
        base_color = mix(shade, sun, light * 0.65)
        tip_color = mix(shade, sun, min(1.0, light * (0.85 + 0.3 * tint)))
        grow = LEAF_GROW[detail]
        leaf_card(mesh, base, outward, UP, size * 1.4 * grow * (0.8 + 0.4 * scale),
                  size * grow * (0.8 + 0.4 * scale), base_color, tip_color, fold=LEAF_FOLD,
                  detail=detail, curl=LEAF_CURL)


BARK = (0.15, 0.115, 0.075)
BARK_DARK = (0.07, 0.055, 0.04)
# Branch levels drawn per LOD (0 main stems .. 3 twigs); leaves always sit at
# the twig tips, so coarse LODs keep the silhouette while dropping fine wood.
BRANCH_DRAWN = (3, 2, 1)
BRANCH_SIDES = (5, 4, 3, 3)
BRANCH_SEGMENTS = (3, 3, 2, 1)
# No core at LOD0: up close its smooth shell reads as an egg inside the bush,
# and the full leaf count hides the interior anyway.
CORE_SCALE = (0.0, 0.45, 0.65)
# Share of the way to the envelope each level's branch grows; twigs reach it.
BRANCH_REACH = (0.55, 0.6, 0.75, 1.0)


def grow_bush(mesh, rng, detail, envelope, stems, stem_width, leaf_size, leaves_per_twig,
              shade, sun, upright=0.0):
    """A bush grown as a branch hierarchy inside an ellipsoid envelope:
    `stems` main stems from the root, each splitting into three branches,
    then three twigs (four levels). Branches taper, curve outward and up and
    are kept inside the envelope; each twig tip carries a cluster of small
    leaves on short stalks. Every random value is drawn at every LOD, so all
    LODs share one layout; coarse LODs skip fine wood and keep fewer, larger
    leaves. A dark inner core (the shadowed mass of twigs) keeps the bush
    opaque."""
    center, rx, ry, rz = envelope

    def inside(p):
        return ((p[0] - center[0]) / rx) ** 2 + ((p[1] - center[1]) / ry) ** 2 + ((p[2] - center[2]) / rz) ** 2

    def to_envelope(base, direction):
        """Distance from base along direction to the envelope surface."""
        o = ((base[0] - center[0]) / rx, (base[1] - center[1]) / ry, (base[2] - center[2]) / rz)
        d = (direction[0] / rx, direction[1] / ry, direction[2] / rz)
        a = dot(d, d); b = 2.0 * dot(o, d); c = dot(o, o) - 1.0
        disc = b * b - 4.0 * a * c
        if disc <= 0.0:
            return 0.05
        return max(0.05, (-b + math.sqrt(disc)) / (2.0 * a))

    def outward(p):
        return norm(((p[0] - center[0]) / rx + 1e-4, (p[1] - center[1]) / ry, (p[2] - center[2]) / rz + 1e-4))

    tips = []

    def grow(base, direction, length, width, level):
        sway = (rng.random() - 0.5, rng.random() - 0.5)
        children = [(0.45 + 0.5 * rng.random(), rng.random() * 6.283, rng.random(), 0.55 + 0.25 * rng.random())
                    for _ in range(3 if level < 3 else 0)]
        # Each level grows a share of the way to the envelope; twigs reach it.
        length = min(length, to_envelope(base, direction) * BRANCH_REACH[level])
        segments = BRANCH_SEGMENTS[level]
        points, tangents = [], []
        heading = direction
        for i in range(segments + 1):
            points.append(add(base, mul(heading, length * i / segments)) if i == 0 else
                          add(points[-1], mul(heading, length / segments)))
            tangents.append(heading)
            # Curve gently upward and sideways.
            heading = norm(add(heading, (sway[0] * 0.2, 0.05, sway[1] * 0.2)))
        if level <= BRANCH_DRAWN[detail]:
            tube(mesh, points, tangents,
                 [width * (1.0 - 0.55 * i / segments) for i in range(segments + 1)],
                 [mix(BARK_DARK, BARK, (level + i / segments) / 4.0) for i in range(segments + 1)],
                 sides=BRANCH_SIDES[level])
        if level == 3:
            tips.append((points[-1], heading))
            # A few leaves along the twig as well as at its tip.
            tips.append((points[0], heading))
            return
        for along, azimuth, lift, scale in children:
            index = min(segments, int(round(along * segments)))
            at = points[index]
            # Mostly the parent's way, pushed outward from the trunk axis
            # (outward from the centre would point low branches downward),
            # with a random spread and a slight upward lean.
            axis_out = norm((at[0] - center[0] + 1e-3, 0.0, at[2] - center[2] + 1e-3))
            spread = (math.cos(azimuth), lift - 0.2, math.sin(azimuth))
            child = norm(add(add(mul(heading, 0.55), mul(axis_out, 0.45)),
                             add(mul(norm(spread), 0.45), (0.0, 0.25 + upright * 0.4, 0.0))))
            grow(at, child, 10.0, width * 0.6, level + 1)

    height = center[1] + ry
    for stem in range(stems):
        angle = stem * 6.283 / stems + rng.random() * 0.8
        tilt = 0.25 + 0.35 * rng.random()
        direction = norm((math.cos(angle) * (tilt + 0.3), 1.0 + upright, math.sin(angle) * (tilt + 0.3)))
        grow((0.03 * math.cos(angle), 0.0, 0.03 * math.sin(angle)), direction,
             height * (0.5 + 0.2 * rng.random()), stem_width, 0)

    # The core sits inside the foliage: centred on the twig tips and sized
    # from their spread, so it never pokes out where branches did not reach.
    count = float(len(tips))
    mid = tuple(sum(tip[0][axis] for tip in tips) / count for axis in range(3))
    spread = tuple(math.sqrt(sum((tip[0][axis] - mid[axis]) ** 2 for tip in tips) / count) for axis in range(3))
    k = CORE_SCALE[detail]
    if k > 0.0:
        crown_core(mesh, mid, spread[0] * 1.5 * k, spread[1] * 1.5 * k, spread[2] * 1.5 * k, detail, 1.7)

    grow_size = LEAF_GROW[detail]
    for tip, heading in tips:
        cluster = [tuple(rng.random() for _ in range(7)) for _ in range(leaves_per_twig)]
        normal = norm(((tip[0] - center[0]) / rx, (tip[1] - center[1]) / ry, (tip[2] - center[2]) / rz))
        light = 0.3 + 0.7 * (0.5 + 0.5 * normal[1])
        for keep, azimuth, rise, scale, tint, roll, shade_jitter in cluster:
            if keep >= LEAF_KEEP[detail]:
                continue
            # Leaf on a short stalk around the twig tip, facing out and up.
            stalk = norm(add(mul(heading, 0.6), (math.cos(azimuth * 6.283), 0.2 + 0.6 * rise,
                                                 math.sin(azimuth * 6.283))))
            base = add(tip, mul(stalk, leaf_size * 0.35))
            outward = norm(add(stalk, mul(normal, 0.5)))
            size = leaf_size * grow_size * (0.8 + 0.4 * scale)
            # Leaves turn about their own axis at random; with one up hint for
            # all of them they lie in parallel tiers.
            up_hint = norm(add(UP, (math.cos(roll * 6.283) * 1.2, 0.0, math.sin(roll * 6.283) * 1.2)))
            # Per-leaf value jitter (+-25 %) and an occasional yellowing leaf.
            value = 0.75 + 0.5 * shade_jitter
            leaf_sun = mix(sun, (0.2, 0.22, 0.05), 0.5) if tint > 0.94 else sun
            leaf_card(mesh, base, outward, up_hint, size * 1.5, size,
                      mul(mix(shade, leaf_sun, light * 0.6), value),
                      mul(mix(shade, leaf_sun, min(1.0, light * (0.85 + 0.3 * tint))), value),
                      fold=LEAF_FOLD, detail=detail, curl=LEAF_CURL)


def meadow_bush(detail):
    """Rounded broadleaf bush (hazel / dogwood-like), ~1.2 m tall, ~1.5 m wide,
    grown as stems, branches and twigs with leaf clusters."""
    mesh = Mesh(wind_response=0.25)
    grow_bush(mesh, random.Random(6401), detail, ((0.0, 0.62, 0.0), 0.78, 0.6, 0.72),
              stems=7, stem_width=0.028, leaf_size=0.08, leaves_per_twig=8,
              shade=SHRUB_LEAF_SHADE, sun=SHRUB_LEAF_SUN)
    return mesh


def meadow_bush_tall(detail):
    """Upright oval bush (privet / young holly-like), ~1.7 m tall."""
    mesh = Mesh(wind_response=0.2)
    grow_bush(mesh, random.Random(6501), detail, ((0.0, 0.9, 0.0), 0.5, 0.85, 0.48),
              stems=6, stem_width=0.032, leaf_size=0.065, leaves_per_twig=8,
              shade=(0.02, 0.06, 0.02), sun=(0.07, 0.19, 0.045), upright=0.6)
    return mesh


def tall_grass_clump(detail):
    """Dense tall grass clump (~1.1 m) with seed stalks: tall cover that hides
    the ground at eye level."""
    rng = random.Random(6601)
    mesh = Mesh()
    layout = [(rng.uniform(-0.2, 0.2), rng.uniform(-0.2, 0.2), rng.random()) for _ in range(3)]
    for index, (x, z, size) in enumerate(layout):
        tussock(mesh, random.Random(6700 + index), (x, 0.0, z), 34 + int(10 * size), 0.95 + 0.25 * size,
                0.09 + 0.04 * size, detail, dry_share=0.25, width=0.016)
    stalks = random.Random(6800)
    for base in accents.scatter(stalks, 8, 0.25):
        yaw, height, lean, keep = stalks.random() * 6.283, 1.05 + 0.25 * stalks.random(), \
            0.06 + 0.1 * stalks.random(), stalks.random()
        if keep < (1.0, 0.7, 0.45)[detail]:
            seed_stalk(mesh, base, yaw, height, lean, detail)
    sward(mesh, random.Random(6900), 30, 0.45, detail)
    return mesh


# Tall meadow grass (the reference meadows, `.local/reference-images/
# chlorophyll-video/png`: knee- to waist-high fine blades as one mass that
# hides the ground). The mass reads by its two tones: the depth between blades
# is deep, shaded green and the sunlit tips are bright yellow-green, so roots
# here are far darker than the short grass's. Blades are flat (two columns)
# to keep the triangle count down at a density where patches overlap; coarser
# levels keep fewer, wider blades.
TALL_KEEP = (1.0, 0.6, 0.4)
TALL_WIDEN = (1.0, 1.6, 3.0)
TALL_SEGMENTS = (3, 3, 2)
TALL_ROOT = (0.016, 0.036, 0.011)
TALL_MID = (0.05, 0.10, 0.026)
TALL_TIP = (0.16, 0.22, 0.07)
TALL_SEED = (0.20, 0.22, 0.10)


def fine_blades(mesh, rng, center, blades, height, crown, detail, lean_yaw, width=0.011,
                dry_share=0.12):
    """Long fine blades from one crown, mostly upright with arching tips,
    leaning a little with the patch (lean_yaw) as wind-laid grass does."""
    for _ in range(blades):
        radius, angle = math.sqrt(rng.random()), rng.random() * 2.0 * math.pi
        keep, length_jitter, rise_jitter = rng.random(), rng.random(), rng.random()
        droop, dry, twist = 0.08 + 0.22 * rng.random(), rng.random(), (rng.random() - 0.5) * 1.4
        yaw = angle + (rng.random() - 0.5) * 0.9
        yaw = math.atan2(math.sin(yaw) + 0.6 * math.sin(lean_yaw), math.cos(yaw) + 0.6 * math.cos(lean_yaw))
        if keep >= TALL_KEEP[detail]:
            continue
        base = add(center, (crown * radius * math.cos(angle), 0.0, crown * radius * math.sin(angle)))
        rise = math.radians(86.0 - 24.0 * radius ** 1.5 - 10.0 * rise_jitter)
        length_ = height * (0.6 + 0.45 * length_jitter) * (1.05 - 0.12 * radius)
        tip = DRY_TIP if dry < dry_share else TALL_TIP
        shade = 0.8 + 0.4 * ((length_jitter * 7.31 + rise_jitter * 3.17) % 1.0)
        blade(mesh, base, yaw, length_, rise, droop, width * TALL_WIDEN[detail],
              TALL_SEGMENTS[detail], root=TALL_ROOT, mid=TALL_MID, tip=tip, twist=twist,
              columns=2, shade=shade)


def wispy_seed_stem(mesh, base, yaw, height, lean, detail):
    """A thin culm with a small, dull seed head: wisps above the mass, not
    the pale panicles that read as leaves floating over the grass."""
    top, axis = accents.stem(mesh, base, yaw, height, lean, 0.002, (3, 2, 2)[detail],
                             root=TALL_MID, top=TALL_SEED)
    spike = [add(top, mul(axis, 0.05 * i / 2)) for i in range(3)]
    side = perpendicular(axis, horizontal(yaw + math.pi * 0.5))
    mesh.ribbon(spike, [side] * 3, [0.004, 0.005, 0.002], [TALL_SEED, TALL_SEED, TALL_SEED])


def meadow_tall_grass(detail):
    """A metre-wide patch of tall meadow grass, ~0.7-1.05 m: seven crowns of
    fine blades leaning one way, with a few wispy seed stems above them.
    Placed at over one per square metre (GrassField tall fields), patches
    overlap into a continuous mass that hides the ground except from steeply
    above."""
    rng = random.Random(8101)
    # Half the wind response: the bend grows with blade length, and at full
    # response the wind laid these blades flat, all one way.
    mesh = Mesh(wind_response=0.5)
    lean_yaw = rng.random() * 2.0 * math.pi
    layout = [(rng.uniform(-0.45, 0.45), rng.uniform(-0.45, 0.45), rng.random()) for _ in range(7)]
    for index, (x, z, size) in enumerate(layout):
        fine_blades(mesh, random.Random(8200 + index), (x, 0.0, z), 44 + int(12 * size),
                    0.7 + 0.35 * size, 0.08 + 0.05 * size, detail, lean_yaw)
    stalks = random.Random(8300)
    for base in accents.scatter(stalks, 4, 0.4):
        yaw, height, lean, keep = (stalks.random() * 6.283, 0.95 + 0.25 * stalks.random(),
                                   0.05 + 0.12 * stalks.random(), stalks.random())
        if keep < (1.0, 0.6, 0.35)[detail]:
            wispy_seed_stem(mesh, base, yaw, height, lean, detail)
    return mesh


def far_grass_tuft(detail):
    """Cheap clump for the far grass ring (seen from ~30 m to 200 m): eighteen
    wide, two-segment blades fanning from a broad crown, ~0.55-0.8 m tall and
    ~1 m across, in the tall grass's colours (dark roots, bright tips), so the
    ring carries on the tall mass instead of flattening into a lawn. Lower
    levels keep fewer, wider, single-segment blades (Ghost of Tsushima's rule:
    fewer and wider blades with distance), so a tuft 100 m away is a few
    triangles instead of 54: the ring was half of all grass vertex work
    (FieldWalk, 2026-11-22)."""
    rng = random.Random(7101)
    mesh = Mesh(wind_response=0.5)
    keep_share = FAR_TUFT_KEEP[detail]
    for _ in range(18):
        radius, angle = math.sqrt(rng.random()), rng.random() * 2.0 * math.pi
        length_jitter, rise_jitter, dry = rng.random(), rng.random(), rng.random()
        yaw_jitter, keep = rng.random(), (length_jitter * 13.7 + rise_jitter * 5.3) % 1.0
        if keep >= keep_share:
            continue
        base = (0.42 * radius * math.cos(angle), 0.0, 0.42 * radius * math.sin(angle))
        rise = math.radians(82.0 - 26.0 * radius - 10.0 * rise_jitter)
        shade = 0.85 + 0.3 * ((length_jitter * 7.31 + rise_jitter * 3.17) % 1.0)
        blade(mesh, base, angle + (yaw_jitter - 0.5) * 0.8, 0.55 + 0.25 * length_jitter, rise,
              0.2, 0.08 * FAR_TUFT_WIDEN[detail], FAR_TUFT_SEGMENTS[detail], root=TALL_ROOT,
              mid=TALL_MID, tip=DRY_TIP if dry < 0.12 else TALL_TIP, shade=shade)
    return mesh


FAR_TUFT_KEEP = (1.0, 0.45, 0.22)
FAR_TUFT_WIDEN = (1.0, 1.9, 3.4)
FAR_TUFT_SEGMENTS = (2, 1, 1)


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
    # A solid core under the leaves so the mound hides the ground (drawn
    # before the leaves; consumes no random values, so the layout is kept).
    for index, (center, lobe_radius, lobe_height) in enumerate(lobes):
        crown_core(mesh, add(center, (0.0, lobe_height * 0.35, 0.0)), lobe_radius * 0.8,
                   lobe_height * 0.55, lobe_radius * 0.8, detail, 0.7 + index * 0.9)
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
                  0.078 * grow * (0.8 + 0.4 * size), base_color, tip_color,
                  fold=LEAF_FOLD, detail=detail, curl=LEAF_CURL)
    return mesh


BRAMBLE_CANE = (0.26, 0.11, 0.08)
BRAMBLE_CANE_TIP = (0.22, 0.20, 0.08)
BRAMBLE_LEAF_BASE = (0.03, 0.085, 0.025)
BRAMBLE_LEAF_TIP = (0.06, 0.13, 0.035)


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
                          0.065 * scale * grow * (0.8 + 0.4 * size), BRAMBLE_LEAF_BASE, BRAMBLE_LEAF_TIP,
                          fold=LEAF_FOLD, detail=detail, curl=LEAF_CURL)
        if flower < 0.5 and detail < 2:
            top = points[segments // 2]
            accents.petal_ring(mesh, add(top, (0.0, 0.012, 0.0)), UP, 5, 0.003, 0.010, 0.009, 0.3,
                               (0.80, 0.74, 0.72), (0.92, 0.86, 0.86), 1, flower * 12.0)
            accents.disc(mesh, add(top, (0.0, 0.014, 0.0)), UP, 0.004, 0.002, 5,
                         (0.70, 0.62, 0.20), (0.60, 0.55, 0.20))
    return mesh


FERN_RACHIS = (0.07, 0.12, 0.04)
FERN_PINNA_BASE = (0.035, 0.085, 0.025)
FERN_PINNA_TIP = (0.09, 0.17, 0.045)


def meadow_fern(detail):
    """Bracken: arching fronds with paired pinnae."""
    return fern(detail)


def forest_fern(detail):
    """Woodland fern (~1 m): more, longer fronds in the darker green of
    shaded ferns."""
    return fern(detail, seed=6351, fronds=10, yaw_step=0.628 * 1.07, scale=1.45,
                rachis=(0.07, 0.15, 0.04), base=(0.025, 0.085, 0.022), tip=(0.07, 0.19, 0.05))


def fern(detail, seed=6301, fronds=7, yaw_step=0.9, scale=1.0,
         rachis=FERN_RACHIS, base=FERN_PINNA_BASE, tip=FERN_PINNA_TIP):
    """Arching fronds with paired pinnae. Defaults are meadow bracken."""
    rng = random.Random(seed)
    mesh = Mesh(wind_response=0.85)
    for k in range(fronds):
        yaw = k * yaw_step + rng.random() * 0.6
        length_ = (0.62 + 0.25 * rng.random()) * scale
        rise, droop = math.radians(52 + 22 * rng.random()), 0.5 + 0.4 * rng.random()
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
                    [rachis] * len(points))
        if detail == 2:
            # One broad tapering blade stands in for the pinnae.
            normals = [norm(cross(side, t)) for t in tangents]
            widths = [0.24 * scale * math.sin(math.pi * min(1.0, (0.15 + i / segments) * 0.9))
                      for i in range(segments + 1)]
            mesh.ribbon([add(p, mul(n, 0.004)) for p, n in zip(points, normals)], [side] * len(points),
                        widths, [mix(base, tip, i / segments) for i in range(segments + 1)],
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
            span = 0.19 * scale * math.sin(math.pi * min(1.0, 0.25 + 0.85 * (1.0 - t))) *                 (1.0 if detail == 0 else 1.15)
            for sign in (-1.0, 1.0):
                out = norm(add(mul(side, sign), mul(tangent, 0.35)))
                out = norm(add(out, (0.0, -0.18, 0.0)))
                pinna = [add(at, mul(out, span * i / 2)) for i in range(3)]
                across = perpendicular(out, tangent)
                mesh.ribbon(pinna, [across] * 3, [scale * 0.055 * (1.0 if detail == 0 else 1.6),
                                                  scale * 0.045 * (1.0 if detail == 0 else 1.6), 0.0],
                            [base, mix(base, tip, 0.6), tip])
    return mesh


# --- forest floor (.md/todo.md Foliage Phase 4) ----------------------------------
# Plants for the forest-soil paint under trees: fallen-leaf litter, broadleaf
# herbs, oak seedlings and dead grass. A GrassField on the forest floor layer
# (Scripts/Grass/MeadowGrass ApplyForestFloor) scatters them.

LITTER_COLORS = ((0.16, 0.08, 0.025), (0.26, 0.14, 0.04), (0.33, 0.24, 0.07),
                 (0.08, 0.05, 0.02), (0.20, 0.17, 0.06), (0.12, 0.10, 0.04))
HERB_SHADE = (0.022, 0.07, 0.02)
HERB_SUN = (0.055, 0.125, 0.038)
HERB_STALK = (0.06, 0.12, 0.03)
LITTER_KEEP = (1.0, 0.55, 0.3)
LITTER_GROW = (1.0, 1.35, 1.8)


def forest_litter(detail):
    """A patch of fallen leaves (~0.9 m) lying on the soil, with a twig or two.
    Static: no wind response."""
    rng = random.Random(7101)
    mesh = Mesh(wind_response=0.0)
    for _ in range(46):
        radius, angle = math.sqrt(rng.random()) * 0.45, rng.random() * 2.0 * math.pi
        keep, yaw, tilt, size, shade, kind = (rng.random() for _ in range(6))
        if keep >= LITTER_KEEP[detail]:
            continue
        base = (radius * math.cos(angle), 0.004 + 0.012 * shade, radius * math.sin(angle))
        # Lying flat, a few propped up on their neighbours.
        outward = norm(add(horizontal(yaw * 6.283), (0.0, 0.25 * tilt * tilt, 0.0)))
        color = LITTER_COLORS[int(kind * len(LITTER_COLORS)) % len(LITTER_COLORS)]
        value = 0.8 + 0.4 * shade
        length_ = (0.06 + 0.05 * size) * LITTER_GROW[detail]
        leaf_card(mesh, base, outward, UP, length_, length_ * 0.62, mul(color, value * 0.85),
                  mul(color, value), fold=0.06, detail=detail, curl=-0.12)
    if detail < 2:
        for k in range(2):
            yaw = rng.random() * 6.283
            start = (rng.uniform(-0.3, 0.3), 0.008, rng.uniform(-0.3, 0.3))
            branch(mesh, start, add(start, mul(horizontal(yaw), 0.25 + 0.2 * rng.random())), 0.008, 2)
    return mesh


def forest_herb(detail):
    """Broadleaf woodland herb (~0.45 m): a rosette of broad ovate leaves on
    long stalks, the glossy undergrowth of the RDR2 forest floor."""
    rng = random.Random(7201)
    mesh = Mesh(wind_response=0.7)
    count = 9
    for k in range(count):
        keep, reach, rise, size, tint, roll = (rng.random() for _ in range(6))
        if keep >= (1.0, 0.75, 0.55)[detail]:
            continue
        yaw = k * 2.39996 + rng.random() * 0.4  # golden angle
        direction = horizontal(yaw)
        # Petiole: up and out, longer for the outer leaves.
        stalk = (0.14 + 0.2 * reach)
        lift = math.radians(48 + 28 * rise)
        tip = add(mul(direction, stalk * math.cos(lift)), (0.0, stalk * math.sin(lift), 0.0))
        if detail < 2:
            branch(mesh, (0.0, 0.0, 0.0), tip, 0.006, 2, color_base=HERB_STALK, color_tip=HERB_STALK)
        # Blade: outward and gently down, broad (heart-ish via the wide ovate).
        outward = norm(add(direction, (0.0, 0.15 - 0.35 * rise, 0.0)))
        grow = (1.0, 1.1, 1.25)[detail]
        length_ = (0.17 + 0.1 * size) * grow
        up_hint = norm(add(UP, (math.cos(roll * 6.283) * 0.3, 0.0, math.sin(roll * 6.283) * 0.3)))
        light = 0.55 + 0.45 * rise
        value = 0.85 + 0.3 * tint
        leaf_card(mesh, tip, outward, up_hint, length_, length_ * 0.78,
                  mul(mix(HERB_SHADE, HERB_SUN, light * 0.5), value),
                  mul(mix(HERB_SHADE, HERB_SUN, light), value),
                  fold=0.14, detail=detail, curl=0.22)
    return mesh


def forest_seedling(detail):
    """Oak seedling (~0.45 m): a thin stem with alternate leaves."""
    rng = random.Random(7301)
    mesh = Mesh(wind_response=0.5)
    height = 0.42
    segments = (4, 3, 2)[detail]
    points = [(0.012 * math.sin(i * 1.7), height * i / segments, 0.012 * math.cos(i * 1.3))
              for i in range(segments + 1)]
    tangents = [norm(sub(points[min(i + 1, segments)], points[max(i - 1, 0)])) for i in range(segments + 1)]
    if detail < 2:
        tube(mesh, points, tangents, [0.006 * (1 - 0.6 * i / segments) for i in range(segments + 1)],
             [mix(BARK_DARK, BARK, i / segments) for i in range(segments + 1)], sides=3)
    else:
        branch(mesh, points[0], points[-1], 0.008, 2)
    for k in range(8):
        keep, size, tint = rng.random(), rng.random(), rng.random()
        if keep >= (1.0, 0.7, 0.45)[detail]:
            continue
        t = 0.3 + 0.7 * k / 7
        at = (points[0][0], height * t, points[0][2])
        yaw = k * 2.6 + rng.random() * 0.5
        outward = norm(add(horizontal(yaw), (0.0, 0.25, 0.0)))
        length_ = (0.07 + 0.04 * size) * (1.0, 1.15, 1.35)[detail]
        leaf_card(mesh, at, outward, UP, length_, length_ * 0.55, mul(SHRUB_LEAF_SHADE, 1.3),
                  mul(mix(SHRUB_LEAF_SUN, (0.12, 0.2, 0.05), tint), 1.0),
                  fold=LEAF_FOLD, detail=detail, curl=LEAF_CURL)
    return mesh


def dry_grass_clump(detail):
    """Dead and dry grass tussocks (~0.6 m) for clearings and forest edges."""
    mesh = Mesh()
    rng = random.Random(7401)
    for index in range(2):
        center = (rng.uniform(-0.12, 0.12), 0.0, rng.uniform(-0.12, 0.12))
        tussock(mesh, random.Random(7410 + index), center, 30, 0.5 + 0.15 * rng.random(), 0.08,
                detail, dry_share=0.85, width=0.014)
    return mesh


IVY_SHADE = (0.018, 0.05, 0.02)
IVY_SUN = (0.045, 0.105, 0.035)
IVY_STEM = (0.07, 0.06, 0.035)


def forest_branch(detail):
    """A fallen branch (~1.6 m) with side twigs, lying on the forest floor.
    Static: no wind response."""
    rng = random.Random(7501)
    mesh = Mesh(wind_response=0.0)
    segments = (6, 4, 2)[detail]
    length_ = 1.6
    points, tangents = [], []
    position, heading = (-0.8, 0.035, 0.0), norm((1.0, 0.02, 0.1))
    bends = [(rng.uniform(-0.25, 0.25), rng.uniform(-0.04, 0.05)) for _ in range(6)]
    for i in range(segments + 1):
        points.append(position)
        tangents.append(heading)
        bend = bends[min(5, i * 6 // (segments + 1))]
        heading = norm(add(heading, (0.0, bend[1], bend[0])))
        position = add(position, mul(heading, length_ / segments))
        position = (position[0], max(0.02, position[1]), position[2])
    tube(mesh, points, tangents, [0.028 * (1 - 0.65 * i / segments) for i in range(segments + 1)],
         [mix(BARK_DARK, BARK, 0.3 + 0.5 * i / segments) for i in range(segments + 1)],
         sides=(5, 4, 3)[detail])
    # Side twigs (fine ones only up close).
    for k in range(7):
        along, side_sign, rise, reach, keep = rng.random(), rng.random(), rng.random(), rng.random(), rng.random()
        if keep >= (1.0, 0.6, 0.0)[detail]:
            continue
        index = min(segments, int(round((0.25 + 0.7 * along) * segments)))
        at = points[index]
        side = norm(cross(tangents[index], UP))
        direction = norm(add(add(mul(tangents[index], 0.5), mul(side, 1.0 if side_sign < 0.5 else -1.0)),
                             (0.0, 0.15 + 0.5 * rise, 0.0)))
        branch(mesh, at, add(at, mul(direction, 0.22 + 0.3 * reach)), 0.012, 2)
    return mesh


def ground_ivy(detail):
    """A patch of ivy creeping over the forest floor (~1.1 m): runners along
    the ground with dark lobed leaves on short stalks."""
    rng = random.Random(7601)
    mesh = Mesh(wind_response=0.35)
    for runner in range(6):
        yaw = runner * 1.05 + rng.random() * 0.5
        length_ = 0.4 + 0.3 * rng.random()
        segments = (5, 3, 2)[detail]
        points = []
        position = (0.0, 0.012, 0.0)
        for i in range(segments + 1):
            points.append(position)
            yaw += (rng.random() - 0.5) * 0.9
            step = mul(horizontal(yaw), length_ / segments)
            position = (position[0] + step[0], 0.012 + 0.02 * rng.random(), position[2] + step[2])
        if detail < 2:
            side = [horizontal(yaw + math.pi * 0.5)] * len(points)
            mesh.ribbon(points, side, [0.006] * len(points), [IVY_STEM] * len(points))
        leaves = 9
        for k in range(leaves):
            along, turn, size, tint, keep = (rng.random() for _ in range(5))
            if keep >= (1.0, 0.6, 0.35)[detail]:
                continue
            t = along * segments
            low = min(segments - 1, int(t))
            at = add(mul(points[low], 1 - (t - low)), mul(points[low + 1], t - low))
            outward = norm(add(horizontal(turn * 6.283), (0.0, 0.35, 0.0)))
            base = add(at, (0.0, 0.02 + 0.03 * size, 0.0))
            length_leaf = (0.05 + 0.035 * size) * (1.0, 1.25, 1.6)[detail]
            value = 0.8 + 0.4 * tint
            leaf_card(mesh, base, outward, UP, length_leaf, length_leaf * 0.95,
                      mul(IVY_SHADE, value), mul(mix(IVY_SHADE, IVY_SUN, 0.4 + 0.6 * tint), value),
                      fold=0.08, detail=detail, curl=0.1)
    return mesh


PLANTS = {
    "meadow_tussock": (meadow_tussock, "four grass tussocks over a low sward"),
    "field_tussock": (field_tussock, "tall grass tussocks with seed stalks"),
    "meadow_shrub": (meadow_shrub, "low lumpy small-leaved shrub"),
    "meadow_bramble": (meadow_bramble, "arching bramble canes with leaves and flowers"),
    "meadow_fern": (meadow_fern, "bracken-like fern fronds"),
    "meadow_bush": (meadow_bush, "rounded broadleaf bush ~1.2 m with a solid crown core"),
    "meadow_bush_tall": (meadow_bush_tall, "upright oval bush ~1.7 m with a solid crown core"),
    "tall_grass_clump": (tall_grass_clump, "dense tall grass clump ~1.1 m with seed stalks"),
    "meadow_tall_grass": (meadow_tall_grass, "1 m patch of tall fine meadow grass ~0.6-1.0 m with seed stems"),
    "far_grass_tuft": (far_grass_tuft, "cheap wide-bladed tuft for the far grass ring"),
    "forest_litter": (forest_litter, "fallen-leaf litter patch for the forest floor"),
    "forest_herb": (forest_herb, "broadleaf woodland herb rosette ~0.45 m"),
    "forest_fern": (forest_fern, "woodland fern ~1 m, darker than bracken"),
    "forest_seedling": (forest_seedling, "oak seedling ~0.45 m"),
    "dry_grass_clump": (dry_grass_clump, "dead and dry grass tussocks ~0.6 m"),
    "forest_branch": (forest_branch, "fallen branch ~1.6 m with side twigs"),
    "ground_ivy": (ground_ivy, "ivy patch creeping over the forest floor ~1.1 m"),
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
