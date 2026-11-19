"""Shared helpers for the generated woodland assets (conifers, boulders,
forest clusters, sky sprites).

Conventions (same as the imported oak and rock packs):
  * metres, Y up, ground pivot at the origin;
  * native .mesh files (MeshSerialization.cpp, version 7): 264-byte
    AnimationVertex records, uint32 indices;
  * texture v = 0 is the top row of the image; tangent w orients
    cross(N, T) along +v (PropCook's BuildEngineVertices), and tangent-space
    normal maps decode as n = rgb * 2 - 1 against (T, cross(N, T) * w, N);
  * vertex colours are linear and multiply the base colour texture when the
    material sets useVertexColors;
  * prefabs carry their own levels (Mesh.lodMeshes / Material.lodMaterials,
    PrefabLod.h); an empty entry hides a part at that level;
  * colour textures for alpha-tested foliage are cooked to KTX2 by
    TextureCook --alpha-cutoff (push-pull fill + coverage-preserving mips),
    so cutouts keep their coverage with distance instead of thinning.

Sources and previews go to .local/woodland-gen; only the cooked outputs are
assets.
"""
from __future__ import annotations

import math
import os
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Engine" / "Core" / "Assets"
WORK = ROOT / ".local" / "woodland-gen"

NATIVE_MESH_MAGIC = 0x4853454D
NATIVE_MESH_VERSION = 7
VERTEX_FLOATS = 66  # sizeof(AnimationVertex) / 4


def asset_path(path: Path) -> str:
    """Engine asset path ("Assets/...") of a file under Engine/Core/Assets."""
    return "Assets/" + Path(path).resolve().relative_to(ASSETS.resolve()).as_posix()


def engine_path(path: Path) -> str:
    """Repository-relative path ("Engine/Core/Assets/...") used by prefabs for materials."""
    return Path(path).resolve().relative_to(ROOT.resolve()).as_posix()


# --- geometry -------------------------------------------------------------

def normalize(v, axis=-1):
    v = np.asarray(v, dtype=np.float64)
    n = np.linalg.norm(v, axis=axis, keepdims=True)
    return v / np.maximum(n, 1e-12)


class MeshBuilder:
    """Triangle soup with shared vertices; positions, normals, uvs, colours."""

    def __init__(self):
        self.p: list = []
        self.n: list = []
        self.uv: list = []
        self.c: list = []
        self.t: list = []  # optional explicit tangents (x, y, z, w) or None
        self.idx: list = []

    def vertex(self, position, normal, uv, color=(1, 1, 1, 1), tangent=None):
        self.p.append(tuple(float(x) for x in position))
        self.n.append(tuple(float(x) for x in normal))
        self.uv.append((float(uv[0]), float(uv[1])))
        if len(color) == 3:
            color = (color[0], color[1], color[2], 1.0)
        self.c.append(tuple(float(x) for x in color))
        self.t.append(None if tangent is None else tuple(float(x) for x in tangent))
        return len(self.p) - 1

    def tri(self, a, b, c):
        self.idx.append((a, b, c))

    def quad(self, a, b, c, d):
        """a-b bottom edge, d-c top edge, counter-clockwise seen from the front."""
        self.idx.append((a, b, c))
        self.idx.append((a, c, d))

    def append(self, other: "MeshBuilder"):
        base = len(self.p)
        self.p += other.p; self.n += other.n; self.uv += other.uv
        self.c += other.c; self.t += other.t
        self.idx += [(a + base, b + base, c + base) for a, b, c in other.idx]

    @property
    def triangle_count(self):
        return len(self.idx)

    def arrays(self):
        p = np.array(self.p, dtype=np.float64).reshape(-1, 3)
        n = normalize(np.array(self.n, dtype=np.float64).reshape(-1, 3))
        uv = np.array(self.uv, dtype=np.float64).reshape(-1, 2)
        c = np.array(self.c, dtype=np.float64).reshape(-1, 4)
        idx = np.array(self.idx, dtype=np.int64).reshape(-1, 3)
        return p, n, uv, c, idx

    def tangents(self):
        """UV-derived tangents (PropCook convention), explicit ones kept."""
        p, n, uv, _, idx = self.arrays()
        tan = np.zeros_like(p)
        bit = np.zeros_like(p)
        if len(idx):
            a, b, c = p[idx[:, 0]], p[idx[:, 1]], p[idx[:, 2]]
            ta, tb, tc = uv[idx[:, 0]], uv[idx[:, 1]], uv[idx[:, 2]]
            e1, e2 = b - a, c - a
            du1, dv1 = (tb - ta)[:, 0:1], (tb - ta)[:, 1:2]
            du2, dv2 = (tc - ta)[:, 0:1], (tc - ta)[:, 1:2]
            det = du1 * dv2 - du2 * dv1
            ok = (np.abs(det) > 1e-20)[:, 0]
            r = np.where(np.abs(det) > 1e-20, 1.0 / np.where(det == 0, 1, det), 0.0)
            t = (e1 * dv2 - e2 * dv1) * r
            s = (e2 * du1 - e1 * du2) * r
            t[~ok] = 0; s[~ok] = 0
            for k in range(3):
                np.add.at(tan, idx[:, k], t)
                np.add.at(bit, idx[:, k], s)
        o = tan - n * np.sum(tan * n, axis=1, keepdims=True)
        bad = np.linalg.norm(o, axis=1) < 1e-9
        # Fallback: any vector perpendicular to the normal.
        alt = np.cross(n, np.where(np.abs(n[:, 1:2]) < 0.9, [[0, 1, 0]], [[1, 0, 0]]))
        o[bad] = alt[bad]
        o = normalize(o)
        w = np.where(np.sum(np.cross(n, o) * bit, axis=1) < 0, -1.0, 1.0)
        result = np.concatenate([o, w[:, None]], axis=1)
        for i, explicit in enumerate(self.t):
            if explicit is not None:
                result[i] = explicit
        return result

    def write_native(self, path: Path):
        p, n, uv, c, idx = self.arrays()
        tangents = self.tangents()
        data = np.zeros((len(p), VERTEX_FLOATS), dtype=np.float32)
        data[:, 0:3] = p
        data[:, 3:6] = n
        data[:, 6:8] = uv
        data[:, 8:12] = tangents
        data[:, 12:14] = uv          # uv1
        data[:, 14:18] = c
        data[:, 18:22] = 0           # joints0
        if not np.all(np.isfinite(data)):
            raise ValueError(f"non-finite vertex data in {path}")
        indices = idx.astype(np.uint32).reshape(-1)
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "wb") as f:
            f.write(struct.pack("<4I", NATIVE_MESH_MAGIC, NATIVE_MESH_VERSION, len(p), len(indices)))
            f.write(data.tobytes())
            f.write(indices.tobytes())
        return len(idx)


def read_native_mesh(path: Path):
    """positions, normals, uvs, colours, triangle indices of a native .mesh."""
    raw = Path(path).read_bytes()
    magic, version, count, index_count = struct.unpack_from("<4I", raw, 0)
    if magic != NATIVE_MESH_MAGIC:
        raise ValueError(f"{path}: not a native mesh")
    vertices = np.frombuffer(raw, dtype=np.float32, count=count * VERTEX_FLOATS, offset=16)
    vertices = vertices.reshape(count, VERTEX_FLOATS).astype(np.float64)
    if index_count:
        idx = np.frombuffer(raw, dtype=np.uint32, count=index_count, offset=16 + count * 264)
        idx = idx.astype(np.int64).reshape(-1, 3)
    else:
        idx = np.arange(count, dtype=np.int64).reshape(-1, 3)
    return vertices[:, 0:3], vertices[:, 3:6], vertices[:, 6:8], vertices[:, 14:18], idx


# --- images ---------------------------------------------------------------

def write_png(path: Path, image: np.ndarray):
    """image: HxWx{1,3,4} floats in 0..1 (written as stored: row 0 = top)."""
    image = np.asarray(image)
    if image.ndim == 2:
        image = image[..., None]
    channels = image.shape[2]
    color_type = {1: 0, 3: 2, 4: 6}[channels]
    data = (np.clip(image, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    raw = b"".join(b"\x00" + row.tobytes() for row in data)
    chunk = lambda tag, body: (struct.pack(">I", len(body)) + tag + body +
                               struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(
        ">IIBBBBB", data.shape[1], data.shape[0], 8, color_type, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def read_png(path: Path) -> np.ndarray:
    """8-bit non-interlaced PNG (grey, RGB, palette, grey+alpha, RGBA) -> HxWx4 floats."""
    raw = Path(path).read_bytes()
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, palette, alpha_table = 8, [], None, None
    width = height = depth = color_type = interlace = 0
    while pos < len(raw):
        length, = struct.unpack(">I", raw[pos:pos + 4])
        tag = raw[pos + 4:pos + 8]
        body = raw[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE":
            palette = np.frombuffer(body, dtype=np.uint8).reshape(-1, 3)
        elif tag == b"tRNS":
            alpha_table = np.frombuffer(body, dtype=np.uint8)
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"IEND":
            break
    if depth != 8 or interlace:
        raise ValueError(f"{path}: only 8-bit non-interlaced PNGs are supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
    data = np.frombuffer(zlib.decompress(b"".join(idat)), dtype=np.uint8)
    stride = width * channels
    rows = data.reshape(height, stride + 1)
    out = np.zeros((height, stride), dtype=np.int32)
    previous = np.zeros(stride, dtype=np.int32)
    for y in range(height):
        kind = rows[y, 0]
        line = rows[y, 1:].astype(np.int32)
        if kind == 0:
            current = line
        elif kind == 1:
            current = np.cumsum(line.reshape(width, channels), axis=0).reshape(-1) & 255
        elif kind == 2:
            current = (line + previous) & 255
        else:
            current = np.zeros(stride, dtype=np.int32)
            for x in range(stride):
                left = current[x - channels] if x >= channels else 0
                up = previous[x]
                if kind == 1:
                    value = line[x] + left
                elif kind == 3:
                    value = line[x] + ((left + up) >> 1)
                else:  # Paeth
                    up_left = previous[x - channels] if x >= channels else 0
                    p = left + up - up_left
                    pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                    value = line[x] + (left if pa <= pb and pa <= pc else up if pb <= pc else up_left)
                current[x] = value & 255
        out[y] = current
        previous = current
    pixels = out.reshape(height, width, channels).astype(np.uint8)
    if color_type == 3:
        rgb = palette[pixels[..., 0]]
        a = (alpha_table[pixels[..., 0]] if alpha_table is not None and len(alpha_table)
             else np.full(pixels.shape[:2], 255, np.uint8))
        if alpha_table is not None:
            a = np.where(pixels[..., 0] < len(alpha_table), alpha_table[np.minimum(pixels[..., 0], len(alpha_table) - 1)], 255)
        pixels = np.concatenate([rgb, a[..., None].astype(np.uint8)], axis=2)
    elif channels == 1:
        pixels = np.concatenate([np.repeat(pixels, 3, axis=2), np.full(pixels.shape, 255, np.uint8)], axis=2)
    elif channels == 2:
        pixels = np.concatenate([np.repeat(pixels[..., :1], 3, axis=2), pixels[..., 1:]], axis=2)
    elif channels == 3:
        pixels = np.concatenate([pixels, np.full(pixels.shape[:2] + (1,), 255, np.uint8)], axis=2)
    return pixels.astype(np.float64) / 255.0


def srgb_to_linear(c):
    c = np.asarray(c, dtype=np.float64)
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(np.asarray(c, dtype=np.float64), 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def downsample(image, factor):
    h, w = image.shape[:2]
    return image.reshape(h // factor, factor, w // factor, factor, *image.shape[2:]).mean(axis=(1, 3))


def push_pull_fill(rgb, weight):
    """Fill texels with zero weight from the weighted average around them."""
    levels = [(rgb * weight[..., None], weight)]
    while min(levels[-1][1].shape) > 1:
        c, w = levels[-1]
        h, wd = c.shape[0] // 2 * 2, c.shape[1] // 2 * 2
        c, w = c[:h, :wd], w[:h, :wd]
        levels.append((downsample(c, 2) if h and wd else c, downsample(w, 2)))
    filled = levels[-1][0] / np.maximum(levels[-1][1], 1e-8)[..., None]
    for c, w in reversed(levels[:-1]):
        up = np.repeat(np.repeat(filled, 2, axis=0), 2, axis=1)
        up = np.pad(up, ((0, max(0, c.shape[0] - up.shape[0])), (0, max(0, c.shape[1] - up.shape[1])), (0, 0)),
                    mode="edge")[:c.shape[0], :c.shape[1]]
        own = c / np.maximum(w, 1e-8)[..., None]
        t = np.clip(w, 0, 1)[..., None]
        filled = own * t + up * (1 - t)
    return filled


# --- noise ----------------------------------------------------------------

def value_noise_2d(shape, cells, seed, tile=True):
    """Smooth value noise on an (h, w) grid with cells=(cy, cx) lattice cells."""
    rng = np.random.default_rng(seed)
    cy, cx = cells
    lattice = rng.random((cy + 1, cx + 1))
    if tile:
        lattice[-1, :] = lattice[0, :]
        lattice[:, -1] = lattice[:, 0]
    h, w = shape
    y = np.linspace(0, cy, h, endpoint=not tile)[:, None] * np.ones((1, w))
    x = np.linspace(0, cx, w, endpoint=not tile)[None, :] * np.ones((h, 1))
    if tile:
        y = (np.arange(h)[:, None] + 0.5) / h * cy * np.ones((1, w))
        x = (np.arange(w)[None, :] + 0.5) / w * cx * np.ones((h, 1))
    iy = np.minimum(np.floor(y).astype(int), cy - 1)
    ix = np.minimum(np.floor(x).astype(int), cx - 1)
    fy, fx = y - iy, x - ix
    fy, fx = fy * fy * (3 - 2 * fy), fx * fx * (3 - 2 * fx)
    a = lattice[iy, ix]; b = lattice[iy, ix + 1]
    c = lattice[iy + 1, ix]; d = lattice[iy + 1, ix + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fbm_2d(shape, base_cells, octaves, seed, gain=0.5, tile=True):
    total = np.zeros(shape)
    amplitude, norm = 1.0, 0.0
    for o in range(octaves):
        cells = (base_cells[0] * 2 ** o, base_cells[1] * 2 ** o)
        total += amplitude * value_noise_2d(shape, cells, seed + 101 * o, tile)
        norm += amplitude
        amplitude *= gain
    return total / norm


def _hash3(ix, iy, iz, seed):
    h = (ix * 73856093) ^ (iy * 19349663) ^ (iz * 83492791) ^ (seed * 2654435761)
    h = (h ^ (h >> 13)) * 1274126177
    h = h ^ (h >> 16)
    return (h & 0xFFFFFF) / float(0x1000000)


def value_noise_3d(points, seed):
    """Smooth value noise at Nx3 points (unit lattice)."""
    p = np.asarray(points, dtype=np.float64)
    i = np.floor(p).astype(np.int64)
    f = p - i
    f = f * f * (3 - 2 * f)
    result = np.zeros(len(p))
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                w = ((f[:, 0] if dx else 1 - f[:, 0]) * (f[:, 1] if dy else 1 - f[:, 1]) *
                     (f[:, 2] if dz else 1 - f[:, 2]))
                result += w * _hash3(i[:, 0] + dx, i[:, 1] + dy, i[:, 2] + dz, seed)
    return result


def fbm_3d(points, frequency, octaves, seed, gain=0.5):
    p = np.asarray(points, dtype=np.float64)
    total = np.zeros(len(p))
    amplitude, norm = 1.0, 0.0
    for o in range(octaves):
        total += amplitude * value_noise_3d(p * frequency * 2 ** o + 17.3 * o, seed + 31 * o)
        norm += amplitude
        amplitude *= gain
    return total / norm


# --- software rasterizer (impostor and cluster baking) --------------------

class Texture:
    """Linear RGBA texture sampled bilinearly with uv (v = 0 top row)."""

    def __init__(self, rgba_linear: np.ndarray):
        self.data = rgba_linear
        self.h, self.w = rgba_linear.shape[:2]

    @staticmethod
    def from_png(path, srgb=True):
        image = read_png(path)
        if srgb:
            image[..., :3] = srgb_to_linear(image[..., :3])
        return Texture(image)

    def sample(self, uv):
        u = (uv[:, 0] - np.floor(uv[:, 0])) * self.w - 0.5
        v = (uv[:, 1] - np.floor(uv[:, 1])) * self.h - 0.5
        x0 = np.floor(u).astype(int); y0 = np.floor(v).astype(int)
        fx = (u - x0)[:, None]; fy = (v - y0)[:, None]
        x0 %= self.w; y0 %= self.h
        x1 = (x0 + 1) % self.w; y1 = (y0 + 1) % self.h
        d = self.data
        return ((d[y0, x0] * (1 - fx) + d[y0, x1] * fx) * (1 - fy) +
                (d[y1, x0] * (1 - fx) + d[y1, x1] * fx) * fy)


class Surface:
    """One draw for the rasterizer: triangles plus their material."""

    def __init__(self, positions, normals, uvs, colors, triangles, texture=None,
                 base_color=(1, 1, 1), alpha_cutoff=0.0, use_vertex_colors=True):
        self.p = np.asarray(positions, dtype=np.float64)
        self.n = np.asarray(normals, dtype=np.float64)
        self.uv = np.asarray(uvs, dtype=np.float64)
        self.c = np.asarray(colors, dtype=np.float64)
        self.tri = np.asarray(triangles, dtype=np.int64)
        self.texture = texture
        self.base = np.asarray(base_color, dtype=np.float64)
        self.cutoff = alpha_cutoff
        self.use_vertex_colors = use_vertex_colors

    @staticmethod
    def from_builder(builder: MeshBuilder, **material):
        p, n, uv, c, idx = builder.arrays()
        return Surface(p, n, uv, c, idx, **material)

    def transformed(self, matrix3, offset):
        m = np.asarray(matrix3, dtype=np.float64)
        s = Surface(self.p @ m.T + offset, normalize(self.n @ m.T), self.uv, self.c, self.tri,
                    self.texture, self.base, self.cutoff, self.use_vertex_colors)
        return s


def rasterize(surfaces, right, up, forward, origin_uv, scale, size):
    """Orthographic render.

    right/up/forward: unit world axes of the image (forward points from the
    scene toward the camera). A world point p lands at pixel
    x = (dot(p, right) - origin_uv[0]) * scale, y = (dot(p, up) - origin_uv[1]) * scale,
    with row 0 at the smallest dot(p, up). Returns albedo (linear), alpha,
    world normal, depth (larger = nearer the camera).
    """
    width, height = size
    albedo = np.zeros((height, width, 3))
    normal = np.zeros((height, width, 3))
    alpha = np.zeros((height, width))
    depth = np.full((height, width), -np.inf)
    right, up, forward = (np.asarray(a, dtype=np.float64) for a in (right, up, forward))
    for s in surfaces:
        if not len(s.tri):
            continue
        sx = (s.p @ right - origin_uv[0]) * scale
        sy = (s.p @ up - origin_uv[1]) * scale
        sz = s.p @ forward
        for a, b, c in s.tri:
            xa, xb, xc = sx[a], sx[b], sx[c]
            ya, yb, yc = sy[a], sy[b], sy[c]
            area = (xb - xa) * (yc - ya) - (xc - xa) * (yb - ya)
            if abs(area) < 1e-12:
                continue
            x0 = max(0, int(math.floor(min(xa, xb, xc))))
            x1 = min(width - 1, int(math.ceil(max(xa, xb, xc))))
            y0 = max(0, int(math.floor(min(ya, yb, yc))))
            y1 = min(height - 1, int(math.ceil(max(ya, yb, yc))))
            if x0 > x1 or y0 > y1:
                continue
            px = np.arange(x0, x1 + 1) + 0.5
            py = np.arange(y0, y1 + 1) + 0.5
            gx, gy = np.meshgrid(px, py)
            w0 = ((xb - gx) * (yc - gy) - (xc - gx) * (yb - gy)) / area
            w1 = ((xc - gx) * (ya - gy) - (xa - gx) * (yc - gy)) / area
            w2 = 1 - w0 - w1
            inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not inside.any():
                continue
            yy, xx = np.nonzero(inside)
            w0, w1, w2 = w0[inside], w1[inside], w2[inside]
            yy, xx = yy + y0, xx + x0
            z = w0 * sz[a] + w1 * sz[b] + w2 * sz[c]
            nearer = z > depth[yy, xx]
            if not nearer.any():
                continue
            yy, xx, z = yy[nearer], xx[nearer], z[nearer]
            w = np.stack([w0[nearer], w1[nearer], w2[nearer]], axis=1)
            col = np.broadcast_to(s.base, (len(z), 3)).copy()
            a_out = np.ones(len(z))
            if s.use_vertex_colors:
                vc = w @ s.c[[a, b, c]]
                col *= vc[:, :3]
            if s.texture is not None:
                uv = w @ s.uv[[a, b, c]]
                texel = s.texture.sample(uv)
                col *= texel[:, :3]
                a_out = texel[:, 3]
                keep = a_out >= s.cutoff
                if not keep.any():
                    continue
                yy, xx, z, col, w = yy[keep], xx[keep], z[keep], col[keep], w[keep]
            nrm = normalize(w @ s.n[[a, b, c]])
            # Two-sided cards face the camera.
            flip = (nrm @ forward) < 0
            nrm[flip] *= -1
            depth[yy, xx] = z
            albedo[yy, xx] = col
            normal[yy, xx] = nrm
            alpha[yy, xx] = 1.0
    return albedo, alpha, normal, depth


# --- impostor atlases ------------------------------------------------------

def bake_impostor(surfaces, name, texture_dir: Path, mesh_path: Path, views=6,
                  atlas_size=1024, supersample=2, bands=3, ambient_occlusion=True):
    """Bake a View-baked impostor (Material.impostorBillboard) like the oak pack's:
    `views` vertical cards through the placement axis, each facing the
    direction it was baked from, plus a horizontal card seen from above.

    Writes <name>_Impostor_Color.png / _Normal.png into texture_dir (sources)
    and the card mesh to mesh_path. Returns (color_png, normal_png).
    """
    allp = np.concatenate([s.p for s in surfaces if len(s.p)])
    top = float(allp[:, 1].max())
    bottom = min(0.0, float(allp[:, 1].min()))
    radius = float(np.max(np.linalg.norm(allp[:, [0, 2]], axis=1))) * 1.02
    height = top - bottom
    # Layout: side views in a grid (whichever of 6x1, 3x2, 2x3 gives the
    # most texels per metre for this silhouette), top view in the space
    # left over. Cells share one scale so all views match.
    side_w = 2 * radius
    margin = 4

    def fit(cols, rows):
        # Top view goes beside the grid if there is room, else below it.
        below = min((atlas_size - margin * (cols + 1)) / (cols * side_w),
                    (atlas_size - margin * (rows + 2)) / (rows * height + 2 * radius))
        beside = min((atlas_size - margin * (cols + 2)) / (cols * side_w + 2 * radius),
                     (atlas_size - margin * (rows + 1)) / (rows * height))
        return max((below, "below"), (beside, "beside"))

    candidates = [(fit(c, -(-views // c)), c) for c in (views, 3, 2) if c <= views]
    (scale, top_place), cols = max(candidates, key=lambda e: e[0][0])
    rows = -(-views // cols)
    while True:
        cell_w = int(math.ceil(side_w * scale))
        cell_h = int(math.ceil(height * scale))
        top_px = int(math.ceil(2 * radius * scale))
        grid_w = margin + cols * (cell_w + margin)
        grid_h = margin + rows * (cell_h + margin)
        fits = (grid_w + (top_px + margin if top_place == "beside" else 0) <= atlas_size and
                grid_h + (top_px + margin if top_place == "below" else 0) <= atlas_size)
        if fits:
            break
        scale *= 0.995
    ss = supersample
    color = np.zeros((atlas_size, atlas_size, 3))
    alpha = np.zeros((atlas_size, atlas_size))
    nmap = np.zeros((atlas_size, atlas_size, 3))
    nmap[...] = (0.5, 0.5, 1.0)
    mesh = MeshBuilder()
    up = np.array([0.0, 1.0, 0.0])

    def place(img_a, img_c, img_n, x0, y0):
        h, w = img_a.shape
        color[y0:y0 + h, x0:x0 + w] = img_c
        alpha[y0:y0 + h, x0:x0 + w] = img_a
        nmap[y0:y0 + h, x0:x0 + w] = img_n

    def render(right, upaxis, forward, origin, w_px, h_px, tangent, bitangent, card_normal):
        alb, a, n, _ = rasterize(surfaces, right, upaxis, forward, origin, scale * ss, (w_px * ss, h_px * ss))
        a_small = downsample(a, ss)
        alb_small = downsample(alb * a[..., None], ss) / np.maximum(a_small, 1e-6)[..., None]
        n_small = normalize(downsample(n * a[..., None], ss) + 1e-9)
        # Tangent-space normals against the card frame.
        ts = np.stack([n_small @ tangent, n_small @ bitangent, n_small @ card_normal], axis=-1)
        ts[..., 2] = np.maximum(ts[..., 2], 0.05)
        ts = normalize(ts)
        if ambient_occlusion:
            # Darken toward the inside of the silhouette: cheap self-occlusion
            # so the flat card keeps the crown's depth.
            occl = 0.72 + 0.28 * blur(a_small, 6)
            alb_small = alb_small * occl[..., None]
        return a_small, alb_small, ts * 0.5 + 0.5

    # Silhouette bands per card for a tight polygon (less overdraw).
    for k in range(views):
        yaw = 2 * math.pi * k / views
        card_normal = np.array([math.cos(yaw), 0.0, math.sin(yaw)])   # toward the baking camera
        tangent = np.cross(up, card_normal)                               # u direction
        tangent = tangent / np.linalg.norm(tangent)
        # cross(N, T) * w must be +Y (v grows with height).
        w_sign = 1.0 if np.dot(np.cross(card_normal, tangent), up) > 0 else -1.0
        x0 = margin + (k % cols) * (cell_w + margin)
        y0 = margin + (k // cols) * (cell_h + margin)
        a, c, n = render(tangent, up, card_normal, (-radius, bottom), cell_w, cell_h, tangent, up, card_normal)
        place(a, c, n, x0, y0)
        # Card polygon: `bands` horizontal slices fitted to the silhouette.
        rows = np.nonzero(a.max(axis=1) > 0.02)[0]
        top_row = (rows.max() + 1) if len(rows) else cell_h
        band_edges = np.linspace(0, top_row, bands + 1).round().astype(int)
        extents = []
        for b0, b1 in zip(band_edges[:-1], band_edges[1:]):
            slab = a[max(0, b0 - 2):min(cell_h, b1 + 2)]
            cols_on = np.nonzero(slab.max(axis=0) > 0.02)[0]
            if len(cols_on):
                extents.append((max(0, cols_on.min() - 2), min(cell_w, cols_on.max() + 3)))
            else:
                extents.append((cell_w // 2, cell_w // 2 + 1))
        # Boundary rows take the union of the bands they join.
        verts = []
        for i, row in enumerate(band_edges):
            lo = min(extents[max(0, i - 1)][0], extents[min(bands - 1, i)][0])
            hi = max(extents[max(0, i - 1)][1], extents[min(bands - 1, i)][1])
            yw = bottom + row / scale
            left = -radius + lo / scale
            right_ = -radius + hi / scale
            vrow = (y0 + row) / atlas_size
            tan4 = (tangent[0], tangent[1], tangent[2], w_sign)
            l = mesh.vertex(tangent * left + up * yw, card_normal, ((x0 + lo) / atlas_size, vrow), tangent=tan4)
            r = mesh.vertex(tangent * right_ + up * yw, card_normal, ((x0 + hi) / atlas_size, vrow), tangent=tan4)
            verts.append((l, r))
        for (l0, r0), (l1, r1) in zip(verts[:-1], verts[1:]):
            mesh.quad(l0, r0, r1, l1)

    # Top view: looking down -Y. Card normal +Y, u along tangent, v along
    # bitangent = cross(N, T) * w.
    card_normal = up
    tangent = np.array([0.0, 0.0, -1.0])
    bitangent = np.cross(card_normal, tangent)          # (-1, 0, 0) ... choose w=+1
    if top_place == "beside":
        x0, y0 = grid_w, margin
    else:
        x0, y0 = margin, grid_h
    size_px = min(top_px, atlas_size - y0 - margin, atlas_size - x0 - margin)
    a, c, n = render(tangent, bitangent, card_normal, (-radius, -radius), size_px, size_px,
                     tangent, bitangent, card_normal)
    place(a, c, n, x0, y0)
    # A horizontal card at 55% of the crown height.
    yc = bottom + height * 0.55
    tan4 = (tangent[0], tangent[1], tangent[2], 1.0)
    corners = []
    for (cu, cv) in ((0, 0), (1, 0), (1, 1), (0, 1)):
        pu, pv = -radius + cu * size_px / scale, -radius + cv * size_px / scale
        pos = tangent * pu + bitangent * pv + up * yc
        corners.append(mesh.vertex(pos, card_normal,
                                   ((x0 + cu * size_px) / atlas_size, (y0 + cv * size_px) / atlas_size),
                                   tangent=tan4))
    mesh.quad(*corners)

    # Fill the background so mips and filtering never pull in black.
    filled = push_pull_fill(color, alpha)
    rgba = np.concatenate([linear_to_srgb(filled), alpha[..., None]], axis=2)
    nfill = push_pull_fill(nmap, alpha)
    nfill = normalize(nfill * 2 - 1) * 0.5 + 0.5
    texture_dir.mkdir(parents=True, exist_ok=True)
    color_png = texture_dir / f"{name}_Impostor_Color.png"
    normal_png = texture_dir / f"{name}_Impostor_Normal.png"
    write_png(color_png, rgba)
    write_png(normal_png, nfill)
    triangles = mesh.write_native(mesh_path)
    return color_png, normal_png, triangles


def blur(image, radius):
    """Separable box blur (3 passes ~ Gaussian)."""
    out = np.asarray(image, dtype=np.float64)
    for _ in range(3):
        for axis in (0, 1):
            pad = [(0, 0)] * out.ndim
            pad[axis] = (radius, radius)
            padded = np.pad(out, pad, mode="edge")
            c = np.cumsum(padded, axis=axis)
            c = np.concatenate([np.zeros_like(np.take(c, [0], axis=axis)), c], axis=axis)
            n = out.shape[axis]
            out = (np.take(c, np.arange(2 * radius + 1, 2 * radius + 1 + n), axis=axis) -
                   np.take(c, np.arange(0, n), axis=axis)) / (2 * radius + 1)
    return out


# --- texture cooking --------------------------------------------------------

def find_texture_cook():
    for config in ("RelWithDebInfo", "Release", "Debug"):
        for candidate in (ROOT / "build" / config / "TextureCook.exe",
                          ROOT / "build" / "Debug" / config / "TextureCook.exe"):
            if candidate.exists():
                return candidate
    found = shutil.which("TextureCook")
    return Path(found) if found else None


def cook(source: Path, output: Path, usage: str, alpha_cutoff: float | None = None,
         max_size: int | None = None):
    """PNG source -> KTX2 asset. Falls back to copying the PNG if TextureCook
    is not built (the asset then loads with runtime box-filtered mips)."""
    tool = find_texture_cook()
    output.parent.mkdir(parents=True, exist_ok=True)
    if tool is None:
        fallback = output.with_suffix(".png")
        shutil.copyfile(source, fallback)
        print(f"  TextureCook not built; using {fallback.name} (no coverage-preserving mips)")
        return fallback
    args = [str(tool), "--usage", usage]
    if alpha_cutoff is not None:
        args += ["--alpha-cutoff", f"{alpha_cutoff:g}"]
    if max_size:
        args += ["--max-size", str(max_size)]
    args += [str(source), str(output)]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"TextureCook failed for {source}:\n{result.stdout}\n{result.stderr}")
    return output


# --- materials and prefabs -------------------------------------------------

MATERIAL_DEFAULTS = {
    "version": 1,
    "baseColor": [1, 1, 1],
    "specularModel": "MetallicRoughness",
    "metallic": 0,
    "roughness": 0.85,
    "castsShadows": True,
    "doubleSided": False,
    "normalScale": 1,
}


def write_material(path: Path, **fields):
    import json
    material = dict(MATERIAL_DEFAULTS)
    material.update(fields)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(material, indent=4) + "\n", encoding="utf-8", newline="\n")
    return path


def write_prefab(path: Path, name: str, parts, extra_root_components=""):
    """parts: list of dicts with
         mesh (asset path), lods (list of asset paths, "" hides),
         material (repo path), lod_materials (list, "" keeps base),
         lod1, lod2 (Mesh.gpuLod1Distance/gpuLod2Distance), wind (foliageWindStrength).
    One part goes on the root object; several become child primitives (as the
    oak pack's prefabs do)."""
    from xml.sax.saxutils import quoteattr, escape

    def components(part, indent):
        pad = " " * indent
        attrs = (f'Mesh.file={quoteattr(part["mesh"])} Mesh.foliageWindStrength="{part.get("wind", 0):g}" '
                 f'Mesh.useLightProbes="false" Mesh.gpuLodSelection="true" '
                 f'Mesh.gpuLod1Distance="{part.get("lod1", 60):g}" Mesh.gpuLod2Distance="{part.get("lod2", 220):g}"')
        lines = []
        lods = part.get("lods", [])
        if lods:
            lines.append(f"{pad}<components:Mesh {attrs}>")
            lines.append(f"{pad}  <Mesh.lodMeshes>")
            for entry in lods:
                lines.append(f'{pad}    <item _valueType="string" />' if not entry else
                             f"{pad}    <item>{escape(entry)}</item>")
            lines.append(f"{pad}  </Mesh.lodMeshes>")
            lines.append(f"{pad}</components:Mesh>")
        else:
            lines.append(f"{pad}<components:Mesh {attrs} />")
        lod_materials = part.get("lod_materials", [])
        material_attr = f'Material.materialAsset={quoteattr(part["material"])}'
        if any(lod_materials):
            lines.append(f"{pad}<components:Material {material_attr}>")
            lines.append(f"{pad}  <Material.lodMaterials>")
            for entry in lod_materials:
                lines.append(f'{pad}    <item _valueType="string" />' if not entry else
                             f"{pad}    <item>{escape(entry)}</item>")
            lines.append(f"{pad}  </Material.lodMaterials>")
            lines.append(f"{pad}</components:Material>")
        else:
            lines.append(f"{pad}<components:Material {material_attr} />")
        return lines

    def transform(indent):
        pad = " " * indent
        return [f"{pad}<transform>",
                f'{pad}  <Transform.position _arrayEncoding="spaceSeparatedNumbers">0 0 0</Transform.position>',
                f'{pad}  <Transform.rotation _arrayEncoding="spaceSeparatedNumbers">0 0 0</Transform.rotation>',
                f'{pad}  <Transform.scale _arrayEncoding="spaceSeparatedNumbers">1 1 1</Transform.scale>',
                f"{pad}</transform>"]

    lines = ["<?xml version='1.0' encoding='utf-8'?>",
             '<Prefab xmlns:components="urn:engine-components" version="1" type="prefab">',
             f'  <object name={quoteattr(name)} enabled="true">',
             "    <components>"]
    if len(parts) == 1:
        lines += components(parts[0], 6)
    if extra_root_components:
        lines.append(extra_root_components)
    lines.append("    </components>")
    if len(parts) == 1:
        lines.append("    <children />")
    else:
        lines.append("    <children>")
        for i, part in enumerate(parts):
            lines.append(f'      <item name={quoteattr(part.get("name", f"Primitive {i + 1}"))} enabled="true">')
            lines += transform(8)
            lines.append("        <components>")
            lines += components(part, 10)
            lines.append("        </components>")
            lines.append("        <children />")
            lines.append("      </item>")
        lines.append("    </children>")
    lines += transform(4)
    lines += ["  </object>", "</Prefab>"]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return path
