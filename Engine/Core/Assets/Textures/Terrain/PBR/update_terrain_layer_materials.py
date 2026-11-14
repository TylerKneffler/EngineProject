"""Points the existing terrain layer materials at the CC0 PBR sets.

The streamed and test scenes (oak_meadow, oak_meadow_expanded,
terrain_performance, grass_* tests) reference Materials/Terrain/*.material
and Materials/Woodland/{Meadow,Path,WoodlandFloor}.material, so updating those
files moves every scene onto the new layers without editing the scenes.

Several old materials share one photo set (five grasses -> Grass004). Each
keeps a distinct look through baseColorTextureTint, which the terrain array
builder bakes into the layer's slice. The tint moves the set's mean color
halfway (in log space) toward the old material's measured mean, so palette
differences survive without dragging the photos to the old saturated values.
terrainMacroColor is set to the tinted mean so far terrain matches.

Requires Pillow and numpy. Run from anywhere.
"""

import json
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
ASSETS = HERE.parents[2]
MATERIALS = ASSETS / "Materials"

# Photo set settings: height-blend strength, uv scale (2 m base repeat),
# constant roughness replacing the set's roughness map (None keeps the map),
# and near-camera relief depth in texture repeats (heightScale).
# Grass004's map averages 0.26 (single glossy blades); a ground of blades is
# rough at terrain scale, so the layer uses a constant 0.9 instead.
SETS = {
    "Grass004": (0.4, 1.0, 0.9, 0.008),    # 1.6 cm over a 2 m repeat
    "Ground048": (0.6, 1.0, None, 0.012),  # 2.4 cm
    "Ground054": (0.6, 1.0, None, 0.01),   # 2 cm
    "Gravel022": (1.0, 1.5, None, 0.02),   # 2.7 cm over 1.33 m
    "Rock030": (1.0, 0.5, None, 0.015),    # 6 cm over 4 m
    "Snow006": (0.5, 0.5, None, 0.008),    # 3.2 cm over 4 m
}

# material -> (photo set, old mean linear color to lean toward, or None)
LAYERS = {
    "Terrain/alpine_snow": ("Snow006", None),
    "Terrain/dark_soil": ("Ground048", None),
    "Terrain/warm_dirt": ("Ground048", (0.19217, 0.11348, 0.05328)),
    "Terrain/shaded_moss": ("Grass004", (0.04561, 0.07992, 0.03262)),
    "Terrain/field_grass": ("Grass004", (0.13832, 0.22583, 0.04636)),
    "Terrain/highland_grass": ("Grass004", (0.21779, 0.252, 0.07456)),
    "Terrain/meadow_grass": ("Grass004", (0.07028, 0.17221, 0.03527)),
    "Terrain/valley_grass": ("Grass004", (0.04927, 0.15253, 0.03096)),
    "Terrain/woodland_grass": ("Grass004", (0.0427, 0.0944, 0.02921)),
    "Terrain/shore_gravel": ("Gravel022", None),
    "Terrain/shore_sand": ("Ground054", None),
    "Terrain/weathered_slate": ("Rock030", None),
    "Woodland/Meadow": ("Grass004", (0.07028, 0.17221, 0.03527)),
    "Woodland/Path": ("Ground048", (0.19217, 0.11348, 0.05328)),
    "Woodland/WoodlandFloor": ("Ground048", None),
}


def linear_mean(path):
    rgb = np.asarray(Image.open(path).convert("RGB")).astype(np.float64) / 255.0
    linear = np.where(rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4)
    return linear.reshape(-1, 3).mean(axis=0)


def main():
    means = {name: linear_mean(HERE / name / f"{name}_Color.jpg") for name in SETS}
    for material_name, (set_name, old_mean) in LAYERS.items():
        path = MATERIALS / f"{material_name}.material"
        data = json.loads(path.read_text(encoding="utf-8"))
        mean = means[set_name]
        tint = np.ones(3)
        if old_mean is not None:
            tint = np.clip(np.sqrt(np.asarray(old_mean) / mean), 0.5, 2.0)
        blend, uv_scale, constant_roughness, relief = SETS[set_name]
        prefix = f"Assets/Textures/Terrain/PBR/{set_name}/{set_name}"
        for key in ("normalTexture", "metallicRoughnessTexture",
                    "occlusionTexture", "heightTexture"):
            data.pop(key, None)
        data.update({
            "roughness": constant_roughness or 1,
            "heightScale": relief,
            "metallic": 0,
            "normalScale": 1,
            "terrainHeightBlendStrength": blend,
            "terrainUvScale": uv_scale,
            "terrainMacroColor": [round(float(v), 5) for v in np.clip(mean * tint, 0, 1)],
            "baseColorTextureTint": [round(float(v), 4) for v in tint],
            "baseColorTexture": f"{prefix}_Color.jpg",
            "normalTexture": f"{prefix}_NormalGL.jpg",
            "metallicRoughnessTexture": f"{prefix}_Roughness.jpg",
            "occlusionTexture": f"{prefix}_AmbientOcclusion.jpg",
            "heightTexture": f"{prefix}_Displacement.jpg",
        })
        if constant_roughness is not None:
            del data["metallicRoughnessTexture"]
        path.write_text(json.dumps(data, indent=4) + "\n", encoding="utf-8")
        print(f"{material_name:24s} {set_name:10s} tint {np.round(tint, 3)} macro {np.round(mean * tint, 3)}")


if __name__ == "__main__":
    main()
