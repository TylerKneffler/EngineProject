"""Builds the landscape demo map: mountains, forests and lake basins.

Writes
  Engine/Core/Assets/TerrainData/Landscape/world.heights   height archive
  Engine/Core/Assets/TerrainData/Landscape/world.fargrid   far silhouette grid
  Engine/Core/Assets/Scenes/terrain/landscape_demo.scene   the scene
and, with --preview <png>, a top-down preview colored like the terrain paint.

The map is (2E+1)^2 cells of 32x32 quads at 4 m (128 m cells); E = 20 gives
5.2 km on a side. Heights are stored in 1/256 m, so they span +-128 m.

Lakes have no water yet. Basins are carved below WATER_LEVEL and the scene's
LandscapeDriver paints them as sand beds with gravel shores. The archive's
two-bit code per sample is a rock class (0..3) from slope, computed over the
whole map so the paint matches across cell borders. Forests and grass are
placed at runtime by LandscapeDriver from the same heights.

The explorer and sun objects are copied from oak_meadow_expanded.scene so
their component settings stay current. Requires numpy. Re-running replaces
the generated files.
"""

import argparse
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Engine" / "Core" / "Assets"
DATA = ASSETS / "TerrainData" / "Landscape"
SCENE = ASSETS / "Scenes" / "terrain" / "landscape_demo.scene"
SOURCE_SCENE = ASSETS / "Scenes" / "terrain" / "oak_meadow_expanded.scene"

EXTENT = 20            # half extent in cells
CELLS = 32             # quads per cell edge
SPACING = 4.0          # metres per quad
WATER_LEVEL = -40.0    # keep in sync with LandscapeDriver.waterLevel
SIGNATURE = 0x4C414E4453434150
ARCHIVE_MAGIC = 0x31544748524E5254
FARGRID_MAGIC = 0x314452474E525254
SCALE = 256.0
NAMESPACE = "urn:engine-components"


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def lattice(ix, iz, seed):
    h = (ix.astype(np.uint64) * np.uint64(0x9E3779B1) ^
         iz.astype(np.uint64) * np.uint64(0x85EBCA77) ^
         np.uint64(seed * 0xC2B2AE3D & 0xFFFFFFFF)) & np.uint64(0xFFFFFFFF)
    h ^= h >> np.uint64(15)
    h = (h * np.uint64(0x2C1B3C6D)) & np.uint64(0xFFFFFFFF)
    h ^= h >> np.uint64(12)
    h = (h * np.uint64(0x297A2D39)) & np.uint64(0xFFFFFFFF)
    h ^= h >> np.uint64(15)
    return (h & np.uint64(0xFFFFFF)).astype(np.float64) / float(0xFFFFFF)


def value_noise(x, z, period, seed):
    x = x / period
    z = z / period
    fx, fz = np.floor(x), np.floor(z)
    ix, iz = fx.astype(np.int64) + 100000, fz.astype(np.int64) + 100000
    tx, tz = x - fx, z - fz
    tx = tx * tx * (3 - 2 * tx)
    tz = tz * tz * (3 - 2 * tz)
    a, b = lattice(ix, iz, seed), lattice(ix + 1, iz, seed)
    c, d = lattice(ix, iz + 1, seed), lattice(ix + 1, iz + 1, seed)
    return (a * (1 - tx) + b * tx) * (1 - tz) + (c * (1 - tx) + d * tx) * tz


def fbm(x, z, period, seed, octaves=4):
    total, amplitude, norm = 0.0, 1.0, 0.0
    for octave in range(octaves):
        total = total + amplitude * value_noise(x, z, period / (2 ** octave), seed + octave * 17)
        norm += amplitude
        amplitude *= 0.5
    return total / norm


def bump(x, z, cx, cz, radius):
    d = np.sqrt((x - cx) ** 2 + (z - cz) ** 2) / radius
    return 1.0 - smoothstep(0.15, 1.0, d)


# Lakes: (center x, center z, radius). The first sits in front of the start;
# the last is an alpine lake inside the northern range.
LAKES = [(180.0, 560.0, 430.0), (-1500.0, -700.0, 560.0),
         (1350.0, -1300.0, 520.0), (1500.0, 900.0, 380.0),
         (-700.0, 1650.0, 300.0)]
START = (0.0, 40.0)


def build_heights():
    side = (2 * EXTENT + 1) * CELLS + 1
    origin = -EXTENT * CELLS * SPACING
    axis = origin + np.arange(side) * SPACING
    x, z = np.meshgrid(axis, axis)  # row = z, col = x

    rolling = (fbm(x, z, 900.0, 3) - 0.5) * 2.0 * 24.0
    hills = (fbm(x, z, 330.0, 53, 3) - 0.5) * 2.0 * 13.0
    # Mountain ranges where a broad mask is high. Two ridged octaves: long
    # crests, and shorter spurs steep enough for bare rock faces.
    mask = smoothstep(0.44, 0.60, fbm(x, z, 1500.0, 101, 3))
    crests = 1.0 - np.abs(2.0 * fbm(x, z, 560.0, 211, 4) - 1.0)
    spurs = 1.0 - np.abs(2.0 * fbm(x, z, 170.0, 223, 3) - 1.0)
    mountains = mask * (20.0 + 78.0 * crests ** 1.8 + 30.0 * crests * spurs ** 1.5)
    detail = (fbm(x, z, 60.0, 307, 3) - 0.5) * 3.0
    height = -24.0 + rolling + hills + mountains + detail

    # Lowland around the start; lakes keep their own surroundings so the
    # alpine lake stays ringed by peaks.
    lowland = bump(x, z, START[0], START[1], 700.0)
    valley = -27.0 + rolling * 0.4 + hills * 0.6 + detail
    height = height * (1.0 - lowland) + valley * lowland
    for index, (cx, cz, radius) in enumerate(LAKES):
        # Irregular shorelines: warp the coordinates before measuring distance.
        wx = x + (fbm(x, z, 420.0, 401 + index * 7, 3) - 0.5) * 2.0 * radius * 0.55
        wz = z + (fbm(x, z, 420.0, 433 + index * 7, 3) - 0.5) * 2.0 * radius * 0.55
        d = np.sqrt((wx - cx) ** 2 + ((wz - cz) * 1.25) ** 2) / radius
        basin = 1.0 - smoothstep(0.45, 1.0, d)
        floor = WATER_LEVEL - 3.0 - 13.0 * (1.0 - smoothstep(0.0, 0.7, d))
        height = height * (1.0 - basin) + np.minimum(height, floor) * basin
    # A dry spot to stand on at the start.
    pad = bump(x, z, START[0], START[1], 45.0)
    height = height * (1.0 - pad) + np.maximum(height, WATER_LEVEL + 7.0) * pad
    return np.clip(height, -120.0, 124.0), origin


def rock_class(height):
    gz, gx = np.gradient(height, SPACING)
    slope = 1.0 - 1.0 / np.sqrt(1.0 + gx * gx + gz * gz)
    cls = np.zeros(height.shape, np.uint8)
    cls[slope > 0.13] = 1
    cls[slope > 0.19] = 2
    cls[slope > 0.26] = 3
    cls[height < WATER_LEVEL + 1.0] = 0
    return cls


def write_archive(height, codes):
    DATA.mkdir(parents=True, exist_ok=True)
    quantized = np.clip(np.rint(height * SCALE), -32768, 32767).astype("<i2")
    side = 2 * EXTENT + 1
    with open(DATA / "world.heights", "wb") as file:
        file.write(struct.pack("<QIQiiif", ARCHIVE_MAGIC, 2, SIGNATURE, EXTENT,
                               CELLS, CELLS, SCALE))
        for cell_z in range(side):
            for cell_x in range(side):
                r0, c0 = cell_z * CELLS, cell_x * CELLS
                block = quantized[r0:r0 + CELLS + 1, c0:c0 + CELLS + 1]
                file.write(block.tobytes())
                flat = codes[r0:r0 + CELLS + 1, c0:c0 + CELLS + 1].reshape(-1)
                padded = np.zeros((flat.size + 3) // 4 * 4, np.uint8)
                padded[:flat.size] = flat
                quads = padded.reshape(-1, 4)
                packed = (quads[:, 0] | (quads[:, 1] << 2) | (quads[:, 2] << 4) |
                          (quads[:, 3] << 6)).astype(np.uint8)
                file.write(packed.tobytes())
    vertex_side = side + 1
    corners = quantized[::CELLS, ::CELLS]
    assert corners.shape == (vertex_side, vertex_side)
    with open(DATA / "world.fargrid", "wb") as file:
        file.write(struct.pack("<QIQiiii", FARGRID_MAGIC, 2, SIGNATURE, EXTENT,
                               vertex_side, CELLS, CELLS))
        file.write(np.ascontiguousarray(corners).tobytes())
        file.write(struct.pack("<I", 0))


def sample(height, origin, x, z):
    col = int(round((x - origin) / SPACING))
    row = int(round((z - origin) / SPACING))
    return float(height[row, col])


def numbers(values):
    return " ".join(f"{v:.6g}" for v in values)


def find_item(objects, name):
    for item in objects.findall("item"):
        if item.get("name") == name:
            return item
    raise SystemExit(f"{SOURCE_SCENE.name} has no object named {name!r}")


def set_vector(parent, tag, values):
    element = parent.find(tag)
    for child in list(element):
        element.remove(child)
    element.set("_arrayEncoding", "spaceSeparatedNumbers")
    element.text = numbers(values)


def write_scene(height, origin):
    ET.register_namespace("components", NAMESPACE)
    source = ET.parse(SOURCE_SCENE).getroot()
    source_objects = source.find("objects")
    c = "{%s}" % NAMESPACE

    scene = ET.Element("Scene", {"version": "1"})
    settings = source.find("settings")
    settings.set("showGrid", "false")
    # The source scene's distance texture LOD adds up to +4 mips of bias past
    # 48 m, which blurs every tree a little way off. Normal mip selection is
    # enough here.
    settings.set("textureLodEnabled", "false")
    scene.append(settings)
    objects = ET.SubElement(scene, "objects")

    ground = sample(height, origin, *START)
    explorer = find_item(source_objects, "Meadow Explorer")
    explorer.set("name", "Landscape Explorer")
    # Start in the air above the meadow pad, looking over the first lake.
    set_vector(explorer.find("transform"), "Transform.position",
               (START[0], ground + 25.0, START[1]))
    camera = explorer.find("components").find(c + "Camera")
    camera.set("Camera.far", "9000")
    # Look across the first lake toward the ranges.
    set_vector(camera, "Camera.target", (LAKES[0][0], ground - 20.0, LAKES[0][1]))
    # Free flight: WASD, Space up, Shift down, Ctrl boost.
    controller = explorer.find("components").find(c + "FirstPersonController")
    controller.set("FirstPersonController.moveSpeed", "25")
    controller.set("FirstPersonController.sprintMultiplier", "4")
    controller.set("FirstPersonController.flightMode", "true")
    controller.set("FirstPersonController.flightResponse", "3")
    controller.set("FirstPersonController.flightGroundClearance", "1.5")
    # Skim the lakes rather than dive to their beds.
    controller.set("FirstPersonController.flightMinimumHeight", str(WATER_LEVEL + 1.2))
    body = explorer.find("components").find(c + "RigidBody")
    body.set("RigidBody.useGravity", "false")
    objects.append(explorer)

    sun = find_item(source_objects, "Late afternoon sun")
    # The low sun grazes the meadows: the source scene's tiny normal bias
    # left them banded with shadow acne from about 20 m out.
    for light in sun.iter(c + "Light"):
        light.set("Light.shadowNormalBias", "0.06")
    objects.append(sun)

    terrain = ET.SubElement(objects, "item", {"name": "Landscape", "enabled": "true"})
    transform = ET.SubElement(terrain, "transform")
    for tag, values in (("Transform.position", (0, 0, 0)), ("Transform.rotation", (0, 0, 0)),
                        ("Transform.scale", (1, 1, 1))):
        ET.SubElement(transform, tag, {"_arrayEncoding": "spaceSeparatedNumbers"}).text = numbers(values)
    components = ET.SubElement(terrain, "components")
    ET.SubElement(components, c + "Mesh", {"Mesh.file": "", "Mesh.useLightProbes": "false"})
    ET.SubElement(components, c + "Material", {
        "Material.useVertexColors": "true", "Material.specularModel": "MetallicRoughness",
        "Material.alphaMode": "Opaque", "Material.doubleSided": "false"})
    manager = ET.SubElement(components, c + "TerrainManager", {
        "TerrainManager." + key: value for key, value in {
            "cellsX": str(CELLS), "cellsY": str(CELLS),
            "cellWidth": numbers([SPACING]), "cellDepth": numbers([SPACING]),
            # Five cells (about 700 m) of real terrain and trees around the
            # viewer; mesh detail halves beyond two cells and again beyond three.
            "loadedRadius": "5", "editorRadius": "3", "farRadius": str(EXTENT),
            "lod2Radius": "2", "lod4Radius": "3", "farTileScreenErrorPixels": "6",
            # Trees beyond the loaded cells, as impostor blocks, to the map edge.
            "farObjectRadius": str(EXTENT),
            "maxLodChangesPerUpdate": "4", "maxLoadsPerUpdate": "6",
            "maxUnloadsPerUpdate": "4", "asyncStreaming": "true",
            "maxPreparedCells": "4", "maxUploadsPerUpdate": "4",
            "batchFarCells": "true", "fastRuntimeLighting": "true",
            "stochasticTerrainVariation": "true", "terrainMacroColorEnabled": "true",
            "reliefMapping": "true", "reliefShadows": "false",
            "measurementGrid": "false", "generateColliders": "true",
            "streamCollidersByDistance": "true", "cpuBudgetMiB": "512",
            "gpuBudgetMiB": "256", "cacheBudgetMiB": "1024",
            "viewerObjectName": "Landscape Explorer",
            "cellDataDirectory": "Engine/Core/Assets/TerrainData/Landscape",
            "runtimeBakeDirectory": "", "requireRuntimeBake": "false"}.items()})
    ET.SubElement(manager, "TerrainManager.cells")
    names = ET.SubElement(manager, "TerrainManager.objectLayerNames")
    for name in ("Landscape forest", "Procedural meadow grass", "Landscape props"):
        ET.SubElement(names, "item").text = name
    runs = ET.SubElement(manager, "TerrainManager.proceduralRuns")
    for y in range(-EXTENT, EXTENT + 1):
        ET.SubElement(runs, "item", {"y": str(y), "start": str(-EXTENT), "end": str(EXTENT)})
    ET.SubElement(manager, "TerrainManager.editedCells")
    ET.SubElement(components, c + "LandscapeDriver", {
        "LandscapeDriver.viewerObjectName": "Landscape Explorer",
        "LandscapeDriver.mapHalfExtentCells": str(EXTENT),
        "LandscapeDriver.heightArchive": "Assets/TerrainData/Landscape/world.heights",
        "LandscapeDriver.farGrid": "Assets/TerrainData/Landscape/world.fargrid",
        "LandscapeDriver.cacheDirectory": ".local/cache/landscape_demo",
        "LandscapeDriver.enableFarTerrain": "true",
        "LandscapeDriver.waterLevel": numbers([WATER_LEVEL]),
        "LandscapeDriver.treesPerCell": "220",
        "LandscapeDriver.forestLayerName": "Landscape forest",
        "LandscapeDriver.grassRadius": "26"})
    ET.SubElement(terrain, "children")

    ET.indent(scene, space="  ")
    header = ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!--\n"
              "  Landscape demo: a 5.2 km streamed map with mountain ranges, oak forests and\n"
              "  four lake basins (no water yet: sand beds, gravel shores).\n"
              "  Generated by tools/build_landscape_demo.py; re-running replaces this file.\n-->\n")
    SCENE.write_text(header + ET.tostring(scene, encoding="unicode") + "\n", encoding="utf-8")


def write_preview(path, height, codes):
    import zlib
    land = smoothstep(WATER_LEVEL + 1.5, WATER_LEVEL + 4.0, height)
    shore = smoothstep(WATER_LEVEL - 3.5, WATER_LEVEL - 1.0, height)
    rock = land * np.maximum(codes / 3.0, smoothstep(50.0, 66.0, height))
    snow = land * smoothstep(96.0, 108.0, height)
    colors = {"sand": (0.76, 0.66, 0.46), "gravel": (0.5, 0.5, 0.47), "grass": (0.3, 0.5, 0.2),
              "rock": (0.42, 0.4, 0.38), "snow": (0.95, 0.96, 0.98)}
    image = np.zeros(height.shape + (3,))
    for name, weight in (("sand", 1 - shore), ("gravel", shore * (1 - land)),
                         ("grass", land * (1 - rock) * (1 - snow)),
                         ("rock", rock * (1 - snow)), ("snow", snow)):
        image += weight[..., None] * np.array(colors[name])
    gz, gx = np.gradient(height, SPACING)
    image *= np.clip(0.75 + (gx - gz) * 0.6, 0.35, 1.25)[..., None]
    data = (np.clip(image[::-1], 0, 1) * 255).astype(np.uint8)  # +z up
    raw = b"".join(b"\x00" + row.tobytes() for row in data)
    chunk = lambda tag, body: (struct.pack(">I", len(body)) + tag + body +
                               struct.pack(">I", zlib.crc32(tag + body)))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(
        ">IIBBBBB", data.shape[1], data.shape[0], 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preview", help="also write a top-down PNG preview")
    arguments = parser.parse_args()
    height, origin = build_heights()
    codes = rock_class(height)
    write_archive(height, codes)
    write_scene(height, origin)
    if arguments.preview:
        write_preview(arguments.preview, height, codes)
    water = float((height < WATER_LEVEL).mean() * 100)
    print(f"map {height.shape[1] * SPACING / 1000:.1f} km, heights {height.min():.1f}..{height.max():.1f} m, "
          f"lake area {water:.1f}%, rock class 3 {float((codes == 3).mean() * 100):.1f}%, "
          f"start ground {sample(height, origin, *START):.1f} m")


if __name__ == "__main__":
    main()
