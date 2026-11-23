"""Game broadleaf trees from Poly Haven's scanned tree models (CC0).

Poly Haven's `tree_small_02` (a slender, curved deciduous tree, 4.5 m) is a
trunk (28k triangles), branches (95k) and ~30,000 alpha-cut leaves (1.94M
triangles) with separate trunk, branch and leaf texture sets. Games draw such a
crown as a "billboard cloud": the leaves are grouped into clusters and each
cluster is rendered into a card that stands where the cluster was. This tool:

  1. reads the glTF (trunk, branches, leaves as separate primitives);
  2. cooks the trunk and the branches to three-level chains with PropCook
     (quadric simplification), moved back into the tree's frame;
  3. groups the leaves into CLUSTERS clusters (k-means on leaf centres) and,
     per cluster, renders its leaves into two crossed cards: one facing out of
     the crown, one across it; each leaf goes to the card it faces most, so
     crossing the cards does not double the leaf density. The cards are
     packed into one atlas (graded toward the meadow's greens, depth-shaded
     inside each cluster). The same card set serves every mesh level: the
     cards are a few hundred triangles, and what they cost is screen area;
  4. bakes a view impostor (woodland_assets.bake_impostor) of the full tree
     for the last level;
  5. writes materials and a prefab (Trunk, Branches, Leaves parts; the trunk
     carries the impostor), then bakes crown normals and occlusion into the
     cards (cook_foliage_lighting).

Rendering two million leaf triangles with the per-triangle rasterizer in
woodland_assets would take hours, so this tool splats triangles as dense
barycentric point lattices instead (vectorised, same outputs).

Needs .local/polyhaven/tree_small_02/gltf (the 2k glTF and its textures,
plus textures/tree_small_02_leaves_alpha_2k.png), PropCook, TextureCook, scipy.

Usage: python tools/scan_tree_cards.py [--preview out.png] [--no-impostor]
"""

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import woodland_assets as wa  # noqa: E402

ROOT = wa.ROOT
SOURCE = ROOT / ".local" / "polyhaven" / "tree_small_02" / "gltf"
WORK = ROOT / ".local" / "tree-gen"
MODELS = wa.ASSETS / "Models" / "Woodland" / "Broadleaf"
MESHES = MODELS / "Meshes"
TEXTURES = MODELS / "Textures"
MATERIALS = MODELS / "Materials"
PREFABS = wa.ASSETS / "Prefabs" / "Woodland" / "Broadleaf"

CLUSTERS = 160
ATLAS = 4096
SUPERSAMPLE = 2
LEAF_CUTOFF = 0.4
IMPOSTOR_CUTOFF = 0.4
IMPOSTOR_TINT = [0.58, 0.58, 0.54]
CARD_GRADE = 0.6
TREE_WIND = 0.1
FOLIAGE_VARIATION = 0.55
# (triangles per level) for PropCook; each level stops early rather than
# exceed its error budget (a share of the bounding diagonal, doubling per level).
TRUNK_LODS = (4000, 1200, 300)
BRANCH_LODS = (5000, 1200, 300)
# Pieces smaller than this (m) are left out per level: twigs inside the crown.
BRANCH_MIN_EXTENTS = (0.25, 0.4, 0.8)
MAX_ERROR = 0.002


# --- glTF -------------------------------------------------------------------

def load_gltf():
    gltf = json.loads((SOURCE / "tree_small_02_2k.gltf").read_text())
    data = (SOURCE / "tree_small_02.bin").read_bytes()
    types = {5126: np.float32, 5125: np.uint32, 5123: np.uint16}
    widths = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}

    def accessor(i):
        a = gltf["accessors"][i]
        view = gltf["bufferViews"][a["bufferView"]]
        offset = view.get("byteOffset", 0) + a.get("byteOffset", 0)
        return np.frombuffer(data, types[a["componentType"]], a["count"] * widths[a["type"]],
                             offset).reshape(a["count"], -1).astype(np.float64 if a["componentType"] == 5126
                                                                     else np.int64)

    parts = {}
    for prim in gltf["meshes"][0]["primitives"]:
        material = gltf["materials"][prim["material"]]
        name = material["name"].replace("tree_small_02_", "")
        attrs = prim["attributes"]
        uv_set = material["pbrMetallicRoughness"]["baseColorTexture"].get("texCoord", 0)
        uv = accessor(attrs[f"TEXCOORD_{uv_set}"])
        transform = material["pbrMetallicRoughness"]["baseColorTexture"].get("extensions", {}).get(
            "KHR_texture_transform")
        if transform:
            uv = uv * np.array(transform.get("scale", [1, 1])) + np.array(transform.get("offset", [0, 0]))
        parts[name] = dict(p=accessor(attrs["POSITION"]), n=accessor(attrs["NORMAL"]), uv=uv,
                           tri=accessor(prim["indices"]).reshape(-1, 3))
    return parts


def texture_rgba(stem, alpha_png=None):
    color = np.asarray(Image.open(SOURCE / "textures" / f"{stem}_2k.jpg").convert("RGB"), np.float64) / 255
    if alpha_png:
        a = np.asarray(Image.open(SOURCE / "textures" / alpha_png), np.float64)
        a = a / (65535.0 if a.max() > 255 else 255.0)
        if a.ndim == 3:
            a = a[..., 0]
    else:
        a = np.ones(color.shape[:2])
    return wa.Texture(np.concatenate([wa.srgb_to_linear(color), a[..., None]], axis=2))


# --- point-splat rasterizer ---------------------------------------------------

def rasterize_splat(surfaces, right, up, forward, origin_uv, scale, size):
    """Same contract as woodland_assets.rasterize (orthographic; albedo linear,
    alpha, world normal, depth with larger = nearer; row 0 at the smallest
    dot(p, up)), but each triangle is drawn as a barycentric lattice of points
    spaced at most 2/3 pixel, all triangles of a lattice size at once."""
    width, height = size
    albedo = np.zeros((height * width, 3))
    normal = np.zeros((height * width, 3))
    alpha = np.zeros(height * width)
    depth = np.full(height * width, -np.inf)
    right, up, forward = (np.asarray(a, dtype=np.float64) for a in (right, up, forward))
    for s in surfaces:
        if not len(s.tri):
            continue
        sx = (s.p @ right - origin_uv[0]) * scale
        sy = (s.p @ up - origin_uv[1]) * scale
        sz = s.p @ forward
        tri = s.tri
        xa, xb, xc = sx[tri[:, 0]], sx[tri[:, 1]], sx[tri[:, 2]]
        ya, yb, yc = sy[tri[:, 0]], sy[tri[:, 1]], sy[tri[:, 2]]
        # Cull triangles off the image.
        on = ((np.maximum(np.maximum(xa, xb), xc) >= 0) & (np.minimum(np.minimum(xa, xb), xc) < width) &
              (np.maximum(np.maximum(ya, yb), yc) >= 0) & (np.minimum(np.minimum(ya, yb), yc) < height))
        longest = np.maximum(np.maximum(np.hypot(xb - xa, yb - ya), np.hypot(xc - xb, yc - yb)),
                             np.hypot(xa - xc, ya - yc))
        steps = np.clip(np.ceil(longest * 1.5), 1, 96).astype(np.int64)
        for n in np.unique(steps[on]):
            chosen = np.nonzero(on & (steps == n))[0]
            i, j = np.meshgrid(np.arange(n + 1), np.arange(n + 1), indexing="ij")
            keep = (i + j) <= n
            bary = np.stack([i[keep], j[keep], n - i[keep] - j[keep]], axis=1) / n   # (m, 3)
            # Chunks of at most ~4M points.
            per = max(1, 4_000_000 // len(bary))
            for c0 in range(0, len(chosen), per):
                t = tri[chosen[c0:c0 + per]]                                          # (k, 3)
                def interp(values):
                    v = values[t]                                                     # (k, 3, d)
                    return np.einsum("mj,kjd->kmd", bary, v).reshape(-1, v.shape[-1])
                px = np.einsum("mj,kj->km", bary, sx[t]).ravel()
                py = np.einsum("mj,kj->km", bary, sy[t]).ravel()
                pz = np.einsum("mj,kj->km", bary, sz[t]).ravel()
                ix = np.floor(px).astype(np.int64)
                iy = np.floor(py).astype(np.int64)
                inside = (ix >= 0) & (ix < width) & (iy >= 0) & (iy < height)
                col = np.broadcast_to(s.base, (len(px), 3)).copy()
                if s.texture is not None:
                    texel = s.texture.sample(interp(s.uv))
                    col *= texel[:, :3]
                    inside &= texel[:, 3] >= s.cutoff
                if s.use_vertex_colors:
                    col *= interp(s.c[:, :3])
                if not inside.any():
                    continue
                k = (iy * width + ix)[inside]
                z = pz[inside]
                col = col[inside]
                nrm = wa.normalize(interp(s.n)[inside])
                # Nearest point per pixel, then against what is already there.
                order = np.lexsort((z, k))
                k, z, col, nrm = k[order], z[order], col[order], nrm[order]
                last = np.r_[k[1:] != k[:-1], True]
                k, z, col, nrm = k[last], z[last], col[last], nrm[last]
                nearer = z > depth[k]
                k, z, col, nrm = k[nearer], z[nearer], col[nearer], nrm[nearer]
                flip = (nrm @ forward) < 0
                nrm[flip] *= -1
                depth[k] = z
                albedo[k] = col
                normal[k] = nrm
                alpha[k] = 1.0
    return (albedo.reshape(height, width, 3), alpha.reshape(height, width),
            normal.reshape(height, width, 3), depth.reshape(height, width))


# --- colour ------------------------------------------------------------------

def grade(albedo, amount):
    """Toward the meadow's greens (as scan_bush_cards.grade): the scan's leaves
    are a cool blue-green. Luminance is kept; stems and twigs stay brown."""
    weights = np.array([0.2126, 0.7152, 0.0722])
    lum = albedo @ weights
    leafiness = np.clip((albedo[..., 1] - albedo[..., 0]) / np.maximum(albedo[..., 1], 1e-6) * 6.0 + 0.2, 0, 1)
    leaf = np.array([0.36, 1.0, 0.09]) / (np.array([0.36, 1.0, 0.09]) @ weights)
    graded = lum[..., None] * leaf
    return albedo + (graded - albedo) * (amount * leafiness)[..., None]


# --- leaf clusters -----------------------------------------------------------

def leaf_components(leaves):
    import scipy.sparse as sp
    import scipy.sparse.csgraph as cg
    tri = leaves["tri"]
    count = len(leaves["p"])
    edges = np.concatenate([tri[:, [0, 1]], tri[:, [1, 2]]])
    graph = sp.coo_matrix((np.ones(len(edges)), (edges[:, 0], edges[:, 1])), shape=(count, count))
    _, label = cg.connected_components(graph, directed=False)
    return label


def make_clusters(leaves, seed=7):
    """k-means on leaf centres; returns per-triangle cluster ids and cluster centres."""
    from scipy.cluster.vq import kmeans2
    label = leaf_components(leaves)
    count = label.max() + 1
    centres = np.zeros((count, 3))
    np.add.at(centres, label, leaves["p"])
    centres /= np.bincount(label, minlength=count)[:, None]
    rng = np.random.default_rng(seed)
    init = centres[rng.choice(count, CLUSTERS, replace=False)]
    means, assign = kmeans2(centres, init, iter=30, minit="matrix")
    tri_cluster = assign[label[leaves["tri"][:, 0]]]
    return tri_cluster, means, label, assign


def build_cards(leaves, leaf_texture, tri_cluster, means):
    """Two crossed cards per cluster; returns card records (geometry + pixels)."""
    p, n, uv, tri = leaves["p"], leaves["n"], leaves["uv"], leaves["tri"]
    crown_axis = np.array([np.median(p[:, 0]), 0.0, np.median(p[:, 2])])
    tri_normal = wa.normalize(n[tri].sum(axis=1))
    cards = []
    up_world = np.array([0.0, 1.0, 0.0])
    for k in range(len(means)):
        members = np.nonzero(tri_cluster == k)[0]
        if len(members) == 0:
            continue
        centre = means[k]
        outward = centre - crown_axis
        outward[1] *= 0.35                    # mostly horizontal
        if np.linalg.norm(outward) < 1e-3:
            outward = np.array([1.0, 0, 0])
        outward = wa.normalize(outward)
        across = wa.normalize(np.cross(up_world, outward))
        # Each leaf goes to the card it faces most (either side).
        facing_out = np.abs(tri_normal[members] @ outward) >= np.abs(tri_normal[members] @ across)
        for forward, chosen in ((outward, members[facing_out]), (across, members[~facing_out])):
            if len(chosen) == 0:
                continue
            right = wa.normalize(np.cross(up_world, forward))
            up = np.cross(forward, right)
            used = np.unique(tri[chosen])
            pts = p[used]
            lo = np.array([(pts @ right).min(), (pts @ up).min()])
            hi = np.array([(pts @ right).max(), (pts @ up).max()])
            margin = 0.02
            lo -= margin; hi += margin
            plane = float(np.median(pts @ forward))
            cards.append(dict(cluster=k, forward=forward, right=right, up=up, lo=lo, hi=hi, plane=plane,
                              tris=chosen))
    return cards


def pack(cards, atlas):
    """Shelf packing at one scale (pixels per metre) as large as fits."""
    def try_scale(scale):
        x = y = shelf = 0
        placed = []
        for c in sorted(range(len(cards)), key=lambda i: -(cards[i]["hi"][1] - cards[i]["lo"][1])):
            w = int(math.ceil((cards[c]["hi"][0] - cards[c]["lo"][0]) * scale)) + 2
            h = int(math.ceil((cards[c]["hi"][1] - cards[c]["lo"][1]) * scale)) + 2
            if x + w > atlas:
                x, y, shelf = 0, y + shelf, 0
            if y + h > atlas or w > atlas:
                return None
            placed.append((c, x, y, w, h))
            x += w
            shelf = max(shelf, h)
        return placed
    lo, hi = 10.0, 4000.0
    best = None
    for _ in range(30):
        mid = (lo + hi) / 2
        result = try_scale(mid)
        if result:
            lo, best = mid, result
        else:
            hi = mid
    return lo, best


def render_atlas(leaves, leaf_texture, cards):
    scale, placed = pack(cards, ATLAS)
    print(f"  {len(cards)} cards at {scale:.0f} px/m")
    rgb = np.zeros((ATLAS, ATLAS, 3))
    alpha = np.zeros((ATLAS, ATLAS))
    ss = SUPERSAMPLE
    for c, x0, y0, w, h in placed:
        card = cards[c]
        surface = wa.Surface(leaves["p"], leaves["n"], leaves["uv"], np.ones((len(leaves["p"]), 4)),
                             leaves["tri"][card["tris"]], texture=leaf_texture, alpha_cutoff=0.5,
                             use_vertex_colors=False)
        # One pixel of border around the card's bounds.
        origin = card["lo"] - 1.0 / scale
        albedo, a, _, depth = rasterize_splat([surface], card["right"], card["up"], card["forward"],
                                              origin, scale * ss, (w * ss, h * ss))
        covered = a > 0
        if covered.any():
            d = depth[covered]
            near, far = np.percentile(d, 95), np.percentile(d, 5)
            front = np.clip((depth - far) / max(near - far, 1e-6), 0, 1)
            albedo *= (0.75 + 0.25 * front)[..., None]
        albedo = grade(albedo, CARD_GRADE)
        a_small = wa.downsample(a, ss)
        c_small = wa.downsample(albedo * a[..., None], ss) / np.maximum(a_small, 1e-6)[..., None]
        # Row 0 of the render is the card's bottom; the atlas has v = 0 at the top.
        rgb[y0:y0 + h, x0:x0 + w] = np.flipud(c_small)
        alpha[y0:y0 + h, x0:x0 + w] = np.flipud(a_small)
        card["uv"] = (x0 / ATLAS, (y0 + h) / ATLAS, (x0 + w) / ATLAS, y0 / ATLAS)   # u0, v_bottom, u1, v_top
        card["origin"] = origin
        card["size"] = (w / scale, h / scale)
    filled = wa.push_pull_fill(rgb, (alpha > 0.5).astype(np.float64))
    return filled, alpha


def card_mesh(cards, offsets):
    """offsets: per-cluster translation (a variant's stretch of the crown)."""
    mesh = wa.MeshBuilder()
    for card in cards:
        right, up, forward = card["right"], card["up"], card["forward"]
        (u0, vb, u1, vt) = card["uv"]
        ox, oy = card["origin"]
        w, h = card["size"]
        base = forward * card["plane"] + offsets[card["cluster"]]
        corners = [base + right * ox + up * oy, base + right * (ox + w) + up * oy,
                   base + right * (ox + w) + up * (oy + h), base + right * ox + up * (oy + h)]
        uvs = [(u0, vb), (u1, vb), (u1, vt), (u0, vt)]
        ids = [mesh.vertex(q, forward, t, (1, 1, 1, 1)) for q, t in zip(corners, uvs)]
        mesh.quad(*ids)
    return mesh


# --- trunk and branches --------------------------------------------------------

def write_obj(path, part):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w") as f:
        for q in part["p"]:
            f.write(f"v {q[0]:.6f} {q[1]:.6f} {q[2]:.6f}\n")
        for q in part["n"]:
            f.write(f"vn {q[0]:.6f} {q[1]:.6f} {q[2]:.6f}\n")
        # PropCook flips V (FBX/OBJ convention); glTF's v already runs down.
        for t in part["uv"]:
            f.write(f"vt {t[0]:.6f} {1.0 - t[1]:.6f}\n")
        for a, b, c in part["tri"] + 1:
            f.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")


def find_tool(name):
    for config in ("RelWithDebInfo", "Release", "Debug"):
        path = ROOT / "build" / config / f"{name}.exe"
        if path.exists():
            return path
    raise SystemExit(f"{name} is not built: cmake --build build --config RelWithDebInfo --target {name}")


def piece_extents(part):
    """Per-triangle extent (largest bounding-box side) of the connected piece
    it belongs to, welding vertices split at uv seams."""
    import scipy.sparse as sp
    import scipy.sparse.csgraph as cg
    _, weld = np.unique(np.round(part["p"], 5), axis=0, return_inverse=True)
    tri = weld.ravel()[part["tri"]]
    count = tri.max() + 1
    edges = np.concatenate([tri[:, [0, 1]], tri[:, [1, 2]]])
    _, label = cg.connected_components(sp.coo_matrix(
        (np.ones(len(edges)), (edges[:, 0], edges[:, 1])), shape=(count, count)), directed=False)
    piece = label[tri[:, 0]]
    corners = part["p"][part["tri"]]
    lo = np.full((piece.max() + 1, 3), np.inf)
    hi = np.full((piece.max() + 1, 3), -np.inf)
    np.minimum.at(lo, piece, corners.min(axis=1))
    np.maximum.at(hi, piece, corners.max(axis=1))
    return (hi - lo).max(axis=1)[piece]


def cook_chain(part, name, lods, min_extents=None):
    """PropCook chain, moved back to the tree's frame (PropCook centres the
    pivot in x/z and puts it at the base). Twigs are open tubes whose borders
    PropCook keeps, so they cannot be simplified away: min_extents drops, per
    level, every piece smaller than the given size (they vanish in the crown
    cards at that level's distance)."""
    extents = piece_extents(part) if min_extents else None
    paths = []
    for level, target in enumerate(lods):
        chosen = part
        if min_extents and min_extents[level] > 0:
            keep = extents >= min_extents[level]
            chosen = dict(part, tri=part["tri"][keep])
        obj = WORK / f"{name}_LOD{level}.obj"
        write_obj(obj, chosen)
        temp = f"{name}_level{level}"
        subprocess.run([str(find_tool("PropCook")), str(obj), str(MESHES), "--name", temp,
                        "--lods", str(target), "--sink", "0", "--max-error", f"{MAX_ERROR * 2 ** level:g}"],
                       check=True)
        used = chosen["p"][np.unique(chosen["tri"])]
        lo, hi = used.min(axis=0), used.max(axis=0)
        pivot = np.array([(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2])
        path = MESHES / f"{name}_LOD{level}.mesh"
        path.unlink(missing_ok=True)
        (MESHES / f"{temp}_LOD0.mesh").rename(path)
        raw = bytearray(path.read_bytes())
        _, _, count, _ = np.frombuffer(raw[:16], np.uint32)
        data = np.frombuffer(raw, np.float32, int(count) * wa.VERTEX_FLOATS, 16).reshape(-1, wa.VERTEX_FLOATS).copy()
        data[:, 0:3] += pivot.astype(np.float32)
        raw[16:16 + data.nbytes] = data.tobytes()
        path.write_bytes(bytes(raw))
        paths.append(path)
    return paths


# --- main --------------------------------------------------------------------

# Variants: (x/z, y) stretch about the trunk base. Trunk and branches stretch;
# leaf clusters (and their cards) only move with their centres, so the
# variants share one card atlas and keep the leaves' shape.
VARIANTS = {
    "Broadleaf_slender_01": (1.0, 1.0),
    # Narrow, upright crown on a taller stem (hornbeam / young lime habit).
    "Broadleaf_upright_01": (0.72, 1.35),
}


def stretched(part, sxz, sy):
    scale = np.array([sxz, sy, sxz])
    return dict(part, p=part["p"] * scale, n=wa.normalize(part["n"] / scale))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--preview", help="also write the card atlas as a PNG here")
    parser.add_argument("--no-impostor", action="store_true")
    parser.add_argument("--only", help="one variant")
    args = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    parts = load_gltf()
    # Trunk base at the origin.
    trunk = parts["trunk"]
    base = trunk["p"][trunk["p"][:, 1] < trunk["p"][:, 1].min() + 0.15]
    shift = np.array([base[:, 0].mean(), 0.0, base[:, 2].mean()])
    for part in parts.values():
        part["p"] = part["p"] - shift
    leaves = parts["leaves"]
    print(f"leaves: {len(leaves['tri'])} triangles")

    print("crown cards")
    leaf_texture = texture_rgba("tree_small_02_leaves_diff", "tree_small_02_leaves_alpha_2k.png")
    tri_cluster, means, label, assign = make_clusters(leaves)
    vertex_cluster = assign[label]
    cards = build_cards(leaves, leaf_texture, tri_cluster, means)
    rgb, alpha = render_atlas(leaves, leaf_texture, cards)
    family = "Broadleaf"
    png = WORK / f"{family}_Leaves_Color.png"
    wa.write_png(png, np.concatenate([wa.linear_to_srgb(np.clip(rgb, 0, 1)), alpha[..., None]], axis=2))
    if args.preview:
        wa.write_png(Path(args.preview), np.concatenate(
            [wa.linear_to_srgb(np.clip(rgb, 0, 1)) * alpha[..., None], alpha[..., None]], axis=2))
    wa.cook(png, TEXTURES / f"{family}_Leaves_Color.ktx2", "color", alpha_cutoff=LEAF_CUTOFF)

    print("bark textures")
    # TextureCook takes PNG sources.
    for key, stem in (("Trunk", "tree_small_02"), ("Branches", "tree_small_02_branch")):
        for suffix, usage in (("diff", "color"), ("nor_gl", "normal")):
            src = WORK / f"{stem}_{suffix}.png"
            Image.open(SOURCE / "textures" / f"{stem}_{suffix}_2k.jpg").convert("RGB").save(src)
            wa.cook(src, TEXTURES / f"{family}_{key}_{'Color' if usage == 'color' else 'Normal'}.ktx2", usage)

    tex = lambda name: wa.asset_path(TEXTURES / name)
    leaf_material = wa.write_material(
        MATERIALS / f"{family}_Leaves.material", baseColorTexture=tex(f"{family}_Leaves_Color.ktx2"),
        useVertexColors=True, alphaMode="Mask", alphaCutoff=LEAF_CUTOFF, doubleSided=True, roughness=0.8,
        alphaCoverageTransmission=0.15, alphaEdgeFade=0.75, reflectionStrength=0.12,
        foliageColorVariation=FOLIAGE_VARIATION, foliageTranslucency=0.9)
    bark_materials = {key: wa.write_material(
        MATERIALS / f"{family}_{key}.material",
        baseColorTexture=tex(f"{family}_{key}_Color.ktx2"), normalTexture=tex(f"{family}_{key}_Normal.ktx2"),
        useVertexColors=False, roughness=0.9, normalScale=1.0) for key in ("Trunk", "Branches")}
    trunk_tex = texture_rgba("tree_small_02_diff")
    branch_tex = texture_rgba("tree_small_02_branch_diff")
    graded_leaves = wa.Texture(leaf_texture.data.copy())
    graded_leaves.data[..., :3] = grade(graded_leaves.data[..., :3], CARD_GRADE)
    wa.rasterize = rasterize_splat

    for name, (sxz, sy) in VARIANTS.items():
        if args.only and name != args.only:
            continue
        print(name)
        scale = np.array([sxz, sy, sxz])
        offsets = means * scale - means                       # per cluster
        trunk_paths = cook_chain(stretched(parts["trunk"], sxz, sy), f"{name}_Trunk", TRUNK_LODS)
        branch_paths = cook_chain(stretched(parts["branches"], sxz, sy), f"{name}_Branches", BRANCH_LODS,
                                  BRANCH_MIN_EXTENTS)
        leaves_mesh = MESHES / f"{name}_Leaves.mesh"
        print(f"  {leaves_mesh.name}: {card_mesh(cards, offsets).write_native(leaves_mesh)} triangles")

        impostor_mesh = MESHES / f"{name}_Impostor.mesh"
        impostor_material = wa.write_material(
            MATERIALS / f"{name}_Impostor.material",
            baseColorTexture=tex(f"{name}_Impostor_Color.ktx2"), normalTexture=tex(f"{name}_Impostor_Normal.ktx2"),
            # The cards are darkened inside the crown by cook_foliage_lighting
            # (occlusion ~0.54 on average); the flat impostor matches that here.
            baseColor=IMPOSTOR_TINT,
            useVertexColors=False, alphaMode="Mask", alphaCutoff=IMPOSTOR_CUTOFF, doubleSided=True,
            roughness=0.86, normalScale=0.55, alphaEdgeFade=1.0, impostorBillboard=True,
            foliageColorVariation=FOLIAGE_VARIATION)
        if not args.no_impostor or not impostor_mesh.exists():
            print("  impostor")
            ones = lambda part: np.ones((len(part["p"]), 4))
            surfaces = []
            for key, texture in (("trunk", trunk_tex), ("branches", branch_tex)):
                part = stretched(parts[key], sxz, sy)
                surfaces.append(wa.Surface(part["p"], part["n"], part["uv"], ones(part), part["tri"],
                                           texture=texture, use_vertex_colors=False))
            surfaces.append(wa.Surface(leaves["p"] + offsets[vertex_cluster], leaves["n"], leaves["uv"],
                                       ones(leaves), leaves["tri"], texture=graded_leaves, alpha_cutoff=0.5,
                                       use_vertex_colors=False))
            color_png, normal_png, _ = wa.bake_impostor(surfaces, name, WORK, impostor_mesh,
                                                        atlas_size=1024, supersample=2)
            wa.cook(color_png, TEXTURES / f"{name}_Impostor_Color.ktx2", "color", alpha_cutoff=IMPOSTOR_CUTOFF)
            wa.cook(normal_png, TEXTURES / f"{name}_Impostor_Normal.ktx2", "normal")

        leaves_asset = wa.asset_path(leaves_mesh)
        lod1, lod2 = 60, 220
        wa.write_prefab(PREFABS / f"{name}.prefab", name, [
            dict(name="Trunk", mesh=wa.asset_path(trunk_paths[0]),
                 lods=[wa.asset_path(trunk_paths[1]), wa.asset_path(trunk_paths[2]), wa.asset_path(impostor_mesh)],
                 material=wa.engine_path(bark_materials["Trunk"]),
                 lod_materials=["", "", wa.engine_path(impostor_material)], lod1=lod1, lod2=lod2,
                 wind=TREE_WIND, tree_wind=True),
            dict(name="Branches", mesh=wa.asset_path(branch_paths[0]),
                 lods=[wa.asset_path(branch_paths[1]), wa.asset_path(branch_paths[2])],
                 material=wa.engine_path(bark_materials["Branches"]), lod1=lod1, lod2=lod2,
                 wind=TREE_WIND, tree_wind=True),
            dict(name="Leaves", mesh=leaves_asset, lods=[leaves_asset, leaves_asset],
                 material=wa.engine_path(leaf_material), lod1=lod1, lod2=lod2, wind=TREE_WIND, tree_wind=True),
        ])
    # Crown normals and canopy occlusion for the freshly written card meshes.
    import cook_foliage_lighting
    cook_foliage_lighting.cook_prefabs([PREFABS])


if __name__ == "__main__":
    main()
