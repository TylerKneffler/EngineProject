"""Scanned-leaf bushes and shrubs for the grass fields, built from card clusters.

The Poly Haven plant scans (CC0: shrub_04, shrub_02, nettle_plant) are sprigs
of 0.1-1.9 m whose every leaf is its own UV island, so they cannot be simplified
below ~1,200-1,900 triangles each, and a bush made of thirty of them would cost
~55k triangles at every distance. Instead, as games do:

  1. each sprig is rendered (orthographic, two views) into one alpha-masked
     card atlas: photo albedo, depth-darkened so the back of a sprig reads as
     inside the plant;
  2. bushes, shrub clumps, nettles and saplings are built from crossed pairs
     of those cards around an ellipsoid, every card facing out of the plant.
     Both sides of a card are separate triangles that share the outward
     (sphere-like) normal, so the plant lights as a volume from every side
     (the grass shader flips the normal of back faces).

  3. ferns: fern_02's texture is already five flat fronds; they are cut into
     the bottom half of the atlas, and each fern is a rosette of arching,
     drooping frond strips.

Vertex colour carries interior shade and per-card tint; vertex alpha is the
grass wind response. Outputs (three levels each, _lod1 and _lod2 with fewer,
larger cards):
  Assets/Mesh/Grass/scan_bush[_tall].mesh, scan_shrub, scan_nettle, scan_sapling,
  scan_fern[_large]
  Assets/Models/Woodland/Scans/Textures/ScanBushCards_diff.ktx2
  Assets/Materials/Grass/scan_bush.material

Needs the scans in .local/polyhaven (tools/fetch_polyhaven_props.py --list
--only shrub_04,shrub_02,nettle_plant,fern_02 downloads them), PropCook,
TextureCook and scipy.

Usage: python tools/scan_bush_cards.py [--preview out.png]
"""

import argparse
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import woodland_assets as wa  # noqa: E402

ROOT = wa.ROOT
CACHE = ROOT / ".local" / "polyhaven"
WORK = ROOT / ".local" / "scan_bushes"
MESH_DIR = wa.ASSETS / "Mesh" / "Grass"
TEXTURE = wa.ASSETS / "Models" / "Woodland" / "Scans" / "Textures" / "ScanBushCards_diff.ktx2"
MATERIAL = wa.ASSETS / "Materials" / "Grass" / "scan_bush.material"

ATLAS = 2048          # width; the sprig cells fill the top ATLAS x ATLAS square
ATLAS_HEIGHT = 6144   # then the fern fronds (2048 tall), then the flower and herb cells
FLOWER_BAND = 4096    # first row of the flower and herb cells
GRID = 4
CELL = ATLAS // GRID
FRONDS = 5            # fern_02's texture is five flat fronds, cut out as they are
SUPERSAMPLE = 2
WIND = 0.25  # vertex alpha: wind response (1 = a grass blade)

# Atlas cells: (scan, node, view). view 0 looks along +Z, view 1 along +X.
CELLS = [
    ("shrub_04", 1, 0), ("shrub_04", 1, 1), ("shrub_04", 2, 0), ("shrub_04", 2, 1),
    ("shrub_04", 3, 0), ("shrub_04", 3, 1), ("shrub_04", 4, 0), ("shrub_04", 4, 1),
    ("nettle_plant", 1, 0), ("nettle_plant", 1, 1), ("nettle_plant", 2, 0), ("nettle_plant", 2, 1),
    ("shrub_02", 1, 0), ("shrub_02", 2, 0), ("shrub_02", 3, 0), ("shrub_02", 4, 0),
]
# Flowers and herbs (third band). view 2 looks straight down: low rosettes
# (celandine, dandelion, the weed) read from above, as cards lying flat.
# Nodes as PropCook --split numbers them (its --list order, from 1);
# dandelion_01 lists its own LOD nodes too (b_LOD0 is the fourth).
FLOWER_CELLS = [
    ("celandine_01", 3, 0), ("celandine_01", 3, 2), ("celandine_01", 4, 0), ("celandine_01", 4, 2),
    ("dandelion_01", 1, 0), ("dandelion_01", 1, 2), ("dandelion_01", 4, 0), ("dandelion_01", 4, 2),
    ("periwinkle_plant", 1, 0), ("periwinkle_plant", 1, 1), ("periwinkle_plant", 2, 0), ("periwinkle_plant", 3, 0),
    ("weed_plant_02", 1, 2), ("weed_plant_02", 2, 2), ("weed_plant_02", 1, 0), ("weed_plant_02", 3, 2),
]
# Scans whose petals keep their colour: only their green leaves are graded.
FLOWERS = {"celandine_01", "dandelion_01", "periwinkle_plant"}


def find_tool(name):
    for config in ("RelWithDebInfo", "Release", "Debug"):
        path = ROOT / "build" / config / f"{name}.exe"
        if path.exists():
            return path
    raise SystemExit(f"{name} is not built: cmake --build build --config RelWithDebInfo --target {name}")


def sprig_mesh(scan, node):
    """Full-detail cooked sprig (PropCook --split numbers nodes 01, 02, ...)."""
    folder = WORK / "full"
    path = folder / f"{scan}_{node:02d}_LOD0.mesh"
    if not path.exists():
        folder.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(find_tool("PropCook")), str(CACHE / scan / f"{scan}.fbx"), str(folder),
                        "--name", scan, "--split", "--lods", "60000"], check=True)
    return wa.read_native_mesh(path)


_textures = {}


def scan_texture(scan):
    if scan not in _textures:
        color = np.asarray(Image.open(CACHE / scan / f"{scan}_diff.jpg").convert("RGB"), np.float64) / 255
        alpha_path = CACHE / scan / f"{scan}_alpha.jpg"
        # Scans with real leaf geometry (periwinkle) ship no alpha map.
        alpha = (np.asarray(Image.open(alpha_path).convert("L"), np.float64) / 255
                 if alpha_path.exists() else np.ones(color.shape[:2]))
        rgba = np.concatenate([wa.srgb_to_linear(color), alpha[..., None]], axis=2)
        _textures[scan] = wa.Texture(rgba)
    return _textures[scan]


def grade(albedo, amount=None, keep_petals=False):
    """Moves leaf colour toward the meadow's: the scans are a dusty sage that
    reads as dry brush beside the grass (reference meadows: red about 0.7-0.8
    of green in sRGB; bush leaves a little deeper). Luminance is kept. The
    stems (red over green) are a bright red-brown that speckles the bush like
    autumn leaves, so they move halfway to a grey-brown bark."""
    weights = np.array([0.2126, 0.7152, 0.0722])
    lum = albedo @ weights
    leafiness = np.clip((albedo[..., 1] - albedo[..., 0]) / np.maximum(albedo[..., 1], 1e-6) * 6.0 + 0.2, 0, 1)
    leaf = np.array([0.36, 1.0, 0.09]) / (np.array([0.36, 1.0, 0.09]) @ weights)
    bark = np.array([0.55, 0.45, 0.32]) / (np.array([0.55, 0.45, 0.32]) @ weights)
    graded = lum[..., None] * (leaf * leafiness[..., None] + bark * (1 - leafiness[..., None]))
    amount = GRADE if amount is None else amount
    strength = amount * leafiness + (0.0 if keep_petals else 0.5) * (1 - leafiness)
    return albedo + (graded - albedo) * strength[..., None]


GRADE = 0.75
FERN_GRADE = 0.35  # the fern scan is already a fresh green


def bake_cell(scan, node, view):
    """Renders one sprig into a CELL x CELL RGBA tile (row 0 = top).
    Returns the tile (linear rgb, alpha) and the card: its world width and
    height, the uv rectangle inside the cell, and the root's x in the card."""
    p, n, uv, c, idx = sprig_mesh(scan, node)
    surface = wa.Surface(p, n, uv, np.ones((len(p), 4)), idx, texture=scan_texture(scan),
                         alpha_cutoff=0.5, use_vertex_colors=False)
    right = np.array([1.0, 0, 0]) if view != 1 else np.array([0, 0, -1.0])
    forward = {0: np.array([0, 0, 1.0]), 1: np.array([1.0, 0, 0]), 2: np.array([0, 1.0, 0])}[view]
    # Image up: world up for the side views; -Z for the top view.
    up = np.array([0, 1.0, 0]) if view != 2 else np.array([0, 0, -1.0])
    lo = np.array([(p @ right).min(), (p @ up).min()])
    hi = np.array([(p @ right).max(), (p @ up).max()])
    size = hi - lo
    margin = 0.03 * size.max()
    lo -= margin; hi += margin
    size = hi - lo
    scale = CELL / size.max()                       # pixels per metre (cell resolution)
    res = CELL * SUPERSAMPLE
    albedo, alpha, normal, depth = wa.rasterize(
        [surface], right, up, forward, lo, scale * SUPERSAMPLE, (res, res))
    # Depth shade: leaves at the back of the sprig are inside the plant.
    covered = alpha > 0
    if covered.any():
        d = depth[covered]
        near, far = np.percentile(d, 95), np.percentile(d, 5)
        front = np.clip((depth - far) / max(near - far, 1e-6), 0, 1)
        albedo *= (0.72 + 0.28 * front)[..., None]
    if scan == "periwinkle_plant":
        # The scan is Madagascar periwinkle: pink petals and yellowed leaves.
        # Lesser periwinkle, the woodland ground cover it stands in for, flowers
        # blue-violet: magenta pixels turn blue-violet (luminance kept), and the
        # rest is graded as leaves and stems (yellowed leaves no longer count as
        # petals).
        petal = (albedo[..., 0] > albedo[..., 1] * 1.3) & (albedo[..., 2] > albedo[..., 1] * 1.15)
        weights = np.array([0.2126, 0.7152, 0.0722])
        violet = np.array([0.30, 0.25, 0.85]) / (np.array([0.30, 0.25, 0.85]) @ weights)
        recoloured = (albedo @ weights)[..., None] * violet
        albedo = grade(albedo, 0.5)
        albedo[petal] = recoloured[petal]
    else:
        albedo = grade(albedo, 0.5 if scan in FLOWERS else None, keep_petals=scan in FLOWERS)
    rgb = wa.downsample(albedo * alpha[..., None], SUPERSAMPLE)
    a = wa.downsample(alpha, SUPERSAMPLE) if alpha.ndim == 2 else wa.downsample(alpha[..., None], SUPERSAMPLE)[..., 0]
    a = a.reshape(CELL, CELL)
    rgb = rgb / np.maximum(a[..., None], 1e-6)
    tile_rgb, tile_a = np.flipud(rgb), np.flipud(a)  # row 0 = top
    # Card: the used part of the cell (the sprig's bounds).
    used = size * scale / CELL                       # fraction of the cell in u, v
    low = p[:, 1] < lo[1] + margin + 0.05 * size[1]
    # A top view's card is centred on the plant.
    root = 0.5 if view == 2 else (np.median(p[low] @ right) - lo[0]) / size[0] if low.any() else 0.5
    return tile_rgb, tile_a, dict(width=size[0], height=size[1], u=used[0], v=used[1], root=root)


def cut_fronds():
    """The fern scan's texture: FRONDS flat fronds, tip up. Returns each as a
    linear rgb + alpha crop, largest first."""
    from scipy import ndimage
    texture = scan_texture("fern_02").data
    small = texture[::8, ::8, 3] > 0.5
    labels, count = ndimage.label(ndimage.binary_dilation(small, iterations=2))
    sizes = ndimage.sum(small, labels, range(1, count + 1))
    fronds = []
    for label in np.argsort(sizes)[::-1][:FRONDS] + 1:
        ys, xs = np.nonzero(labels == label)
        y0, y1 = max(0, ys.min() * 8 - 8), min(texture.shape[0], ys.max() * 8 + 16)
        x0, x1 = max(0, xs.min() * 8 - 8), min(texture.shape[1], xs.max() * 8 + 16)
        crop = texture[y0:y1, x0:x1].copy()
        # Only this frond: other components inside the box are cleared.
        mask = np.kron(labels[y0 // 8:(y1 + 7) // 8, x0 // 8:(x1 + 7) // 8] == label,
                       np.ones((8, 8), bool))[:y1 - y0, :x1 - x0]
        crop[..., 3] *= mask
        fronds.append(crop)
    return fronds


def build_atlas():
    rgb = np.zeros((ATLAS_HEIGHT, ATLAS, 3))
    alpha = np.zeros((ATLAS_HEIGHT, ATLAS))
    cards = {}
    # Sprig cells in the top square, flower and herb cells in the third band.
    placed = [(i, cell, 0) for i, cell in enumerate(CELLS)] + \
             [(i, cell, FLOWER_BAND) for i, cell in enumerate(FLOWER_CELLS)]
    for i, (scan, node, view), band in placed:
        col, row = i % GRID, i // GRID
        tile_rgb, tile_a, card = bake_cell(scan, node, view)
        y0, x0 = band + row * CELL, col * CELL
        rgb[y0:y0 + CELL, x0:x0 + CELL] = tile_rgb
        alpha[y0:y0 + CELL, x0:x0 + CELL] = tile_a
        # Atlas uv (v = 0 top): the card spans the cell's lower-left part.
        card["uv0"] = (x0 / ATLAS, (y0 + CELL) / ATLAS_HEIGHT)                       # bottom-left
        card["uv1"] = ((x0 + card["u"] * CELL) / ATLAS, (y0 + CELL - card["v"] * CELL) / ATLAS_HEIGHT)
        cards[(scan, node, view)] = card
        print(f"  {scan} {node} view {view}: {card['width']:.2f} x {card['height']:.2f} m")
    # Fern fronds: one slot each across the bottom half, scaled to fit.
    slot_w, slot_h = ATLAS // FRONDS, FLOWER_BAND - ATLAS
    for i, crop in enumerate(cut_fronds()):
        h, w = crop.shape[:2]
        scale = min(slot_w / w, slot_h / h)
        nh, nw = int(h * scale), int(w * scale)
        # Float resize per channel: the colour is linear, 8 bits would band.
        resized = np.stack([np.asarray(Image.fromarray(crop[..., k].astype(np.float32), "F")
                                       .resize((nw, nh), Image.LANCZOS)) for k in range(4)], axis=2)
        resized = np.clip(resized.astype(np.float64), 0, 1)
        x0, y0 = i * slot_w, ATLAS + slot_h - nh
        rgb[y0:y0 + nh, x0:x0 + nw] = grade(resized[..., :3], FERN_GRADE)
        alpha[y0:y0 + nh, x0:x0 + nw] = resized[..., 3]
        cards[("fern_02", i, 0)] = dict(width=nw, height=nh, root=0.5,
                                        uv0=(x0 / ATLAS, (y0 + nh) / ATLAS_HEIGHT),
                                        uv1=((x0 + nw) / ATLAS, y0 / ATLAS_HEIGHT))
        print(f"  fern frond {i}: {w} x {h} px")
    # Fill transparent texels from their neighbours so mips do not darken edges.
    filled = wa.push_pull_fill(rgb, (alpha > 0.5).astype(np.float64))
    return filled, alpha, cards


def card_quad(mesh, card, base, axis, across, height, normal_of, tint, shade):
    """One card, both sides as separate triangles with outward normals.
    base: root point; axis: unit growth direction; across: unit width axis."""
    width = height * card["width"] / card["height"]
    left = -card["root"] * width
    corners = [base + across * left, base + across * (left + width),
               base + across * (left + width) + axis * height, base + across * left + axis * height]
    (u0, v0), (u1, v1) = card["uv0"], card["uv1"]
    uvs = [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]
    shades = [shade[0], shade[0], shade[1], shade[1]]
    ids = []
    for corner, uv, s in zip(corners, uvs, shades):
        color = (tint[0] * s, tint[1] * s, tint[2] * s, WIND)
        ids.append(mesh.vertex(corner, normal_of(corner), uv, color))
    a, b, c, d = ids
    face = np.cross(corners[1] - corners[0], corners[3] - corners[0])
    # Front winding faces +face; the back pair reuses the vertices reversed.
    mesh.tri(a, b, c); mesh.tri(a, c, d)
    mesh.tri(a, c, b); mesh.tri(a, d, c)
    return face


def plant(cards, count, radius, height, size_range, seed, rise=0.5, lowest=-0.1, inner=0.3,
          sphere=0.7, base_shade=0.4, tint_jitter=0.08):
    """Crossed card pairs around an ellipsoid (radius, height) standing on y=0."""
    rng = np.random.default_rng(seed)
    mesh = wa.MeshBuilder()
    centre = np.array([0.0, height * 0.45, 0.0])
    half = np.array([radius, height * 0.55, radius])

    def normal_of(point):
        out = (point - centre) / half
        length = np.linalg.norm(out)
        return out / length if length > 1e-6 else np.array([0, 1.0, 0])

    for i in range(count):
        card = cards[i % len(cards)] if i < len(cards) else cards[rng.integers(len(cards))]
        theta = rng.uniform(0, 2 * math.pi)
        # Most cards on the upper and outer part; a few low ones close the base.
        y = rng.uniform(lowest, 1.0) if rng.uniform() < 0.85 else rng.uniform(lowest, max(lowest, 0.3))
        y = float(np.clip(math.copysign(abs(y) ** 0.8, y), lowest, 1.0))
        horizontal = math.sqrt(max(0.0, 1 - y * y))
        direction = np.array([horizontal * math.cos(theta), y, horizontal * math.sin(theta)])
        size = rng.uniform(*size_range)
        card_height = card["height"] * size
        surface = centre + direction * half
        base = centre + direction * half * inner
        axis = surface - base
        axis = axis / np.linalg.norm(axis) * (1 - rise) + np.array([0, 1.0, 0]) * rise
        axis /= np.linalg.norm(axis)
        # Tips end near the ellipsoid surface.
        base = surface - axis * card_height * 0.85
        base[1] = max(base[1], -0.05)
        side = np.cross(axis, [0, 1.0, 0])
        if np.linalg.norm(side) < 1e-3:
            side = np.array([1.0, 0, 0])
        side /= np.linalg.norm(side)
        roll = rng.uniform(0, math.pi)
        across = side * math.cos(roll) + np.cross(axis, side) * math.sin(roll)
        across2 = np.cross(axis, across)
        jitter = 1 + rng.uniform(-tint_jitter, tint_jitter, 3) * np.array([1.0, 0.6, 1.4])
        tint = jitter * rng.uniform(0.9, 1.08)
        base_level = base_shade + (1 - base_shade) * np.clip((base[1] / max(height, 1e-3)), 0, 1) * 0.6
        shade = (base_level, 1.0)
        card_quad(mesh, card, base, axis, across, card_height, lambda q: normal_lerp(q, across2, normal_of, sphere),
                  tint, shade)
        partner = card.get("partner", card)
        card_quad(mesh, partner, base, axis, across2, card_height,
                  lambda q: normal_lerp(q, across, normal_of, sphere), tint, shade)
    return mesh


def normal_lerp(point, face, normal_of, sphere):
    """Sphere-like normal, leaning toward the card's own facing."""
    out = normal_of(point)
    facing = face if np.dot(face, out) >= 0 else -face
    n = out * sphere + facing * (1 - sphere)
    return n / np.linalg.norm(n)


def fern(fronds, count, length_range, seed, segments, rise=(48.0, 72.0), droop=(55.0, 95.0),
         width_scale=1.0, base_shade=0.5, tint_jitter=0.06):
    """A rosette of fronds: each an arching strip that leaves the crown at
    `rise` degrees and droops by `droop` toward its tip. Both sides are
    separate triangles with an upward-leaning normal. Fewer segments and
    wider fronds for the far levels keep the silhouette."""
    rng = np.random.default_rng(seed)
    mesh = wa.MeshBuilder()
    up = np.array([0, 1.0, 0])
    for i in range(count):
        card = fronds[(i * 3 + seed) % len(fronds)]
        length = rng.uniform(*length_range)
        width = length * card["width"] / card["height"] * width_scale
        theta = 2 * math.pi * (i + rng.uniform(-0.3, 0.3)) / count
        outward = np.array([math.cos(theta), 0, math.sin(theta)])
        side = np.array([-math.sin(theta), 0, math.cos(theta)])
        roll = math.radians(rng.uniform(-20, 20))
        side = side * math.cos(roll) + up * math.sin(roll)
        start = math.radians(rng.uniform(*rise))
        end = start - math.radians(rng.uniform(*droop))
        tint = (1 + rng.uniform(-tint_jitter, tint_jitter, 3)) * rng.uniform(0.92, 1.06)
        (u0, v0), (u1, v1) = card["uv0"], card["uv1"]
        point = outward * 0.03
        rows = []
        for k in range(segments + 1):
            s = k / segments
            elevation = start + (end - start) * s ** 1.3
            tangent = outward * math.cos(elevation) + up * math.sin(elevation)
            surface = np.cross(side, tangent)
            surface = surface if surface[1] >= 0 else -surface
            normal = surface * 0.6 + up * 0.4
            normal /= np.linalg.norm(normal)
            shade = base_shade + (1 - base_shade) * min(1.0, s * 1.5)
            color = (tint[0] * shade, tint[1] * shade, tint[2] * shade, FERN_WIND)
            v = v0 + (v1 - v0) * s
            rows.append((mesh.vertex(point - side * width / 2, normal, (u0, v), color),
                         mesh.vertex(point + side * width / 2, normal, (u1, v), color)))
            point = point + tangent * (length / segments)
        for (a, b), (d, c) in zip(rows, rows[1:]):
            mesh.tri(a, b, c); mesh.tri(a, c, d)
            mesh.tri(a, c, b); mesh.tri(a, d, c)
    return mesh


FERN_WIND = 0.35


def pairs(cards, scan, nodes):
    """Front cards of the given nodes, each with its side view as partner."""
    result = []
    for node in nodes:
        front = dict(cards[(scan, node, 0)])
        side = cards.get((scan, node, 1))
        if side:
            front["partner"] = side
        result.append(front)
    return result


SPECIES = {
    # name: (card set, [(count, size range) per level], radius, height, extra)
    # Bushes stand clear of the knee- to waist-high meadow grass (0.7-1.05 m).
    "scan_bush": (("shrub_04", (1, 2, 3, 4)), [(90, (3.0, 3.9)), (45, (3.7, 4.7)), (18, (4.7, 6.0))],
                  0.9, 1.5, dict(seed=11)),
    "scan_bush_tall": (("shrub_04", (1, 2, 3, 4)), [(120, (3.4, 4.4)), (60, (4.2, 5.4)), (24, (5.4, 7.0))],
                       1.0, 2.3, dict(seed=23, rise=0.65)),
    "scan_shrub": (("shrub_04", (1, 2, 3, 4)), [(14, (1.2, 1.7)), (8, (1.5, 2.0)), (4, (1.9, 2.5))],
                   0.25, 0.4, dict(seed=37, rise=0.7, inner=0.1, lowest=0.0)),
    "scan_nettle": (("nettle_plant", (1, 2)), [(12, (2.2, 3.2)), (7, (2.6, 3.6)), (4, (3.2, 4.2))],
                    0.22, 0.6, dict(seed=41, rise=0.85, inner=0.05, lowest=0.2, sphere=0.5)),
    "scan_sapling": (("shrub_02", (1, 2, 3, 4)), [(5, (0.8, 1.1)), (3, (0.9, 1.2)), (2, (1.0, 1.3))],
                     0.35, 1.3, dict(seed=53, rise=0.9, inner=0.0, lowest=0.6, sphere=0.4)),
    # Lesser periwinkle: woodland ground cover with blue-violet flowers, ~0.3-0.4 m.
    "scan_periwinkle": (("periwinkle_plant", (1, 2, 3)), [(7, (1.0, 1.3)), (4, (1.2, 1.5)), (2, (1.5, 1.8))],
                        0.2, 0.35, dict(seed=59, rise=0.75, inner=0.05, lowest=0.1, sphere=0.5)),
}


# Ferns: (count, segments, width scale) per level, frond length range.
FERNS = {
    # Meadow ferns ~1.2 m across, forest ferns ~1.8 m (the generated ones
    # they replace: 1.3 and 1.7 m).
    "scan_fern": ([(12, 6, 1.0), (8, 4, 1.2), (5, 2, 1.4)], (0.6, 0.9), 61),
    "scan_fern_large": ([(16, 6, 1.0), (10, 4, 1.2), (6, 2, 1.4)], (0.85, 1.2), 67),
}


def rosette(flat_cards, side_cards, flat_count, side_count, size_range, seed, spread=0.12,
            tilt=12.0, tint_jitter=0.06):
    """A low plant: cards lying nearly flat (top views: the rosette and its
    flowers as seen from above, tilted up `tilt` degrees so they catch the
    eye from a standing height) plus a few upright side views in the middle.
    Flat cards keep an upward normal."""
    rng = np.random.default_rng(seed)
    mesh = wa.MeshBuilder()
    up = np.array([0, 1.0, 0])
    for i in range(flat_count):
        card = flat_cards[(i + seed) % len(flat_cards)]
        size = rng.uniform(*size_range)
        theta = rng.uniform(0, 2 * math.pi)
        lean = math.radians(rng.uniform(0.5, 1.0) * tilt)
        axis = np.array([math.cos(theta) * math.cos(lean), math.sin(lean), math.sin(theta) * math.cos(lean)])
        across = np.cross(up, axis)
        across /= np.linalg.norm(across)
        height = card["height"] * size
        centre = np.array([rng.uniform(-spread, spread), 0.01 + 0.015 * i, rng.uniform(-spread, spread)])
        tint = (1 + rng.uniform(-tint_jitter, tint_jitter, 3)) * rng.uniform(0.92, 1.06)
        card_quad(mesh, card, centre - axis * height / 2, axis, across, height, lambda q: up, tint, (0.85, 1.0))
    for i in range(side_count):
        card = side_cards[(i + seed) % len(side_cards)]
        size = rng.uniform(*size_range)
        theta = rng.uniform(0, math.pi)
        across = np.array([math.cos(theta), 0, math.sin(theta)])
        height = card["height"] * size
        base = np.array([rng.uniform(-spread, spread) * 0.5, -0.01, rng.uniform(-spread, spread) * 0.5])
        tint = (1 + rng.uniform(-tint_jitter, tint_jitter, 3)) * rng.uniform(0.92, 1.06)
        normal = lambda q, base=base: (lambda d: d / np.linalg.norm(d))(
            np.array([q[0] - base[0], 0.0, q[2] - base[2]]) * 0.5 + up)
        card_quad(mesh, card, base, up, across, height, normal, tint, (0.6, 1.0))
    return mesh


# Low flowering plants and herbs: (flat cards, side cards), [(flat, side, size range) per level], seed.
ROSETTES = {
    "scan_celandine": (([("celandine_01", 3, 2), ("celandine_01", 4, 2)],
                        [("celandine_01", 3, 0), ("celandine_01", 4, 0)]),
                       [(3, 2, (1.1, 1.5)), (2, 1, (1.3, 1.7)), (1, 0, (1.7, 2.1))], 71),
    "scan_dandelion": (([("dandelion_01", 1, 2), ("dandelion_01", 4, 2)],
                        [("dandelion_01", 1, 0), ("dandelion_01", 4, 0)]),
                       [(3, 2, (1.1, 1.5)), (2, 1, (1.3, 1.7)), (1, 0, (1.7, 2.1))], 73),
    "scan_weed": (([("weed_plant_02", 1, 2), ("weed_plant_02", 2, 2), ("weed_plant_02", 3, 2)],
                   [("weed_plant_02", 1, 0)]),
                  [(3, 1, (1.2, 1.6)), (2, 0, (1.4, 1.8)), (1, 0, (1.8, 2.2))], 79),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--preview", help="also write the atlas as a PNG here")
    args = parser.parse_args()
    print("baking card atlas")
    rgb, alpha, cards = build_atlas()
    WORK.mkdir(parents=True, exist_ok=True)
    png = WORK / "ScanBushCards_diff.png"
    wa.write_png(png, np.concatenate([wa.linear_to_srgb(np.clip(rgb, 0, 1)), alpha[..., None]], axis=2))
    if args.preview:
        wa.write_png(Path(args.preview), np.concatenate(
            [wa.linear_to_srgb(np.clip(rgb, 0, 1)) * alpha[..., None], alpha[..., None]], axis=2))
    texture = wa.cook(png, TEXTURE, "color", alpha_cutoff=0.4)
    for name, ((scan, nodes), levels, radius, height, extra) in SPECIES.items():
        card_set = pairs(cards, scan, nodes)
        for level, (count, size) in enumerate(levels):
            mesh = plant(card_set, count, radius, height, size, **extra)
            suffix = "" if level == 0 else f"_lod{level}"
            path = MESH_DIR / f"{name}{suffix}.mesh"
            triangles = mesh.write_native(path)
            print(f"  {path.name}: {triangles} triangles")
    for name, ((flat, side), levels, seed) in ROSETTES.items():
        for level, (flat_count, side_count, size) in enumerate(levels):
            mesh = rosette([cards[c] for c in flat], [cards[c] for c in side], flat_count, side_count,
                           size, seed)
            suffix = "" if level == 0 else f"_lod{level}"
            path = MESH_DIR / f"{name}{suffix}.mesh"
            print(f"  {path.name}: {mesh.write_native(path)} triangles")
    fronds = [cards[("fern_02", i, 0)] for i in range(FRONDS)]
    for name, (levels, length, seed) in FERNS.items():
        for level, (count, segments, width) in enumerate(levels):
            mesh = fern(fronds, count, length, seed, segments, width_scale=width)
            suffix = "" if level == 0 else f"_lod{level}"
            path = MESH_DIR / f"{name}{suffix}.mesh"
            print(f"  {path.name}: {mesh.write_native(path)} triangles")
    wa.write_material(MATERIAL, roughness=0.9, castsShadows=True, doubleSided=False,
                      useVertexColors=True, baseColorTexture=wa.asset_path(texture),
                      alphaMode="Mask", alphaCutoff=0.4, foliageTranslucency=0.8)
    print(f"  {MATERIAL.name} -> {wa.asset_path(texture)}")


if __name__ == "__main__":
    main()
