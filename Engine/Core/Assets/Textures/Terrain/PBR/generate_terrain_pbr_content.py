"""Writes the CC0 terrain layer materials and the terrain_materials test scene.

Texture sets are ambientCG (CC0 1.0, https://ambientcg.com), 2K JPG:
Grass004, Ground054 (sand), Ground048 (forest soil), Gravel022, Rock030,
Snow006. Each folder keeps Color, NormalGL, Roughness, AmbientOcclusion and
Displacement.

The scene is a 64 m standalone Terrain: rolling ground, a steep ridge for the
biplanar cliff projection, and painted patches whose transitions exercise the
height-lerp blend. Run from anywhere; paths are relative to this file.
"""

import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
ASSETS = HERE.parents[2]           # Engine/Core/Assets
MATERIALS = ASSETS / "Materials" / "Terrain" / "PBR"
SCENE = ASSETS / "Scenes" / "terrain" / "terrain_materials.scene"

# name, folder, uv scale, height-blend strength, roughness factor, preview color
LAYERS = [
    ("sand", "Ground054", 1.0, 0.6, 1.0, (0.62, 0.55, 0.42)),
    ("grass", "Grass004", 1.0, 0.4, 1.0, (0.24, 0.32, 0.12)),
    ("forest_soil", "Ground048", 1.0, 0.6, 1.0, (0.22, 0.15, 0.10)),
    ("gravel", "Gravel022", 1.5, 1.0, 1.0, (0.45, 0.43, 0.40)),
    ("rock", "Rock030", 0.5, 1.0, 1.0, (0.40, 0.38, 0.34)),
    ("snow", "Snow006", 0.5, 0.5, 1.0, (0.85, 0.88, 0.92)),
]


def write_materials():
    MATERIALS.mkdir(parents=True, exist_ok=True)
    for name, folder, uv_scale, blend, roughness, preview in LAYERS:
        prefix = f"Assets/Textures/Terrain/PBR/{folder}/{folder}"
        material = {
            "version": 1,
            "baseColor": list(preview),
            "metallic": 0,
            "roughness": roughness,
            "normalScale": 1,
            "terrainHeightBlendStrength": blend,
            "terrainUvScale": uv_scale,
            "baseColorTexture": f"{prefix}_Color.jpg",
            "normalTexture": f"{prefix}_NormalGL.jpg",
            "metallicRoughnessTexture": f"{prefix}_Roughness.jpg",
            "occlusionTexture": f"{prefix}_AmbientOcclusion.jpg",
            "heightTexture": f"{prefix}_Displacement.jpg",
        }
        (MATERIALS / f"{name}.material").write_text(
            json.dumps(material, indent=4) + "\n", encoding="utf-8")


def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def noise(x, z, seed):
    # Cheap smooth value noise from summed sines; deterministic.
    return (math.sin(x * 0.21 + seed) * math.cos(z * 0.17 - seed * 1.3) +
            0.5 * math.sin(x * 0.53 - z * 0.41 + seed * 2.1))


def terrain_data(cells, cell_size):
    heights, weights = [], []
    count = cells + 1
    for row in range(count):
        for col in range(count):
            x, z = col * cell_size, row * cell_size
            h = 1.2 * noise(x, z, 0.7) + 0.6 * noise(x * 1.9, z * 1.7, 2.3)
            # Ridge across the back: a 7 m cliff for the side projections.
            ridge = smoothstep(40.0, 46.0, z) * (7.0 + 1.5 * noise(x, z, 4.1))
            # A shallow sandy basin at the front left.
            basin = -1.5 * math.exp(-(((x - 14) / 9) ** 2 + ((z - 14) / 8) ** 2))
            heights.append(h + ridge + basin)
    for row in range(count):
        for col in range(count):
            i = row * count + col
            x, z = col * cell_size, row * cell_size
            h = heights[i]
            left = heights[row * count + max(col - 1, 0)]
            right = heights[row * count + min(col + 1, cells)]
            down = heights[max(row - 1, 0) * count + col]
            up = heights[min(row + 1, cells) * count + col]
            nx, nz = (left - right) / (2 * cell_size), (down - up) / (2 * cell_size)
            slope = 1.0 - 1.0 / math.sqrt(1 + nx * nx + nz * nz)  # 0 flat
            w = [0.0] * 6
            w[0] = smoothstep(-0.6, -1.4, h)                      # sand in the basin
            w[1] = 1.0                                            # grass base
            w[2] = smoothstep(0.2, 0.9, noise(x * 0.8, z * 0.8, 5.5))  # soil patches
            w[3] = smoothstep(0.4, 1.0, noise(x * 1.3, z * 0.6, 8.2)) * 0.9  # gravel path
            w[4] = smoothstep(0.15, 0.35, slope) * 2.0            # rock on steep faces
            w[5] = smoothstep(6.0, 8.0, h) * 2.5                  # snow on the ridge top
            if w[0] > 0.0:
                w[1] *= 1.0 - w[0]
            total = sum(w)
            weights.extend(v / total for v in w)
    return heights, weights


def numbers(values):
    return " ".join(f"{v:.4g}" for v in values)


def write_scene():
    cells, cell_size = 64, 1.0
    heights, weights = terrain_data(cells, cell_size)
    layers = "\n".join(
        f'                <item material="Assets/Materials/Terrain/PBR/{name}.material" '
        f'minHeight="-100" maxHeight="100">\n'
        f'                  <color _arrayEncoding="spaceSeparatedNumbers">{numbers(preview)}</color>\n'
        f'                </item>'
        for name, _, _, _, _, preview in LAYERS)
    scene = f'''<?xml version="1.0" encoding="utf-8"?>
<!--
  Terrain material test (Phase 3 of .md/surface-detail-materials-plan.md).
  Standalone 64 m Terrain with six CC0 PBR layers (ambientCG) packed into
  texture arrays: hex-tiling near, height-lerp transitions, biplanar cliff
  projection on the back ridge, distance detail blend toward the horizon.
  Generated by Assets/Textures/Terrain/PBR/generate_terrain_pbr_content.py.
-->
<Scene xmlns:components="urn:engine-components" version="1">
  <settings dimension="0" showGrid="false" gridHalfSize="10" gridCellSize="1" gridOpacity="0.25" gridFadeDistance="80" skyboxEnabled="true" skyboxTexture="Engine/Core/Assets/Textures/Skyboxes/editor-default-sky.png" hdriLightingEnabled="true" hdriIntensity="1" hdriExposure="0" hdriRotation="0" toneMapping="1" outputExposure="0" renderMode="0" sceneViewUiOverlay="false">
    <gridColor _arrayEncoding="spaceSeparatedNumbers">0.3 0.3 0.35</gridColor>
    <gridOriginColor _arrayEncoding="spaceSeparatedNumbers">0.4 0.5 0.8</gridOriginColor>
    <ambientColor _arrayEncoding="spaceSeparatedNumbers">0.03 0.03 0.03</ambientColor>
  </settings>
  <objects>
    <item name="Camera" enabled="true">
      <transform>
        <Transform.position _arrayEncoding="spaceSeparatedNumbers">20 3.2 4</Transform.position>
        <Transform.rotation _arrayEncoding="spaceSeparatedNumbers">0.2 0.25 0</Transform.rotation>
        <Transform.scale _arrayEncoding="spaceSeparatedNumbers">1 1 1</Transform.scale>
      </transform>
      <components>
        <components:Camera Camera.active="true" Camera.useTransformRotation="true" Camera.fov="60" Camera.near="0.05" Camera.far="400" Camera.orthographic="false" Camera.orthographicSize="5">
          <Camera.target _arrayEncoding="spaceSeparatedNumbers">0 1 0</Camera.target>
          <Camera.up _arrayEncoding="spaceSeparatedNumbers">0 1 0</Camera.up>
        </components:Camera>
      </components>
      <children />
    </item>
    <item name="Afternoon Sun" enabled="true">
      <transform>
        <Transform.position _arrayEncoding="spaceSeparatedNumbers">30 20 0</Transform.position>
        <Transform.rotation _arrayEncoding="spaceSeparatedNumbers">0.6 -0.9 0</Transform.rotation>
        <Transform.scale _arrayEncoding="spaceSeparatedNumbers">1 1 1</Transform.scale>
      </transform>
      <components>
        <components:Light Light.lightType="1" Light.intensity="3" Light.range="10" Light.falloff="1.4" Light.intensityMode="0" Light.innerConeAngle="25" Light.outerConeAngle="35" Light.lightingChannels="-1" Light.cookieTexture="" Light.iesProfileTexture="" Light.cookieScale="1" Light.baked="false" Light.castsShadows="true" Light.shadowStrength="1" Light.shadowDepthBias="0.0015" Light.shadowNormalBias="0.01" Light.shadowResolutionScale="1" Light.shadowFilterScale="1">
          <Light.color _arrayEncoding="spaceSeparatedNumbers">1 0.94 0.84</Light.color>
        </components:Light>
      </components>
      <children />
    </item>
    <item name="Terrain" enabled="true">
      <transform>
        <Transform.position _arrayEncoding="spaceSeparatedNumbers">0 0 0</Transform.position>
        <Transform.rotation _arrayEncoding="spaceSeparatedNumbers">0 0 0</Transform.rotation>
        <Transform.scale _arrayEncoding="spaceSeparatedNumbers">1 1 1</Transform.scale>
      </transform>
      <components>
        <components:Mesh Mesh.file="" Mesh.useLightProbes="false" />
        <components:Terrain Terrain.cellsX="{cells}" Terrain.cellsY="{cells}" Terrain.cellWidth="{cell_size}" Terrain.cellDepth="{cell_size}" Terrain.textureTiling="32" Terrain.bakedAssetPath="">
          <Terrain.heights _arrayEncoding="spaceSeparatedNumbers">{numbers(heights)}</Terrain.heights>
          <Terrain.weights _arrayEncoding="spaceSeparatedNumbers">{numbers(weights)}</Terrain.weights>
          <Terrain.layers>
{layers}
          </Terrain.layers>
        </components:Terrain>
        <components:Material Material.useVertexColors="true" Material.specularModel="MetallicRoughness" Material.alphaMode="Opaque" Material.doubleSided="false" />
      </components>
      <children />
    </item>
  </objects>
</Scene>
'''
    SCENE.write_text(scene, encoding="utf-8")


if __name__ == "__main__":
    write_materials()
    write_scene()
    print("wrote", MATERIALS, "and", SCENE)
