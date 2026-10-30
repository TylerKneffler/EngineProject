"""Build two reusable, texture-free grass patches for the engine OBJ loader.

Each blade is real tapered geometry with per-vertex color. The patches are
deliberately loose rather than radial tufts so repeated instances overlap.
Run from any directory: python Scripts/generate_grass_test_meshes.py
"""

from __future__ import annotations

import math
import random
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "Engine/Core/Assets/Mesh/Grass"


def normalize(x: float, y: float, z: float) -> tuple[float, float, float]:
    length = math.sqrt(x * x + y * y + z * z)
    return x / length, y / length, z / length


def make_patch(name: str, seed: int, count: int, source_count: int,
               radius: float,
               height_range: tuple[float, float], width_range: tuple[float, float],
               dry_fraction: float) -> tuple[int, int]:
    rng = random.Random(seed)
    vertices: list[tuple[float, float, float, float, float, float]] = []
    normals: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []
    faces: list[tuple[int, int, int]] = []

    for blade in range(source_count):
        # Uniform disk positions avoid the dense center characteristic of tufts.
        angle = rng.uniform(0.0, 2.0 * math.pi)
        distance = radius * math.sqrt(rng.random())
        root_x, root_z = distance * math.cos(angle), distance * math.sin(angle)
        heading = rng.uniform(0.0, 2.0 * math.pi)
        dx, dz = math.cos(heading), math.sin(heading)
        sx, sz = -dz, dx
        height = rng.uniform(*height_range)
        width = rng.uniform(*width_range)
        lean = rng.uniform(0.08, 0.35) * height
        sideways = rng.uniform(-0.13, 0.13) * height
        dry = rng.random() < dry_fraction
        shade = rng.uniform(0.78, 1.16)
        if dry:
            base_color = (0.31, 0.30, 0.08)
            tip_color = (0.68, 0.58, 0.23)
        else:
            base_color = (0.075, 0.19, 0.035)
            tip_color = (0.34, 0.48, 0.10)

        # LODs retain a stable, spread-out subset of the same authored blades.
        if (blade + 1) * count // source_count == blade * count // source_count:
            continue

        # Six stations give each blade a curved profile and a narrow tip.
        start = len(vertices) + 1
        for station in range(6):
            t = station / 5.0
            curved = t ** 1.7
            cx = root_x + dx * lean * curved + sx * sideways * t * t
            cz = root_z + dz * lean * curved + sz * sideways * t * t
            cy = height * t
            half_width = 0.5 * width * (0.55 + 0.55 * math.sin(math.pi * t)) * (1 - t) ** 0.8
            if station == 5:
                half_width = 0.0005
            # A mild lengthwise twist keeps the strips from appearing flat.
            twist = 0.28 * math.sin(t * math.pi) * (1 if blade % 2 else -1)
            side_x = sx * math.cos(twist) + dx * math.sin(twist)
            side_z = sz * math.cos(twist) + dz * math.sin(twist)
            normal = normalize(-side_z, 0.22 + 0.20 * t, side_x)
            color = tuple(min(1.0, shade * (a * (1 - t) + b * t))
                          for a, b in zip(base_color, tip_color))
            for sign, u in ((-1, 0.0), (1, 1.0)):
                vertices.append((cx + sign * half_width * side_x, cy,
                                 cz + sign * half_width * side_z, *color))
                normals.append(normal)
                uvs.append((u, t))
        for segment in range(5):
            a = start + segment * 2
            faces.append((a, a + 2, a + 1))
            faces.append((a + 1, a + 2, a + 3))

    OUTPUT.mkdir(parents=True, exist_ok=True)
    path = OUTPUT / f"{name}.obj"
    with path.open("w", encoding="ascii", newline="\n") as file:
        file.write(f"# {name}: {count} scattered curved blades; meters, Y up\n")
        file.write("# Set Material.useVertexColors=true and doubleSided=true.\n")
        for x, y, z, r, g, b in vertices:
            file.write(f"v {x:.6f} {y:.6f} {z:.6f} {r:.5f} {g:.5f} {b:.5f}\n")
        for u, v in uvs:
            file.write(f"vt {u:.5f} {v:.5f}\n")
        for x, y, z in normals:
            file.write(f"vn {x:.6f} {y:.6f} {z:.6f}\n")
        for a, b, c in faces:
            file.write("f " + " ".join(f"{i}/{i}/{i}" for i in (a, b, c)) + "\n")
    return len(vertices), len(faces)


if __name__ == "__main__":
    for spec in (
        ("meadow_short", 42311, 54, 54, 0.50, (0.20, 0.39), (0.021, 0.039), 0.10),
        ("meadow_short_lod1", 42311, 30, 54, 0.50, (0.20, 0.39), (0.021, 0.039), 0.10),
        ("meadow_short_lod2", 42311, 14, 54, 0.50, (0.20, 0.39), (0.021, 0.039), 0.10),
        ("field_tall", 91277, 48, 48, 0.54, (0.39, 0.78), (0.024, 0.046), 0.18),
        ("field_tall_lod1", 91277, 26, 48, 0.54, (0.39, 0.78), (0.024, 0.046), 0.18),
        ("field_tall_lod2", 91277, 12, 48, 0.54, (0.39, 0.78), (0.024, 0.046), 0.18),
    ):
        vertex_count, triangle_count = make_patch(*spec)
        print(f"{spec[0]}: {vertex_count} vertices, {triangle_count} triangles")
