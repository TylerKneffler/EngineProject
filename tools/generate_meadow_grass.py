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

BLADE_ROOT = (0.045, 0.12, 0.025)
BLADE_MID = (0.13, 0.28, 0.055)
BLADE_TIP = (0.25, 0.40, 0.10)
DRY_TIP = (0.50, 0.44, 0.22)
SWARD_ROOT = (0.05, 0.13, 0.03)
SWARD_MID = (0.11, 0.25, 0.05)
SWARD_TIP = (0.20, 0.36, 0.08)


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
                  size * grow * (0.8 + 0.4 * scale), base_color, tip_color, fold=0.25 if detail == 0 else 0.0)


BARK = (0.15, 0.115, 0.075)
BARK_DARK = (0.07, 0.055, 0.04)
# Branch levels drawn per LOD (0 main stems .. 3 twigs); leaves always sit at
# the twig tips, so coarse LODs keep the silhouette while dropping fine wood.
BRANCH_DRAWN = (3, 2, 1)
BRANCH_SIDES = (5, 4, 3, 3)
BRANCH_SEGMENTS = (3, 3, 2, 1)
CORE_SCALE = (0.35, 0.6, 0.8)
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
    crown_core(mesh, mid, spread[0] * 1.5 * k, spread[1] * 1.5 * k, spread[2] * 1.5 * k, detail, 1.7)

    grow_size = LEAF_GROW[detail]
    for tip, heading in tips:
        cluster = [tuple(rng.random() for _ in range(5)) for _ in range(leaves_per_twig)]
        normal = norm(((tip[0] - center[0]) / rx, (tip[1] - center[1]) / ry, (tip[2] - center[2]) / rz))
        light = 0.3 + 0.7 * (0.5 + 0.5 * normal[1])
        for keep, azimuth, rise, scale, tint in cluster:
            if keep >= LEAF_KEEP[detail]:
                continue
            # Leaf on a short stalk around the twig tip, facing out and up.
            stalk = norm(add(mul(heading, 0.6), (math.cos(azimuth * 6.283), 0.2 + 0.6 * rise,
                                                 math.sin(azimuth * 6.283))))
            base = add(tip, mul(stalk, leaf_size * 0.35))
            outward = norm(add(stalk, mul(normal, 0.5)))
            size = leaf_size * grow_size * (0.8 + 0.4 * scale)
            leaf_card(mesh, base, outward, UP, size * 1.5, size,
                      mix(shade, sun, light * 0.6), mix(shade, sun, min(1.0, light * (0.85 + 0.3 * tint))))


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


def far_grass_tuft(detail):
    """Cheap tuft for the far grass ring (seen from ~25 m out): a dozen wide,
    two-segment blades fanning from a broad crown, ~0.4 m tall. Every LOD is
    the same mesh; at that range a blade covers a pixel or two."""
    rng = random.Random(7101)
    mesh = Mesh()
    for _ in range(12):
        radius, angle = math.sqrt(rng.random()), rng.random() * 2.0 * math.pi
        length_jitter, rise_jitter, dry = rng.random(), rng.random(), rng.random()
        base = (0.2 * radius * math.cos(angle), 0.0, 0.2 * radius * math.sin(angle))
        rise = math.radians(78.0 - 30.0 * radius - 10.0 * rise_jitter)
        shade = 0.88 + 0.24 * ((length_jitter * 7.31 + rise_jitter * 3.17) % 1.0)
        blade(mesh, base, angle + (rng.random() - 0.5) * 0.8, 0.32 + 0.16 * length_jitter, rise,
              0.25, 0.045, 2, tip=DRY_TIP if dry < 0.15 else BLADE_TIP, shade=shade)
    return mesh


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
    "meadow_bush": (meadow_bush, "rounded broadleaf bush ~1.2 m with a solid crown core"),
    "meadow_bush_tall": (meadow_bush_tall, "upright oval bush ~1.7 m with a solid crown core"),
    "tall_grass_clump": (tall_grass_clump, "dense tall grass clump ~1.1 m with seed stalks"),
    "far_grass_tuft": (far_grass_tuft, "cheap wide-bladed tuft for the far grass ring"),
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
