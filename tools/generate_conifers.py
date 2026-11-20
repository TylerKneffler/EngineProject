#!/usr/bin/env python3
"""Generate conifers for the landscape: trunk + layered branch whorls with
needle cards, four species/size variants, each with an LOD chain and a
view-baked impostor.

Writes (under Engine/Core/Assets):
  Models/Woodland/Conifers/Meshes/<Tree>_LOD{0,1,2}_{Trunk,Needles}.mesh
  Models/Woodland/Conifers/Meshes/<Tree>_Impostor.mesh
  Models/Woodland/Conifers/Textures/Conifer_Needles_Color.ktx2   (shared atlas)
  Models/Woodland/Conifers/Textures/Conifer_Bark_{Color,Normal}.ktx2
  Models/Woodland/Conifers/Textures/<Tree>_Impostor_{Color,Normal}.ktx2
  Models/Woodland/Conifers/Materials/*.material
  Prefabs/Woodland/Conifers/<Tree>.prefab

Levels (prefab Mesh.lodMeshes, like the oak pack):
  LOD0     full whorls: bark branches, 3-6 needle sprays per branch, dead
           twigs under the crown; for the near band (< ~60 m);
  LOD1     the same whorls, two or three larger sprays per branch and no
           branch wood: the mid band (60-220 m). Card area is kept close to
           LOD0's so the crown neither thins nor darkens at the switch;
  LOD2     per-whorl star of big clump cards (non-instanced use);
  Impostor six baked views + top view, used beyond the mid band and by
           TerrainManagerFarObjects for distant forests.
Instanced terrain layers keep three bands (LOD0, LOD1, last level), so the
instanced chain is LOD0 -> LOD1 -> impostor.

Colour: the needle atlas holds neutral detail, species colour comes from
vertex colours (linear). Palettes are muted, measured-style conifer albedos
(dark blue-green spruce and fir, grey-yellow pine) with per-branch and
per-tree variation, darker toward the trunk and lower crown, lighter new
growth at the tips.

Every tree draws its layout (whorls, branches) from a fixed seed before any
detail decision, so all LODs and the impostor agree.

Usage: python Tools/generate_conifers.py [--no-impostors] [--only Name]
"""
from __future__ import annotations

import argparse
import math
import random
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import woodland_assets as wa  # noqa: E402

MODELS = wa.ASSETS / "Models" / "Woodland" / "Conifers"
MESHES = MODELS / "Meshes"
TEXTURES = MODELS / "Textures"
MATERIALS = MODELS / "Materials"
PREFABS = wa.ASSETS / "Prefabs" / "Woodland" / "Conifers"
SOURCES = wa.WORK / "conifers"

NEEDLE_CUTOFF = 0.42
IMPOSTOR_CUTOFF = 0.4
# Material.foliageColorVariation: crown/stand hue and value spread (Object.hlsl).
FOLIAGE_VARIATION = 0.55
# Material.foliageTranslucency: sunlight through backlit needles, plus crown
# normals/occlusion baked by tools/cook_foliage_lighting.py (run from main()).
NEEDLE_TRANSLUCENCY = 0.8
# Mesh.foliageWindStrength with Mesh.foliageWindTree (trunk sway, branch sway,
# needle flutter; Shaders/GrassWind.hlsli ApplyTreeWind).
TREE_WIND = 0.1
UP = np.array([0.0, 1.0, 0.0])

# Needle atlas cells (2 x 2 of 512 px): spray kinds.
CELL_SPRUCE, CELL_FIR, CELL_PINE, CELL_CLUMP = 0, 1, 2, 3


# --- palettes (linear vertex colours; the atlas is ~0.55-0.8 linear) --------

@dataclass
class Palette:
    inner: tuple          # needles near the trunk / lower crown
    outer: tuple          # mature needles at the crown surface
    tips: tuple           # new growth at branch tips
    bark_low: tuple       # trunk base
    bark_high: tuple      # upper trunk
    branch: tuple = (0.14, 0.11, 0.085)
    dead: tuple = (0.17, 0.155, 0.14)


SPRUCE = Palette(inner=(0.040, 0.058, 0.046), outer=(0.072, 0.108, 0.078), tips=(0.100, 0.140, 0.090),
                 bark_low=(0.17, 0.14, 0.12), bark_high=(0.21, 0.16, 0.13))
YOUNG_SPRUCE = Palette(inner=(0.046, 0.066, 0.046), outer=(0.084, 0.122, 0.078), tips=(0.118, 0.160, 0.092),
                       bark_low=(0.19, 0.16, 0.13), bark_high=(0.22, 0.17, 0.13))
PINE = Palette(inner=(0.050, 0.062, 0.046), outer=(0.086, 0.106, 0.074), tips=(0.112, 0.132, 0.086),
               bark_low=(0.17, 0.145, 0.13), bark_high=(0.40, 0.21, 0.12),
               branch=(0.30, 0.19, 0.12))
FIR = Palette(inner=(0.034, 0.054, 0.050), outer=(0.060, 0.096, 0.086), tips=(0.086, 0.128, 0.104),
              bark_low=(0.22, 0.21, 0.20), bark_high=(0.27, 0.26, 0.24))


@dataclass
class Species:
    name: str
    seed: int
    height: float
    crown_base: float        # share of the height without live branches
    radius: float            # widest crown radius (m)
    shape: str               # spruce | fir | pine
    whorl_spacing: float     # metres between whorls
    branches: tuple          # branches per whorl (min, max)
    cell: int
    palette: Palette
    trunk_radius: float
    droop: float = 0.3
    curtains: bool = False
    spray_length: float = 0.75


SPECIES = [
    Species("Spruce_tall_01", 7101, 24.0, 0.14, 3.6, "spruce", 0.46, (6, 8), CELL_SPRUCE, SPRUCE,
            0.36, droop=0.42, curtains=True, spray_length=1.0),
    Species("Spruce_young_01", 7213, 9.5, 0.02, 2.3, "spruce", 0.34, (5, 7), CELL_SPRUCE, YOUNG_SPRUCE,
            0.15, droop=0.22, spray_length=0.72),
    Species("Pine_scots_01", 7331, 20.0, 0.58, 3.8, "pine", 0.95, (3, 5), CELL_PINE, PINE,
            0.30, droop=0.0, spray_length=0.9),
    Species("Fir_subalpine_01", 7457, 15.0, 0.05, 1.95, "fir", 0.36, (5, 7), CELL_FIR, FIR,
            0.22, droop=0.18, spray_length=0.72),
]


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def scale_color(c, s):
    return tuple(x * s for x in c)


def smoothstep(a, b, x):
    t = min(1.0, max(0.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


# --- textures ---------------------------------------------------------------

class Canvas:
    """Linear RGB + coverage canvas with anti-aliased strokes."""

    def __init__(self, size):
        self.rgb = np.zeros((size, size, 3))
        self.a = np.zeros((size, size))
        self.size = size

    def segment(self, p0, p1, width, c0, c1=None):
        c1 = c0 if c1 is None else c1
        x0, y0 = p0; x1, y1 = p1
        r = width * 0.5 + 1.0
        bx0 = max(0, int(math.floor(min(x0, x1) - r))); bx1 = min(self.size - 1, int(math.ceil(max(x0, x1) + r)))
        by0 = max(0, int(math.floor(min(y0, y1) - r))); by1 = min(self.size - 1, int(math.ceil(max(y0, y1) + r)))
        if bx0 > bx1 or by0 > by1:
            return
        gx, gy = np.meshgrid(np.arange(bx0, bx1 + 1) + 0.5, np.arange(by0, by1 + 1) + 0.5)
        dx, dy = x1 - x0, y1 - y0
        ll = dx * dx + dy * dy
        t = np.clip(((gx - x0) * dx + (gy - y0) * dy) / max(ll, 1e-9), 0, 1)
        d = np.hypot(gx - (x0 + t * dx), gy - (y0 + t * dy))
        # Needles taper toward their tip.
        half = width * 0.5 * (1.0 - 0.55 * t)
        cov = np.clip(half + 0.5 - d, 0, 1)
        if not cov.any():
            return
        col = np.asarray(c0)[None, None, :] * (1 - t[..., None]) + np.asarray(c1)[None, None, :] * t[..., None]
        sl = (slice(by0, by1 + 1), slice(bx0, bx1 + 1))
        self.rgb[sl] = self.rgb[sl] * (1 - cov[..., None]) + col * cov[..., None]
        self.a[sl] = self.a[sl] + cov * (1 - self.a[sl])


def needle_color(rng, base, tip_share):
    v = 0.82 + 0.3 * rng.random()
    hue = rng.random() * 0.08 - 0.04
    c = (base[0] * v * (1 + hue), base[1] * v, base[2] * v * (1 - hue))
    return mix(c, (min(1, c[0] * 1.15), min(1, c[1] * 1.12), c[2]), tip_share)


TWIG = (0.30, 0.22, 0.16)
NEEDLE = (0.56, 0.78, 0.55)         # neutral; species colour is in the vertex colours
NEEDLE_PINE = (0.64, 0.78, 0.60)


def draw_twig_with_needles(cv, rng, start, direction, length, needle_len, needle_w, spacing,
                           angle_range, forward_bias, twig_w, base, flat=False, side_twigs=0,
                           side_angle=45.0, depth=0):
    """A twig from `start` along `direction` (radians) with needles both sides."""
    pts = []
    x, y = start
    steps = max(2, int(length / 6))
    bend = (rng.random() - 0.5) * 0.25
    for i in range(steps + 1):
        t = i / steps
        a = direction + bend * t
        pts.append((x + math.cos(a) * length * t, y + math.sin(a) * length * t))
    for p0, p1 in zip(pts[:-1], pts[1:]):
        cv.segment(p0, p1, twig_w, TWIG)
    # Side twigs first, so the main twig's needles overlap them.
    for k in range(side_twigs):
        t = 0.18 + 0.7 * (k + rng.random() * 0.6) / side_twigs
        i = min(steps, int(t * steps))
        side = 1 if k % 2 == 0 else -1
        a = direction + bend * t + side * math.radians(side_angle + rng.random() * 15)
        draw_twig_with_needles(cv, rng, pts[i], a, length * (0.55 - 0.3 * t) * (0.8 + 0.4 * rng.random()),
                               needle_len * 0.9, needle_w, spacing * 1.1, angle_range, forward_bias,
                               twig_w * 0.6, base, flat, 0, side_angle, depth + 1)
    distance = 0.0
    total = length
    while distance < total:
        t = distance / total
        i = min(steps - 1, int(t * steps))
        p = (pts[i][0] + (pts[i + 1][0] - pts[i][0]) * (t * steps - i),
             pts[i][1] + (pts[i + 1][1] - pts[i][1]) * (t * steps - i))
        a0 = direction + bend * t
        tip = smoothstep(0.65, 1.0, t)
        count = 2 if flat else 3
        for k in range(count):
            side = 1 if k % 2 == 0 else -1
            if not flat and k == 2:
                side = 1 if rng.random() < 0.5 else -1
            spread = math.radians(angle_range[0] + rng.random() * (angle_range[1] - angle_range[0]))
            a = a0 + side * (spread - forward_bias)
            nl = needle_len * (0.75 + 0.45 * rng.random()) * (1.0 - 0.35 * tip)
            if not flat and k == 2:
                nl *= 0.45   # foreshortened needles pointing at the viewer
            q = (p[0] + math.cos(a) * nl, p[1] + math.sin(a) * nl)
            c = needle_color(rng, base, tip)
            cv.segment(p, q, needle_w * (0.9 + 0.2 * rng.random()), scale_color(c, 0.82), c)
        distance += spacing * (0.8 + 0.4 * rng.random())


def frond(cv, rng, start, direction, length, depth, max_depth, needle_len, needle_w, flat, base,
          twig_w, side_angle=48.0):
    """A branchlet: side twigs alternate along it inside a tapering
    envelope, recursively, then every twig is clothed in needles."""
    if depth < max_depth:
        count = max(3, int(9 - 3 * depth))
        for k in range(count):
            t = 0.08 + 0.82 * (k + 0.3 + 0.4 * rng.random()) / count
            side = 1 if k % 2 == 0 else -1
            envelope = math.sin(math.pi * (0.12 + 0.88 * t)) ** 0.6 * (1 - 0.35 * t)
            sub = length * (0.5 if depth == 0 else 0.42) * envelope * (0.8 + 0.4 * rng.random())
            p = (start[0] + math.cos(direction) * length * t, start[1] + math.sin(direction) * length * t)
            a = direction + side * math.radians(side_angle + rng.uniform(-8, 10))
            frond(cv, rng, p, a, sub, depth + 1, max_depth, needle_len * 0.92, needle_w, flat, base,
                  twig_w * 0.65, side_angle)
    draw_twig_with_needles(cv, rng, start, direction, length, needle_len, needle_w,
                           needle_w * (1.25 if flat else 1.45), (65, 95) if flat else (30, 88),
                           math.radians(8 if flat else 18), twig_w, base, flat=flat)


def make_needle_atlas(size=1024):
    """2x2 atlas: spruce spray, fir spray, pine tuft, clump (LOD2). Each cell's
    spray runs left (attachment, u=0) to right (tip, u=1) around v=0.5."""
    cell = size // 2
    rng = random.Random(9001)
    cells = []

    # Spruce: dense, all-round short needles on a twice-branched frond.
    cv = Canvas(cell)
    frond(cv, rng, (cell * 0.03, cell * 0.5), 0.0, cell * 0.92, 0, 2, cell * 0.036, cell * 0.0058,
          False, NEEDLE, cell * 0.012)
    cells.append(cv)

    # Fir: flat, two-ranked, longer blunt needles.
    cv = Canvas(cell)
    frond(cv, rng, (cell * 0.03, cell * 0.5), 0.0, cell * 0.9, 0, 2, cell * 0.042, cell * 0.0074,
          True, NEEDLE, cell * 0.012, side_angle=55)
    cells.append(cv)

    # Pine: short twig ending in long needle fans.
    cv = Canvas(cell)
    for k in range(3):
        y = cell * (0.5 + (k - 1) * 0.16)
        a = (k - 1) * 0.28
        x0 = cell * (0.03 + 0.08 * abs(k - 1))
        twig_end = (x0 + math.cos(a) * cell * 0.45, y + math.sin(a) * cell * 0.45)
        cv.segment((x0, y), twig_end, cell * 0.014, TWIG)
        # Bundles along the twig and a dense fan at its end.
        for i in range(320):
            t = 0.35 + 0.65 * rng.random() ** 0.6
            p = (x0 + (twig_end[0] - x0) * t, y + (twig_end[1] - y) * t)
            ang = a + (rng.random() - 0.5) * math.radians(150) * (1.2 - 0.6 * t)
            nl = cell * (0.12 + 0.1 * rng.random()) * (0.6 + 0.6 * t)
            q = (p[0] + math.cos(ang) * nl, p[1] + math.sin(ang) * nl)
            c = needle_color(rng, NEEDLE_PINE, smoothstep(0.7, 1.0, t) * 0.6)
            cv.segment(p, q, cell * 0.0055, scale_color(c, 0.8), c)
    cells.append(cv)

    # Clump: several fronds fanned into a rounded mass (LOD2 whorl cards).
    cv = Canvas(cell)
    for k in range(7):
        a = (k - 3) * 0.2 + (rng.random() - 0.5) * 0.12
        start = (cell * (0.04 + 0.03 * abs(k - 3)), cell * (0.5 + (k - 3) * 0.05))
        frond(cv, rng, start, a, cell * (0.7 + 0.2 * rng.random()) * (1 - 0.08 * abs(k - 3)), 0, 1,
              cell * 0.04, cell * 0.0068, False, NEEDLE, cell * 0.012)
    cells.append(cv)

    # Soft fade at each cell's border: strokes cut by the cell edge would
    # otherwise show a straight line.
    yy, xx = np.meshgrid(np.linspace(0, 1, cell), np.linspace(0, 1, cell), indexing="ij")
    edge = np.minimum(np.minimum(xx, 1 - xx), np.minimum(yy, 1 - yy))
    fade = np.clip((edge - 0.006) / 0.05, 0, 1)

    rgb = np.zeros((size, size, 3)); a = np.zeros((size, size))
    for k, cv in enumerate(cells):
        y0, x0 = (k // 2) * cell, (k % 2) * cell
        cov = np.clip(cv.a, 0, 1)
        rgb[y0:y0 + cell, x0:x0 + cell] = cv.rgb / np.maximum(cov, 1e-6)[..., None]
        a[y0:y0 + cell, x0:x0 + cell] = cov * fade
    # Keep cell borders empty so mips do not bleed across sprays.
    border = 3
    for k in range(2):
        a[k * cell:k * cell + border, :] = 0; a[(k + 1) * cell - border:(k + 1) * cell, :] = 0
        a[:, k * cell:k * cell + border] = 0; a[:, (k + 1) * cell - border:(k + 1) * cell] = 0
    filled = wa.push_pull_fill(rgb, (a > 0.02).astype(float) * a)
    return filled, a


def make_bark(width=256, height=512):
    """Tileable fissured bark: vertical plates, horizontal cracks."""
    plates = wa.fbm_2d((height, width), (6, 5), 4, 4401)
    ridges = 1 - np.abs(wa.fbm_2d((height, width), (3, 9), 3, 4402) * 2 - 1)
    cracks = wa.fbm_2d((height, width), (24, 4), 2, 4403)
    h = 0.55 * plates + 0.45 * ridges ** 3
    h -= 0.25 * np.clip((cracks - 0.62) * 6, 0, 1)
    h = (h - h.min()) / (h.max() - h.min())
    fine = wa.fbm_2d((height, width), (32, 16), 2, 4404)
    color_v = 0.55 + 0.5 * h + 0.15 * (fine - 0.5)
    lichen = np.clip((wa.fbm_2d((height, width), (4, 2), 3, 4405) - 0.62) * 4, 0, 1) * h
    rgb = np.stack([color_v, color_v * 0.94, color_v * 0.86], axis=-1)
    rgb = rgb * (1 - 0.35 * lichen[..., None]) + np.array([0.55, 0.62, 0.48]) * 0.35 * lichen[..., None]
    rgb = np.clip(rgb, 0, 1) * 0.85
    # Tangent-space normal from the height (x along u = columns, y along v = rows).
    strength = 6.0
    dx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5 * strength
    dy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * 0.5 * strength
    n = wa.normalize(np.stack([-dx, -dy, np.ones_like(h)], axis=-1))
    return rgb, n * 0.5 + 0.5


# --- tree layout ------------------------------------------------------------

@dataclass
class Branch:
    origin: np.ndarray
    yaw: float
    pitch: float
    length: float
    droop: float
    height_share: float      # 0 crown base .. 1 top
    seed: int
    whorl: int
    radius: float


@dataclass
class Tree:
    species: Species
    branches: list = field(default_factory=list)
    dead: list = field(default_factory=list)
    trunk: list = field(default_factory=list)    # (y, radius, offset_x, offset_z)
    tint: tuple = (1.0, 1.0, 1.0)


def crown_radius(sp: Species, t):
    """Crown radius at share t (0 crown base, 1 top)."""
    if sp.shape == "spruce":
        return sp.radius * (1 - t) ** 0.95 * (0.82 + 0.18 * smoothstep(0, 0.12, t))
    if sp.shape == "fir":
        return sp.radius * (1 - t) ** 1.25
    # Pine: rounded, flat-topped crown.
    return sp.radius * math.sin(math.pi * (0.1 + 0.9 * t)) ** 0.7 * (1 - 0.35 * t)


def build_tree(sp: Species) -> Tree:
    rng = random.Random(sp.seed)
    tree = Tree(sp)
    tree.tint = (0.94 + 0.12 * rng.random(), 0.95 + 0.1 * rng.random(), 0.94 + 0.12 * rng.random())
    # Trunk axis with a slight lean and wobble.
    lean = (rng.uniform(-1, 1) * 0.012 * sp.height, rng.uniform(-1, 1) * 0.012 * sp.height)
    segments = max(6, int(sp.height / 1.2))
    for i in range(segments + 1):
        t = i / segments
        y = sp.height * t
        wobble = 0.05 * math.sin(t * 9 + sp.seed) * sp.trunk_radius
        r = sp.trunk_radius * (1 - t) ** 0.85 + 0.01
        r *= 1 + 0.55 * math.exp(-y / (0.08 * sp.height + 0.2))   # root flare
        tree.trunk.append((y, r, lean[0] * t * t + wobble, lean[1] * t * t))

    def trunk_at(y):
        t = min(1.0, max(0.0, y / sp.height)) * (len(tree.trunk) - 1)
        i = min(len(tree.trunk) - 2, int(t))
        f = t - i
        a, b = tree.trunk[i], tree.trunk[i + 1]
        return (a[1] + (b[1] - a[1]) * f, np.array([a[2] + (b[2] - a[2]) * f, y, a[3] + (b[3] - a[3]) * f]))

    base_y = sp.height * sp.crown_base
    crown = sp.height * 0.985 - base_y
    y = base_y
    whorl = 0
    yaw0 = rng.random() * math.tau
    while y < sp.height * 0.97:
        t = (y - base_y) / crown
        count = rng.randint(*sp.branches)
        if sp.shape == "pine" and t > 0.85:
            count = max(2, count - 1)
        yaw0 += 2.39996 + rng.uniform(-0.3, 0.3)   # golden angle between whorls
        for k in range(count):
            yaw = yaw0 + math.tau * k / count + rng.uniform(-0.35, 0.35)
            reach = crown_radius(sp, t) * rng.uniform(0.82, 1.08)
            if sp.shape == "pine":
                reach *= rng.uniform(0.75, 1.15)
                pitch = math.radians(rng.uniform(15, 40) - 25 * (1 - t))
            elif sp.shape == "fir":
                pitch = math.radians(rng.uniform(-8, 10) + 20 * t)
            else:
                pitch = math.radians(rng.uniform(-14, 4) + 30 * t)
            radius, origin = trunk_at(y + rng.uniform(-0.1, 0.1) * sp.whorl_spacing)
            length = max(0.25, reach - radius)
            droop = sp.droop * (1 - 0.6 * t) * rng.uniform(0.7, 1.2)
            tree.branches.append(Branch(origin, yaw, pitch, length, droop, t, rng.randrange(1 << 30),
                                        whorl, radius))
        spacing = sp.whorl_spacing * (0.8 + 0.4 * rng.random()) * (1 - 0.35 * t)
        # Spruce and fir also grow shorter branches between the whorls.
        if sp.shape != "pine" and t < 0.92:
            for k in range(rng.randint(2, 3)):
                ti = (y + spacing * (0.35 + 0.3 * rng.random()) - base_y) / crown
                yaw = rng.random() * math.tau
                radius, origin = trunk_at(base_y + ti * crown)
                reach = crown_radius(sp, min(1.0, ti)) * rng.uniform(0.5, 0.75)
                pitch = math.radians(rng.uniform(-10, 10) + 25 * ti)
                tree.branches.append(Branch(origin, yaw, pitch, max(0.2, reach - radius),
                                            sp.droop * 0.6 * (1 - 0.6 * ti), min(1.0, ti),
                                            rng.randrange(1 << 30), whorl, radius))
        whorl += 1
        y += spacing
    # Dead, needleless twigs below the crown (spruce and fir self-prune slowly).
    if sp.shape != "pine":
        y = max(0.8, base_y * 0.25)
        while y < base_y:
            for k in range(rng.randint(2, 4)):
                radius, origin = trunk_at(y)
                tree.dead.append(Branch(origin, rng.random() * math.tau, math.radians(rng.uniform(-25, 5)),
                                        rng.uniform(0.3, 0.9) * min(1.6, sp.radius * 0.45), 0.1, 0.0,
                                        rng.randrange(1 << 30), -1, radius))
            y += sp.whorl_spacing * rng.uniform(0.8, 1.6)
    return tree


def branch_frame(b: Branch):
    d = np.array([math.cos(b.yaw), 0.0, math.sin(b.yaw)])
    return d


def branch_point(b: Branch, s):
    """Point and tangent at share s of the branch (starts at the trunk surface)."""
    d = branch_frame(b)
    rise = math.tan(b.pitch)
    start = b.origin + d * b.radius * 0.9
    horiz = b.length * s
    y = b.length * (rise * s - b.droop * s * s)
    p = start + d * horiz + UP * y
    dy = b.length * (rise - 2 * b.droop * s)
    tangent = wa.normalize(d * b.length + UP * dy)
    return p, tangent


# --- geometry builders --------------------------------------------------------

BARK_TILE_W, BARK_TILE_H = 0.7, 1.4


def add_tube(mesh, rings, sides, color_at, cap_tip=True):
    """rings: list of (center, radius, along, v) along a curve; bark-mapped tube."""
    ring_ids = []
    for center, radius, along, v_coord in rings:
        along = wa.normalize(along)
        ref = np.array([1.0, 0.0, 0.0]) if abs(along[0]) < 0.9 else np.array([0.0, 0.0, 1.0])
        side = wa.normalize(np.cross(along, ref))
        other = np.cross(side, along)
        repeats = max(1, round(math.tau * max(radius, 0.05) / BARK_TILE_W))
        ids = []
        for k in range(sides + 1):
            a = math.tau * k / sides
            n = side * math.cos(a) + other * math.sin(a)
            ids.append(mesh.vertex(center + n * radius, n, (repeats * k / sides, v_coord), color_at(center)))
        ring_ids.append(ids)
    for r0, r1 in zip(ring_ids[:-1], ring_ids[1:]):
        for k in range(sides):
            # Outward faces: a, b along the ring, then up the tube.
            mesh.quad(r0[k + 1], r0[k], r1[k], r1[k + 1])


def trunk_mesh(tree: Tree, sides, step):
    sp = tree.species
    mesh = wa.MeshBuilder()
    pal = sp.palette

    def color_at(p):
        t = p[1] / sp.height
        blend = smoothstep(0.35, 0.75, t) if sp.shape == "pine" else t
        return mix(pal.bark_low, pal.bark_high, blend)

    rings = []
    picks = list(range(0, len(tree.trunk), step))
    if picks[-1] != len(tree.trunk) - 1:
        picks.append(len(tree.trunk) - 1)
    for i in picks:
        y, r, ox, oz = tree.trunk[i]
        nxt = tree.trunk[min(len(tree.trunk) - 1, i + 1)]
        prv = tree.trunk[max(0, i - 1)]
        along = np.array([nxt[2] - prv[2], nxt[0] - prv[0] + 1e-6, nxt[3] - prv[3]])
        rings.append((np.array([ox, y - (0.25 if i == 0 else 0.0), oz]), r, along, -y / BARK_TILE_H))
    add_tube(mesh, rings, sides, color_at)
    return mesh


def branch_wood(mesh, tree: Tree, branches, dead=False):
    sp = tree.species
    pal = sp.palette
    for b in branches:
        if not dead and b.length < 0.6:
            continue
        rings = []
        segs = 2
        base_r = min(b.radius * 0.35, 0.02 + 0.018 * b.length)
        color = pal.dead if dead else pal.branch
        for i in range(segs + 1):
            s = (i / segs) * (1.0 if dead else 0.75)
            p, t = branch_point(b, s)
            if i == 0:
                p = b.origin + branch_frame(b) * b.radius * 0.3
            rings.append((p, base_r * (1 - 0.8 * s) + 0.004, t, -s * b.length / BARK_TILE_H))
        add_tube(mesh, rings, 3, lambda _p, c=color: c)


def cell_uv(cell, u, v):
    cu, cv = (cell % 2) * 0.5, (cell // 2) * 0.5
    return (cu + 0.012 + u * 0.476, cv + 0.012 + v * 0.476)


def needle_color_at(tree: Tree, p, s, jitter, branch_share):
    """Vertex colour: darker inside and low, lighter tips; per-branch jitter."""
    sp = tree.species
    pal = sp.palette
    axis = np.array([tree.trunk[0][2], p[1], tree.trunk[0][3]])
    radial = np.linalg.norm((p - axis)[[0, 2]])
    envelope = max(0.3, crown_radius(sp, min(1.0, max(0.0, branch_share))) + 0.2)
    outside = min(1.0, radial / envelope)
    c = mix(pal.inner, pal.outer, smoothstep(0.15, 0.85, outside))
    c = mix(c, pal.tips, smoothstep(0.75, 1.0, s) * 0.8)
    # Lower crown is shaded by the rest of the tree.
    shade = 0.75 + 0.25 * smoothstep(0.0, 0.5, branch_share)
    return (c[0] * shade * tree.tint[0] * jitter[0], c[1] * shade * tree.tint[1] * jitter[1],
            c[2] * shade * tree.tint[2] * jitter[2], 1.0)


def foliage_normal(tree: Tree, p, face):
    """Crown-shaped normal (out from the axis, tilted up) blended with the card face."""
    axis = np.array([tree.trunk[0][2], p[1] - tree.species.radius * 0.6, tree.trunk[0][3]])
    out = wa.normalize(p - axis)
    if np.dot(face, out) < 0:
        face = -face
    return wa.normalize(out * 0.75 + face * 0.25 + UP * 0.15)


def add_card(mesh, tree, origin, x_axis, y_axis, length, width, cell, rng, s, share, jitter):
    """Spray card: u along x_axis (attachment -> tip), v across y_axis."""
    face = wa.normalize(np.cross(x_axis, y_axis))
    mesh.card_area = getattr(mesh, "card_area", 0.0) + length * width
    corners = [(0, 0), (1, 0), (1, 1), (0, 1)]
    ids = []
    for cu, cv_ in corners:
        p = origin + x_axis * (cu * length) + y_axis * ((cv_ - 0.5) * width)
        # Sprays sag slightly toward the tip.
        p = p - UP * (0.08 * length * cu * cu)
        ss = min(1.0, s + cu * length / max(0.5, tree.species.radius))
        col = needle_color_at(tree, p, ss, jitter, share)
        ids.append(mesh.vertex(p, foliage_normal(tree, p, face), cell_uv(cell, cu, cv_), col))
    mesh.quad(ids[0], ids[1], ids[2], ids[3])


def spray_size(sp: Species, b: Branch):
    return sp.spray_length * (0.75 + 0.5 * min(1.0, b.length / 2.0))


def pine_layout(b: Branch, size):
    """Fork tips of a pine limb: (share, lateral, lift, yaw, roll, extra)."""
    forks = max(4, int(b.length / (size * 0.32)))
    layout = []
    for i in range(forks):
        r = random.Random(b.seed * 7 + i)
        s = 0.3 + 0.7 * (i + r.random()) / forks
        lateral = (r.random() - 0.5) * 1.1 * b.length * s
        lift = r.uniform(-0.1, 0.35) * b.length * s
        layout.append((s, lateral, lift, r.uniform(-0.8, 0.8), r.uniform(-0.6, 0.6), r.random()))
    return layout


def pine_fork_wood(tree: Tree, mesh, keep):
    """Twigs from each limb to its tufts, so forked clumps stay attached."""
    pal = tree.species.palette
    for b in tree.branches:
        size = spray_size(tree.species, b)
        side = np.array([-branch_frame(b)[2], 0.0, branch_frame(b)[0]])
        for i, (s, lateral, lift, *_rest) in enumerate(pine_layout(b, size)):
            if i % keep:
                continue
            a, _ = branch_point(b, s * 0.6)
            p, _ = branch_point(b, min(1.0, s))
            p = p + side * lateral + UP * lift
            r = 0.012 + 0.01 * b.length * (1 - s)
            add_tube(mesh, [(a, r, p - a, 0.0), (p, r * 0.5, p - a, -np.linalg.norm(p - a) / BARK_TILE_H)],
                     3, lambda _p, c=pal.branch: c)


def spray_cards(tree: Tree, b: Branch, lod: int, mesh):
    sp = tree.species
    rng = random.Random(b.seed)
    value = rng.uniform(0.88, 1.12)
    jitter = (value * rng.uniform(0.95, 1.05), value, value * rng.uniform(0.92, 1.08))
    size = spray_size(sp, b)
    cell = sp.cell
    d = branch_frame(b)
    side = np.array([-d[2], 0.0, d[0]])
    if sp.shape == "pine":
        # The limb forks into twigs that end in upturned needle tufts over
        # its outer two thirds; the forks spread the tufts sideways so the
        # crown reads as clumps, not a ring of single cards. Every level
        # draws the same fork layout and keeps fewer, larger tufts.
        layout = pine_layout(b, size)
        keep = (1, 2, 4)[lod]
        scale = (1.0, 1.45, 2.0)[lod]
        for i, (s, lateral, lift, yaw, roll, extra) in enumerate(layout):
            if i % keep:
                continue
            r = random.Random(b.seed * 11 + i)
            p, t = branch_point(b, min(1.0, s))
            p = p + side * lateral + UP * lift
            x = wa.normalize(t * math.cos(yaw) + side * math.sin(yaw) + UP * (0.45 + 0.4 * extra))
            y = wa.normalize(np.cross(UP, x) if abs(x[1]) < 0.95 else side)
            y = wa.normalize(y * math.cos(roll) + np.cross(x, y) * math.sin(roll))
            length = size * scale * (0.85 + 0.3 * extra)
            add_card(mesh, tree, p - x * length * 0.3, x, y, length, length * 0.85, CELL_PINE, r, s,
                     b.height_share, jitter)
            if lod < 2:
                # Crossed card for volume.
                y2 = wa.normalize(np.cross(x, y))
                add_card(mesh, tree, p - x * length * 0.25, x, y2, length * 0.9, length * 0.75, CELL_PINE,
                         r, s, b.height_share, jitter)
        return
    # Spruce / fir: sprays fanned left and right along the branch.
    if lod == 0:
        n = max(2, int(b.length / (size * 0.3)))
        positions = [0.1 + 0.9 * (i + 0.5) / n for i in range(n)]
        scale = 1.0
    elif lod == 1:
        n = max(1, int(b.length / (size * 0.85)))
        positions = [0.2 + 0.8 * (i + 0.5) / n for i in range(n)]
        scale = 1.45
    else:
        return
    for i, s in enumerate(positions):
        r = random.Random(b.seed * 13 + i)
        p, t = branch_point(b, s)
        side_sign = 1 if i % 2 == 0 else -1
        fan = r.uniform(0.35, 0.9) * side_sign * (0.6 if lod else 1.0)
        x = wa.normalize(t * math.cos(fan) + side * math.sin(fan) + UP * r.uniform(-0.15, 0.1))
        y = wa.normalize(np.cross(UP, x))
        roll = r.uniform(-0.45, 0.45)
        y = wa.normalize(y * math.cos(roll) + np.cross(x, y) * math.sin(roll))
        length = size * scale * (0.85 + 0.3 * r.random()) * (1.0 - 0.25 * s)
        add_card(mesh, tree, p - x * length * 0.1, x, y, length, length * 0.62, cell, r, s,
                 b.height_share, jitter)
        if lod == 0 and r.random() < 0.45:
            # A steeply rolled second spray gives the branch thickness from the side.
            y2 = wa.normalize(y * math.cos(1.2) + np.cross(x, y) * math.sin(1.2))
            add_card(mesh, tree, p - x * length * 0.05, x, y2, length * 0.85, length * 0.5, cell, r, s,
                     b.height_share, jitter)
        if sp.curtains and lod == 0 and s > 0.35 and r.random() < 0.55:
            # Hanging second-order branchlets (Norway spruce comb form).
            hang = wa.normalize(-UP * 0.9 + x * 0.3)
            y2 = wa.normalize(np.cross(hang, UP) if abs(hang[1]) < 0.99 else side)
            add_card(mesh, tree, p, hang, y2, length * 0.8, length * 0.45, cell, r, s, b.height_share, jitter)
    # Leading spray along the branch tip.
    p, t = branch_point(b, 1.0)
    r = random.Random(b.seed * 17)
    y = wa.normalize(np.cross(UP, t))
    if lod == 0 or b.length > size:
        add_card(mesh, tree, p - t * size * 0.5 * scale, t, y, size * 0.9 * scale, size * 0.6 * scale,
                 cell, r, 1.0, b.height_share, jitter)
    if lod == 1:
        # One crossed (vertical) card per branch keeps the side silhouette.
        p, t = branch_point(b, 0.6)
        y2 = wa.normalize(np.cross(t, np.cross(UP, t)))
        add_card(mesh, tree, p - t * b.length * 0.45, t, y2, b.length * 0.95, size * 0.85, cell,
                 r, 0.6, b.height_share, jitter)


def whorl_star(tree: Tree, mesh):
    """LOD2: per whorl, a few big clump cards spanning the crown radius."""
    sp = tree.species
    whorls = {}
    for b in tree.branches:
        whorls.setdefault(b.whorl, []).append(b)
    keep = sorted(whorls)[::2] if len(whorls) > 16 else sorted(whorls)
    for w in keep:
        group = whorls[w]
        rng = random.Random(group[0].seed)
        jitter = (rng.uniform(0.9, 1.1),) * 3
        cards = 3 if sp.shape != "pine" else 2
        for k in range(cards):
            b = group[k * len(group) // cards]
            p0 = b.origin
            p1, t = branch_point(b, 1.0)
            x = wa.normalize(p1 - p0)
            y = wa.normalize(np.cross(UP, x))
            roll = 0.6 if k % 2 else -0.4
            y = wa.normalize(y * math.cos(roll) + np.cross(x, y) * math.sin(roll))
            length = np.linalg.norm(p1 - p0) * 1.1
            width = max(sp.whorl_spacing * 2.6, length * 0.7)
            add_card(mesh, tree, p0, x, y, length, width, CELL_CLUMP if sp.shape != "pine" else CELL_PINE,
                     rng, 0.6, b.height_share, jitter)


def build_lod(tree: Tree, lod: int):
    sp = tree.species
    trunk = trunk_mesh(tree, (12, 7, 5)[lod], (1, 2, 3)[lod])
    if lod == 0:
        branch_wood(trunk, tree, tree.branches)
        branch_wood(trunk, tree, tree.dead, dead=True)
    if sp.shape == "pine" and lod < 2:
        if lod == 1:
            branch_wood(trunk, tree, [b for b in tree.branches if b.length > 1.4])
        pine_fork_wood(tree, trunk, (1, 2)[lod])
    needles = wa.MeshBuilder()
    if lod < 2:
        for b in tree.branches:
            spray_cards(tree, b, lod, needles)
    else:
        whorl_star(tree, needles)
    # Leader: a small upright spray at the top.
    if sp.shape != "pine":
        top = np.array([tree.trunk[-1][2], sp.height * 0.97, tree.trunk[-1][3]])
        rng = random.Random(sp.seed + 5)
        for k in range(2 if lod < 2 else 1):
            yaw = k * math.pi / 2
            y = np.array([math.cos(yaw), 0.0, math.sin(yaw)])
            add_card(needles, tree, top - UP * sp.height * 0.06, UP, y, sp.height * 0.07,
                     sp.height * 0.035, sp.cell, rng, 1.0, 1.0, (1, 1, 1))
    return trunk, needles


# --- main ---------------------------------------------------------------------

def write_materials():
    tex = lambda name: wa.asset_path(TEXTURES / name)
    needles = wa.write_material(
        MATERIALS / "Conifer_Needles.material",
        baseColorTexture=tex("Conifer_Needles_Color.ktx2"), useVertexColors=True,
        alphaMode="Mask", alphaCutoff=NEEDLE_CUTOFF, doubleSided=True, roughness=0.78,
        alphaCoverageTransmission=0.12, alphaEdgeFade=0.75, reflectionStrength=0.1,
        foliageColorVariation=FOLIAGE_VARIATION, foliageTranslucency=NEEDLE_TRANSLUCENCY)
    bark = wa.write_material(
        MATERIALS / "Conifer_Bark.material",
        baseColorTexture=tex("Conifer_Bark_Color.ktx2"), normalTexture=tex("Conifer_Bark_Normal.ktx2"),
        useVertexColors=True, roughness=0.92, normalScale=1.0)
    return needles, bark


def write_impostor_material(name):
    tex = lambda suffix: wa.asset_path(TEXTURES / f"{name}_Impostor_{suffix}.ktx2")
    return wa.write_material(
        MATERIALS / f"{name}_Impostor.material",
        baseColorTexture=tex("Color"), normalTexture=tex("Normal"), useVertexColors=False,
        alphaMode="Mask", alphaCutoff=IMPOSTOR_CUTOFF, doubleSided=True, roughness=0.86,
        normalScale=0.55, alphaEdgeFade=1.0, impostorBillboard=True,
        foliageColorVariation=FOLIAGE_VARIATION)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--no-impostors", action="store_true", help="skip impostor baking")
    parser.add_argument("--only", help="generate one species")
    args = parser.parse_args()

    SOURCES.mkdir(parents=True, exist_ok=True)
    print("needle atlas, bark")
    rgb, alpha = make_needle_atlas()
    needles_png = SOURCES / "Conifer_Needles_Color.png"
    wa.write_png(needles_png, np.concatenate([wa.linear_to_srgb(rgb), alpha[..., None]], axis=2))
    bark_rgb, bark_n = make_bark()
    bark_png = SOURCES / "Conifer_Bark_Color.png"
    bark_n_png = SOURCES / "Conifer_Bark_Normal.png"
    wa.write_png(bark_png, wa.linear_to_srgb(bark_rgb))
    wa.write_png(bark_n_png, bark_n)
    wa.cook(needles_png, TEXTURES / "Conifer_Needles_Color.ktx2", "color", alpha_cutoff=NEEDLE_CUTOFF)
    wa.cook(bark_png, TEXTURES / "Conifer_Bark_Color.ktx2", "color")
    wa.cook(bark_n_png, TEXTURES / "Conifer_Bark_Normal.ktx2", "normal")
    needles_material, bark_material = write_materials()

    needle_texture = wa.Texture(np.concatenate([rgb, alpha[..., None]], axis=2))
    bark_texture = wa.Texture(np.concatenate([bark_rgb, np.ones(bark_rgb.shape[:2] + (1,))], axis=2))

    for sp in SPECIES:
        if args.only and sp.name != args.only:
            continue
        tree = build_tree(sp)
        counts = []
        areas = []
        lod_paths = []
        surfaces0 = None
        for lod in range(3):
            trunk, needles = build_lod(tree, lod)
            tp = MESHES / f"{sp.name}_LOD{lod}_Trunk.mesh"
            np_ = MESHES / f"{sp.name}_LOD{lod}_Needles.mesh"
            counts.append(trunk.write_native(tp) + needles.write_native(np_))
            areas.append(getattr(needles, "card_area", 0.0))
            lod_paths.append((wa.asset_path(tp), wa.asset_path(np_)))
            if lod == 0:
                surfaces0 = [wa.Surface.from_builder(trunk, texture=bark_texture),
                             wa.Surface.from_builder(needles, texture=needle_texture,
                                                     alpha_cutoff=NEEDLE_CUTOFF)]
        impostor_mesh = MESHES / f"{sp.name}_Impostor.mesh"
        impostor_material = write_impostor_material(sp.name)
        if not args.no_impostors or not impostor_mesh.exists():
            color_png, normal_png, tris = wa.bake_impostor(surfaces0, sp.name, SOURCES, impostor_mesh,
                                                           atlas_size=1024, supersample=2)
            wa.cook(color_png, TEXTURES / f"{sp.name}_Impostor_Color.ktx2", "color",
                    alpha_cutoff=IMPOSTOR_CUTOFF)
            wa.cook(normal_png, TEXTURES / f"{sp.name}_Impostor_Normal.ktx2", "normal")
            counts.append(tris)
        # Prefab: trunk part carries the impostor as its last level; the
        # needle part has no entry there and is hidden.
        lod1, lod2 = (sp.height * 2.5, sp.height * 9)
        wa.write_prefab(PREFABS / f"{sp.name}.prefab", sp.name, [
            dict(name="Trunk", mesh=lod_paths[0][0],
                 lods=[lod_paths[1][0], lod_paths[2][0], wa.asset_path(impostor_mesh)],
                 material=wa.engine_path(bark_material),
                 lod_materials=["", "", wa.engine_path(impostor_material)], lod1=lod1, lod2=lod2,
                 wind=TREE_WIND, tree_wind=True),
            dict(name="Needles", mesh=lod_paths[0][1], lods=[lod_paths[1][1], lod_paths[2][1]],
                 material=wa.engine_path(needles_material), lod1=lod1, lod2=lod2,
                 wind=TREE_WIND, tree_wind=True),
        ])
        print(f"{sp.name}: {sp.height:.0f} m, {len(tree.branches)} branches; "
              f"triangles LOD0/1/2/impostor = {'/'.join(str(c) for c in counts)}; "
              f"card area LOD1/LOD0 = {areas[1] / max(areas[0], 1e-6):.2f}")
    # Crown normals and canopy occlusion for the freshly written needle meshes
    # (cooked meshes are skipped, so species not regenerated keep theirs).
    import cook_foliage_lighting
    cook_foliage_lighting.cook_prefabs([PREFABS])


if __name__ == "__main__":
    main()
