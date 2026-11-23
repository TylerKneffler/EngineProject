"""Density-volume terrain (.md/todo.md, volume terrain stage 1): builds a
.volume file and a test scene that draws it (Engine VolumeTerrain).

A volume holds signed density on a regular 3D grid: metres to the surface,
positive inside the ground, negative in the air. Shapes combine by smooth
union (max with a fillet of SMOOTH metres) and subtraction, so overlapping
edits merge into one surface instead of folding through each other, and
overhangs, arches and caves are ordinary shapes.

Ingredients, in the volume's frame (the test cell: x and z 0..128 m):
  * ground: a height field (rows along +z), density = (height - y) scaled by
    the slope so it approximates a distance on steep ground;
  * stamps (tools/bake_terrain_stamp.py): solid behind their face, within
    the patch's outline and STAMP_DEPTH metres deep;
  * brushes: rounded boxes (union) and spheres (subtraction).

Test scene (terrain/volume_test.scene, viewpoints as the stamp test):
the stamp test's block and wall stamps, plus a mass raised against the east
wall that runs sideways out of it and down into the floor (the overlap case),
and a cave carved under the north rim.

File format: see Engine/Core/Components/Terrain/TerrainVolume.h.
Usage: python tools/terrain_volume.py [--no-edits] [--spacing M] [--output SCENE]
"""

import argparse
import math
import random
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree

import build_grass_test_scene as grass
import build_stamp_test_scene as stamp_test
import scene_static
import terrain_stamp_morph as morph

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Engine" / "Core" / "Assets"
VOLUMES = ASSETS / "TerrainData" / "Volumes"
SCENE = ASSETS / "Scenes" / "terrain" / "volume_test.scene"
FLAT_MATERIAL = "Assets/Materials/Terrain/PBR/grass.material"
STEEP_MATERIAL = "Assets/Materials/Terrain/volume_rock.material"
LOD_SETTINGS = dict(chunkCells=32, lod1Distance=64, lod2Distance=128, lod3Distance=256,
                    drawDistance=700, collisionDistance=96)
MAGIC = int.from_bytes(b"TRNRVOL1", "little")
UNITS = 256.0          # stored units per metre
CLAMP = 8.0            # densities are stored within +-CLAMP metres
SMOOTH = 1.5           # fillet of smooth unions, metres
STAMP_DEPTH = 6.0      # metres of rock behind a stamp's face
Y_RANGE = (-6.0, 34.0)
DROP_TESTS = [(100.0, 25.0, 58.0), (112.0, 12.0, 46.0)]   # cubes' starts: over the shoulder, over ground


def smooth_union(a, b, k=SMOOTH):
    """Smooth maximum of two densities (positive inside): max(a, b) with a
    rounded fillet up to k metres where they meet."""
    h = np.clip(0.5 + 0.5 * (a - b) / k, 0.0, 1.0)
    return b + (a - b) * h + k * h * (1.0 - h)


def subtract(a, b):
    """a minus b (both positive inside)."""
    return np.minimum(a, -b)


def ground_density(points, heights, spacing):
    h = morph.sample(heights, spacing, (0.0, 0.0), points[:, 0], points[:, 2])
    gz, gx = np.gradient(heights, spacing)
    slope = np.hypot(morph.sample(gx, spacing, (0.0, 0.0), points[:, 0], points[:, 2]),
                     morph.sample(gz, spacing, (0.0, 0.0), points[:, 0], points[:, 2]))
    return (h - points[:, 1]) / np.sqrt(1.0 + slope * slope)


def stamp_density(points, placements):
    """Solid behind each stamp's face: positive depth behind the nearest rock
    point (along its normal) where the sample lies over the patch, negative
    elsewhere. Distances are approximate away from the surface."""
    rock, normals, sizes = [], [], []
    for stamp, matrix in placements:
        linear, offset = matrix[:3, :3], matrix[:3, 3]
        inside = stamp.weight > morph.BORDER_WEIGHT
        rock.append(stamp.position[inside] @ linear.T + offset)
        n = stamp.normal[inside] @ np.linalg.inv(linear)
        normals.append(n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-9))
        sizes.append(np.full(int(inside.sum()), stamp.spacing * np.cbrt(abs(np.linalg.det(linear)))))
    rock, normals, sizes = np.concatenate(rock), np.concatenate(normals), np.concatenate(sizes)
    tree = cKDTree(rock)
    distance, nearest = tree.query(points)
    offset = points - rock[nearest]
    along = np.sum(offset * normals[nearest], axis=1)          # + out of the face
    lateral = np.linalg.norm(offset - along[:, None] * normals[nearest], axis=1)
    over = lateral <= 1.5 * sizes[nearest]
    depth = -along
    solid = over & (depth > 0.0) & (depth < STAMP_DEPTH)
    # Inside: depth below the face (thinning toward the back); outside: the
    # distance to the rock.
    return np.where(solid, np.minimum(depth, STAMP_DEPTH - depth), -distance)


def rounded_box(points, centre, half, radius):
    q = np.abs(points - np.asarray(centre)) - (np.asarray(half) - radius)
    outside = np.linalg.norm(np.maximum(q, 0.0), axis=1) + np.minimum(q.max(axis=1), 0.0) - radius
    return -outside


def sphere(points, centre, radius):
    return radius - np.linalg.norm(points - np.asarray(centre), axis=1)


CHUNK = 32             # samples per stored chunk edge (version 2)


def write_volume(path, density, spacing, origin):
    """Version 2 (sparse): chunks of CHUNK^3 samples; a chunk whose samples
    all agree (wholly ground or air at the clamp) is one value."""
    ny, nz, nx = density.shape
    q = np.clip(np.rint(np.clip(density, -CLAMP, CLAMP) * UNITS), -32768, 32767).astype("<i2")
    cy, cz, cx = (-(-ny // CHUNK), -(-nz // CHUNK), -(-nx // CHUNK))
    # Samples past the grid's end repeat its last.
    padded = np.pad(q, ((0, cy * CHUNK - ny), (0, cz * CHUNK - nz), (0, cx * CHUNK - nx)), mode="edge")
    path.parent.mkdir(parents=True, exist_ok=True)
    uniform = 0
    with open(path, "wb") as f:
        f.write(struct.pack("<QI3i5fI", MAGIC, 2, nx, ny, nz, spacing, *origin, UNITS, CHUNK))
        for y in range(cy):
            for z in range(cz):
                for x in range(cx):
                    block = padded[y * CHUNK:(y + 1) * CHUNK, z * CHUNK:(z + 1) * CHUNK,
                                   x * CHUNK:(x + 1) * CHUNK]
                    first = block.flat[0]
                    if np.all(block == first):
                        f.write(struct.pack("<Bh", 0, int(first)))
                        uniform += 1
                    else:
                        f.write(struct.pack("<B", 1))
                        f.write(np.ascontiguousarray(block).tobytes())
    return uniform, cx * cy * cz


def world_heights(size):
    """Heights (4 m grid) over `size` metres: the stamp test's block cell at
    the origin corner, and past it gentle hills (about +-15 m) that flatten
    toward the block so its stamps keep their ground."""
    cells = int(round(size / stamp_test.SPACING))
    z, x = np.mgrid[0:cells + 1, 0:cells + 1] * stamp_test.SPACING
    hills = (9.0 * np.sin(x / 61.0 + 0.7) * np.cos(z / 47.0) +
             6.0 * np.sin((x + z) / 29.0) * np.sin(z / 83.0 - 0.4))
    block = stamp_test.base_heights()
    centre = 0.5 * stamp_test.WIDTH
    t = np.clip((np.hypot(x - centre, z - centre) - 90.0) / 70.0, 0.0, 1.0)
    heights = hills * t * t * (3 - 2 * t)
    n = block.shape[0]
    heights[:n, :n] = np.where(t[:n, :n] > 0, heights[:n, :n], 0.0) + block
    return heights


def build_density(spacing, edits=True, size=128.0):
    heights = world_heights(size) if size > stamp_test.WIDTH else stamp_test.base_heights()
    placed = stamp_test.wall_stamps(stamp_test.base_heights(), random.Random(stamp_test.SEED))
    placements = [(morph.load_stamp(n), m) for n, m in placed]
    extent = (heights.shape[0] - 1) * stamp_test.SPACING
    xs = np.arange(0.0, extent + 1e-6, spacing)
    zs = np.arange(0.0, extent + 1e-6, spacing)
    low = min(Y_RANGE[0], float(heights.min()) - 6.0)
    ys = np.arange(low, Y_RANGE[1] + 1e-6, spacing)
    z, x = np.meshgrid(zs, xs, indexing="ij")
    density = np.empty((len(ys), len(zs), len(xs)), np.float32)
    top = stamp_test.BLOCK_HEIGHT
    east = (stamp_test.BLOCK[1] + 0.5) * stamp_test.SPACING
    north = (stamp_test.BLOCK[1] + 0.5) * stamp_test.SPACING
    near_block = (x < stamp_test.WIDTH + 16) & (z < stamp_test.WIDTH + 16)
    # One horizontal slab at a time (a 512 m world is ~15 M samples).
    for j, y in enumerate(ys):
        points = np.stack([x.ravel(), np.full(x.size, y), z.ravel()], 1)
        d = ground_density(points, heights, stamp_test.SPACING)
        near = near_block.ravel()
        if near.any() and y < top + 8:
            d[near] = smooth_union(d[near], stamp_density(points[near], placements))
            if edits:
                # Raised against the east wall, out sideways and down into
                # the floor (a shoulder and a leg; both overlap the wall), and
                # a cave under the north rim, into the wall and the block.
                d[near] = smooth_union(d[near], rounded_box(points[near], (east + 7.0, top * 0.6, 58.0),
                                                            (9.0, 3.0, 5.0), 2.0))
                d[near] = smooth_union(d[near], rounded_box(points[near], (east + 13.0, top * 0.25, 58.0),
                                                            (3.5, top * 0.35, 4.5), 2.0))
                d[near] = subtract(d[near], sphere(points[near], (64.0, 5.0, north - 2.0), 7.0))
        density[j] = d.reshape(len(zs), len(xs))
    return density, (0.0, float(ys[0]), 0.0), heights


def build_scene(volume_asset, heights, output, extent):
    """The stamp test's scene with the terrain cell replaced by the volume."""
    grass.build(*stamp_test.CELL, output=output, far=False, trees=False)
    ET.register_namespace("components", grass.NAMESPACE)
    scene = ET.parse(output).getroot()
    objects = scene.find("objects")
    for item in list(objects):
        if (item.get("name") or "").startswith("Landscape Cell"):
            objects.remove(item)
    item = ET.SubElement(objects, "item", {"name": "Volume Terrain", "enabled": "true"})
    item.append(grass.transform((0, 0, 0)))
    components = ET.SubElement(item, "components")
    # Chunks, levels of detail, colliders and layer shading are the
    # component's own (VolumeTerrain); the grass and rock are the terrain's
    # PBR layers.
    components.append(grass.component("VolumeTerrain", "VolumeTerrain", dict(
        volumeFile=volume_asset, flatMaterial=FLAT_MATERIAL, steepMaterial=STEEP_MATERIAL,
        textureTiling=16, worldExtent=extent, **LOD_SETTINGS)))
    ET.SubElement(item, "children")
    # Collision check: 1 m cubes dropped onto the shoulder's top (about 13 m
    # up at x 100, z 58) and onto open ground; they rest there if the
    # volume's mesh collides.
    for index, start in enumerate(DROP_TESTS):
        cube = ET.SubElement(objects, "item", {"name": f"Drop Test Cube {index + 1}", "enabled": "true"})
        cube.append(grass.transform(start))
        cube_components = ET.SubElement(cube, "components")
        cube_components.append(grass.component("Mesh", "Mesh", {"file": "Assets/Mesh/cube.obj"}))
        tint = grass.component("Material", "Material", {"metallic": 0, "roughness": 0.5})
        tint.append(grass.numbers("Material.baseColor", (0.9, 0.2, 0.1)))
        cube_components.append(tint)
        collider = grass.component("PrimitiveObjectCollider", "PrimitiveObjectCollider", {
            "collisionEnabled": "true", "shape": "Box"})
        collider.append(grass.numbers("PrimitiveObjectCollider.center", (0, 0, 0)))
        collider.append(grass.numbers("PrimitiveObjectCollider.size", (1, 1, 1)))
        cube_components.append(collider)
        cube_components.append(grass.component("RigidBody", "RigidBody", {
            "bodyType": "Dynamic", "mass": 1, "useGravity": "true", "friction": 0.6,
            "restitution": 0.1, "continuousCollision": "true"}))
        ET.SubElement(cube, "children")

    def at(x, z):
        return float(morph.sample(heights, stamp_test.SPACING, (0.0, 0.0), x, z))

    poses = {}
    viewpoints = list(stamp_test.VIEWPOINTS)
    if extent > stamp_test.WIDTH:
        # Across the hills to the block: every level of detail in one view.
        viewpoints.append(("Far", (extent - 40.0, 45.0, extent - 40.0), (64.0, 4.0, 64.0)))
        viewpoints.append(("Ridge", (extent * 0.55, 30.0, extent * 0.35), (64.0, 8.0, 64.0)))
    for name, (ex, eh, ez), target in viewpoints:
        eye = (ex, (eh if eh > stamp_test.BLOCK_HEIGHT else at(ex, ez) + eh), ez)
        poses[name] = eye
        camera_item = ET.SubElement(objects, "item", {"name": name, "enabled": "true"})
        camera_components = ET.SubElement(camera_item, "components")
        camera = grass.component("Camera", "Camera", {
            "active": "false", "useTransformRotation": "false", "fov": 67, "near": 0.06,
            "far": 4000, "orthographic": "false", "orthographicSize": 5})
        camera.append(grass.numbers("Camera.up", (0, 1, 0)))
        camera.append(grass.numbers("Camera.target", target))
        camera_components.append(camera)
        ET.SubElement(camera_item, "children")
        camera_item.append(grass.transform(eye))
    explorer = next(i for i in objects if i.get("name") == grass.VIEWER)
    for old in explorer.findall("transform"):
        explorer.remove(old)
    explorer.insert(0, grass.transform(poses[stamp_test.VIEWPOINTS[0][0]]))
    scene_static.mark_static(objects)
    # The drop-test cubes fall: dynamic, not static.
    for item in objects:
        if (item.get("name") or "").startswith("Drop Test Cube"):
            item.attrib.pop("static", None)
    ET.indent(scene, "  ")
    header = ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!--\n"
              "  Volume terrain test: the stamp test's block as a density volume.\n"
              "  Generated by tools/terrain_volume.py; re-running replaces this file.\n"
              "-->\n")
    output.write_text(header + ET.tostring(scene, encoding="unicode") + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--spacing", type=float, default=1.0)
    parser.add_argument("--no-edits", action="store_true", help="leave out the shoulder and the cave")
    parser.add_argument("--output", type=Path, default=SCENE)
    parser.add_argument("--size", type=float, default=128.0,
                        help="world size in metres (over 128: hills around the block cell)")
    parser.add_argument("--name", default=None, help="volume file name (default from the options)")
    args = parser.parse_args()
    density, origin, heights = build_density(args.spacing, edits=not args.no_edits, size=args.size)
    name = args.name or ("volume_test" + ("_plain" if args.no_edits else "") +
                         (f"_{int(args.size)}" if args.size > 128 else ""))
    path = VOLUMES / f"{name}.volume"
    uniform, chunks = write_volume(path, density, args.spacing, origin)
    output = args.output.resolve()
    extent = (heights.shape[0] - 1) * stamp_test.SPACING
    build_scene(f"Assets/TerrainData/Volumes/{name}.volume", heights, output, extent)
    ny, nz, nx = density.shape
    print(f"{path.relative_to(ROOT)}: {nx}x{ny}x{nz} samples at {args.spacing:g} m, "
          f"{uniform}/{chunks} chunks uniform, {path.stat().st_size / 1e6:.1f} MB "
          f"(dense {density.size * 2 / 1e6:.1f} MB); scene {output}")


if __name__ == "__main__":
    main()
