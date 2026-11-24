"""Builds the terrain stamp test scene: flat ground with a raised block, for trying stamp brushes.

Writes
  Engine/Core/Assets/Scenes/terrain/stamp_test.scene

One 128 m terrain cell on a 1 m grid (finer than the landscape's 4 m, so a
stamp a few metres across keeps its shape), dead flat, with a 48 m square
block 16 m tall in the middle: its walls are one grid step wide, painted rock,
the ground and the block's top grass. Nothing else is placed: stamps are
terrain brushes (the editor's Stamp tool, or a stamp shape on Raise/Lower),
so stamp the walls in the editor and the strokes become the cell's heights.
Lighting, sky and settings come from tools/build_grass_test_scene.py.

Viewpoints (GameBackendCapture --viewpoint NAME):
  Corner   ground level at the north-east corner
  Wall     the east wall face-on from 35 m
  Along    along the east wall from the south-east, grazing light
  Rim      on top of the block, looking over the east rim
  Above    high, looking down at the whole block

  build/RelWithDebInfo/GameBackendCapture.exe --backend dx12
    --scene Engine/Core/Assets/Scenes/terrain/stamp_test.scene
    --width 1920 --height 1080 --warmup 300 --frames 10 --viewpoint Corner --out .local/stamps/corner

Usage: python tools/build_stamp_test_scene.py [--volume [--edits FILE]] [--output PATH]
Re-running replaces the scene (and any stamping done on it).

base_heights, wall_stamps and the 4 m constants are kept for
tools/terrain_volume.py (its offline volume test world).
"""

import argparse
import math
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np

import build_grass_test_scene as grass
import scene_static
import terrain_stamp_morph as morph

OUTPUT = grass.ASSETS / "Scenes" / "terrain" / "stamp_test.scene"
CELL = (0, 0)
SPACING = 4.0
CELLS = 32
WIDTH = CELLS * SPACING
BLOCK = (10, 22)        # grid columns/rows of the block's top (inclusive)
BLOCK_HEIGHT = 16.0
SINK = 1.0              # stamps' base below the ground
SEED = 9241
ROCK_LAYER = 4

# Per wall: (outward direction xz, stamp names along it). Each stamp is scaled
# to reach the wall top and spread evenly along the wall.
WALLS = [
    ((1.0, 0.0), ["Mountainside_01", "Mountainside_01", "Mountainside_01"]),
    ((0.0, 1.0), ["Cliff_Scan_02"]),
    ((-1.0, 0.0), ["Cliff_Scan_01", "Cliff_Scan_01"]),
    ((0.0, -1.0), ["RockFace_01", "Mountainside_01", "RockFace_01"]),
]

VIEWPOINTS = [
    # name, eye (x, height above ground, z), target (x, y, z)
    ("Corner", (112.0, 1.8, 114.0), (88.0, 8.0, 88.0)),
    ("Wall", (127.0, 1.8, 64.0), (90.0, 8.0, 64.0)),
    ("Along", (106.0, 2.5, 22.0), (90.0, 7.0, 70.0)),
    ("Rim", (72.0, BLOCK_HEIGHT + 1.8, 52.0), (100.0, 10.0, 70.0)),
    ("Above", (150.0, 70.0, 150.0), (64.0, 6.0, 64.0)),
]


# The test scene's own grid (the constants above are the 4 m library ones).
TEST_SPACING = 1.0
TEST_CELLS = 128
BLOCK_METRES = (BLOCK[0] * SPACING, BLOCK[1] * SPACING)   # 40..88 m

def base_heights():
    """Flat ground with a gentle swell, and the block."""
    z, x = np.mgrid[0:CELLS + 1, 0:CELLS + 1] * SPACING
    heights = 0.35 * np.sin(x / 23.0) * np.cos(z / 31.0)
    lo, hi = BLOCK
    heights[lo:hi + 1, lo:hi + 1] = BLOCK_HEIGHT
    return heights


def wall_stamps(heights, rng):
    """(name, 4x4 matrix) per stamp: faces over each wall's steep grid step."""
    lo, hi = BLOCK
    centre = (lo + hi) * 0.5 * SPACING
    half = (hi - lo) * 0.5 * SPACING
    out = []
    for (dx, dz), names in WALLS:
        # Middle of the steep step between the block's edge and the ground.
        middle = centre + (half + SPACING * 0.5) * np.array([dx, dz])
        along = np.array([-dz, dx])
        count = len(names)
        for i, name in enumerate(names):
            stamp = morph.load_stamp(name)
            inside = stamp.weight > morph.BORDER_WEIGHT
            p = stamp.position[inside]
            scale = (BLOCK_HEIGHT + SINK + 0.8) / p[:, 1].max()
            # The face's mean depth goes over the step's middle; pieces share
            # the wall's length and stay within it (ends past a corner would
            # stand free over flat ground).
            face = p[:, 2].mean() * scale
            half_width = 0.5 * (p[:, 0].max() - p[:, 0].min()) * scale
            share = (2.0 * half) / count
            slot = -half + share * (i + 0.5)
            slot = float(np.clip(slot, -half + half_width, half - half_width)) if half_width < half else 0.0
            spot = middle + along * slot - np.array([dx, dz]) * face
            yaw = math.degrees(math.atan2(dx, dz)) + rng.uniform(-6.0, 6.0)
            ground = float(morph.sample(heights, SPACING, (0.0, 0.0), spot[0] + dx * 6, spot[1] + dz * 6))
            matrix = morph.placement_matrix((spot[0], ground - SINK, spot[1]), (0.0, 1.0, 0.0), yaw, scale)
            out.append((name, matrix))
    return out


def test_heights():
    """The test scene: dead flat ground and the block, on the 1 m grid."""
    heights = np.zeros((TEST_CELLS + 1, TEST_CELLS + 1))
    lo, hi = (int(round(m / TEST_SPACING)) for m in BLOCK_METRES)
    heights[lo:hi + 1, lo:hi + 1] = BLOCK_HEIGHT
    return heights


def paint(heights, spacing=SPACING):
    """Grass, with rock on the steep steps."""
    gz, gx = np.gradient(heights, spacing)
    steep = np.hypot(gx, gz) > 0.8
    weights = np.zeros(heights.shape + (len(grass.LAYERS),))
    weights[..., grass.GRASS_LAYER] = np.where(steep, 0.0, 1.0)
    weights[..., ROCK_LAYER] = np.where(steep, 1.0, 0.0)
    return weights


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--volume", action="store_true",
                        help="draw the cell as a density volume built from it (VolumeTerrain fromTerrain)")
    parser.add_argument("--edits", type=Path, metavar="VOLUMEEDIT",
                        help="with --volume: shapes added and carved (a .volumeedit file, VolumeTerrain.editFile)")
    args = parser.parse_args()
    output = args.output.resolve()
    grass.build(*CELL, output=output, far=False, trees=False)

    heights = test_heights()
    weights = paint(heights, TEST_SPACING)

    ET.register_namespace("components", grass.NAMESPACE)
    scene = ET.parse(output).getroot()
    objects = scene.find("objects")
    cell = next(i for i in objects if (i.get("name") or "").startswith("Landscape Cell"))
    cell.set("name", "Stamp Test Cell")
    for child in list(cell.find("children")):
        cell.find("children").remove(child)
    terrain = cell.find("components").find(f"{{{grass.NAMESPACE}}}Terrain")
    terrain.set("Terrain.cellsX", str(TEST_CELLS))
    terrain.set("Terrain.cellsY", str(TEST_CELLS))
    terrain.set("Terrain.cellWidth", f"{TEST_SPACING:.1f}")
    terrain.set("Terrain.cellDepth", f"{TEST_SPACING:.1f}")
    for tag in ("Terrain.heights", "Terrain.weights", "Terrain.lateralOffsets"):
        for old in terrain.findall(tag):
            terrain.remove(old)
    layers = terrain.find("Terrain.layers")
    index = list(terrain).index(layers)
    terrain.insert(index, grass.numbers("Terrain.heights", heights.ravel()))
    terrain.insert(index + 1, grass.numbers("Terrain.weights", weights.reshape(-1)))
    for item in layers:
        item.set("minHeight", f"{heights.min():.3f}")
        item.set("maxHeight", f"{heights.max():.3f}")
    if args.volume:
        # Volume terrain stage 3: the cell (its heights) drawn as a
        # density volume built at load; the terrain's own mesh is hidden.
        cell.find("components").append(grass.component("VolumeTerrain", "VolumeTerrain", {
            "fromTerrain": "true", "steepLayer": ROCK_LAYER, "chunkCells": 32,
            "lod1Distance": 64, "lod2Distance": 128, "lod3Distance": 256,
            "drawDistance": 700, "collisionDistance": 96,
            **({"editFile": args.edits.resolve().as_posix()} if args.edits else {})}))

    def at(x, z):
        return float(morph.sample(heights, TEST_SPACING, (0.0, 0.0), x, z))

    def viewpoint(name, eye, target):
        item = ET.Element("item", {"name": name, "enabled": "true"})
        components = ET.SubElement(item, "components")
        camera = grass.component("Camera", "Camera", {
            "active": "false", "useTransformRotation": "false", "fov": 67, "near": 0.06,
            "far": 4000, "orthographic": "false", "orthographicSize": 5})
        camera.append(grass.numbers("Camera.up", (0, 1, 0)))
        camera.append(grass.numbers("Camera.target", target))
        components.append(camera)
        ET.SubElement(item, "children")
        item.append(grass.transform(eye))
        return item

    poses = {}
    for name, (ex, eh, ez), target in VIEWPOINTS:
        eye = (ex, (eh if eh > BLOCK_HEIGHT else at(ex, ez) + eh), ez)
        poses[name] = (eye, target)
        objects.append(viewpoint(name, eye, target))
    # The explorer starts at the first viewpoint.
    explorer = next(i for i in objects if i.get("name") == grass.VIEWER)
    for old in explorer.findall("transform"):
        explorer.remove(old)
    explorer.insert(0, grass.transform(poses[VIEWPOINTS[0][0]][0]))

    scene_static.mark_static(objects)
    ET.indent(scene, "  ")
    header = ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!--\n"
              "  Stamp test: flat ground and a raised block, for stamp brushes.\n"
              "  Generated by tools/build_stamp_test_scene.py; re-running replaces this file.\n"
              "-->\n")
    output.write_text(header + ET.tostring(scene, encoding="unicode") + "\n", encoding="utf-8")
    print(f"{output}: {TEST_CELLS}x{TEST_CELLS} cells at {TEST_SPACING:g} m, block "
          f"{BLOCK_METRES[0]:g}..{BLOCK_METRES[1]:g} m, {BLOCK_HEIGHT:g} m tall")


if __name__ == "__main__":
    main()
