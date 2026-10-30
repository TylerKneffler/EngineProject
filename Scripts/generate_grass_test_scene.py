"""Regenerate the small flat-terrain grass test scene and editor preview."""

from pathlib import Path
import math
import random
from xml.sax.saxutils import escape


ROOT = Path(__file__).resolve().parents[1]
SCENE = ROOT / "Engine/Core/Assets/Scenes/terrain/grass_flat_test.scene"


def transform(x: float, y: float, z: float, scale: float = 1.0,
              rx: float = 0.0, ry: float = 0.0, rz: float = 0.0) -> str:
    return ("<transform><Transform.position>"
            f"<item>{x:.3f}</item><item>{y:.3f}</item><item>{z:.3f}</item>"
            "</Transform.position><Transform.rotation>"
            f"<item>{rx:.6f}</item><item>{ry:.6f}</item><item>{rz:.6f}</item>"
            "</Transform.rotation><Transform.scale>"
            f"<item>{scale:.3f}</item><item>{scale:.3f}</item><item>{scale:.3f}</item>"
            "</Transform.scale></transform>")


def preview_patch(index: int, x: float, z: float, tall: bool,
                  scale: float) -> str:
    species = "field_tall" if tall else "meadow_short"
    return (f'        <item name="Preview {index:02d}" enabled="true">'
            + transform(x, 0, z, scale)
            + '<components>'
            + f'<components:Mesh Mesh.file="Assets/Mesh/Grass/{species}.obj" />'
            + '<components:Material Material.useVertexColors="true" '
              'Material.doubleSided="true" Material.castsShadows="true" '
              'Material.metallicFactor="0" Material.roughnessFactor="1" '
              f'Material.materialAsset="Assets/Materials/Grass/{species}.material">'
              '<Material.diffuse><item>1</item><item>1</item><item>1</item>'
              '</Material.diffuse></components:Material></components><children />'
              '</item>')


def build() -> str:
    rng = random.Random(812)
    placements = []
    # Jittered, overlapping short blades form a continuous meadow. Tall
    # blades are scattered accents instead of isolated, visible islands.
    for row in range(24):
        for column in range(24):
            x = 0.35 + column * 0.67 + rng.uniform(-0.22, 0.22)
            z = 0.35 + row * 0.67 + rng.uniform(-0.22, 0.22)
            if not (0.25 < x < 15.75 and 0.25 < z < 15.75):
                continue
            placements.append((x, z, False, rng.uniform(0.95, 1.28),
                               rng.uniform(0, 360)))
            if rng.random() < 0.18:
                placements.append((x + rng.uniform(-0.34, 0.34),
                                   z + rng.uniform(-0.34, 0.34), True,
                                   rng.uniform(0.72, 1.08),
                                   rng.uniform(0, 360)))
    # Show the same coverage in edit mode; runtime tiles batch these patches.
    preview = [preview_patch(index + 1, x, z, tall, scale)
               for index, (x, z, tall, scale, _) in enumerate(placements)
               ]
    records = []
    for x, z, tall, scale, yaw in placements:
        asset = "field_tall" if tall else "meadow_short"
        records.append(
            f'<item asset="Assets/Mesh/Grass/{escape(asset)}.obj" '
            f'yawDegrees="{yaw:.3f}" scale="{scale:.3f}">'
            f'<position><item>{x:.3f}</item><item>0</item>'
            f'<item>{z:.3f}</item></position>'
            '<normal><item>0</item><item>1</item><item>0</item></normal>'
            '</item>')
    return '''<?xml version="1.0" encoding="utf-8"?>
<Scene xmlns:components="urn:engine-components" version="1">
  <settings dimension="0" showGrid="false" skyboxEnabled="true" hdriLightingEnabled="false" hdriIntensity="1" toneMapping="1" outputExposure="0">
    <ambientColor><item>0.18</item><item>0.21</item><item>0.16</item></ambientColor>
  </settings>
  <objects>
    <item name="Grass Test Capsule" enabled="true">''' + transform(0, 1.05, -5) + '''
      <components>
        <components:Camera Camera.active="true" Camera.useTransformRotation="false" Camera.fov="70" Camera.near="0.06" Camera.far="80">
          <Camera.target><item>0</item><item>0</item><item>4</item></Camera.target>
          <Camera.up><item>0</item><item>1</item><item>0</item></Camera.up>
        </components:Camera>
        <components:FirstPersonController FirstPersonController.moveSpeed="3.5" FirstPersonController.sprintMultiplier="1.7" FirstPersonController.lookSensitivity="0.0025" FirstPersonController.lockCursor="true" />
        <components:PrimitiveObjectCollider PrimitiveObjectCollider.collisionEnabled="true" PrimitiveObjectCollider.shape="Capsule" PrimitiveObjectCollider.radius="0.35" PrimitiveObjectCollider.height="1.8" />
        <components:RigidBody RigidBody.bodyType="Dynamic" RigidBody.mass="75" RigidBody.useGravity="true" RigidBody.friction="0.8" RigidBody.restitution="0" RigidBody.continuousCollision="true" RigidBody.freezeRotationX="true" RigidBody.freezeRotationY="true" RigidBody.freezeRotationZ="true" />
      </components><children />
    </item>
    <item name="Grass Sun" enabled="true">''' + transform(
        4, 8, -3, rx=math.radians(35), ry=math.radians(170)) + '''
      <components><components:Light Light.lightType="1" Light.intensity="2.5" Light.castsShadows="true" Light.shadowStrength="1" Light.shadowDepthBias="0.0005" Light.shadowNormalBias="0.002" Light.shadowFilterScale="0.5">
        <Light.color><item>1</item><item>0.96</item><item>0.84</item></Light.color>
      </components:Light></components><children />
    </item>
    <item name="Flat Grass Terrain" enabled="true">''' + transform(-8, 0, -8) + '''
      <components>
        <components:Terrain Terrain.cellsX="16" Terrain.cellsY="16" Terrain.cellWidth="1" Terrain.cellDepth="1" Terrain.textureTiling="8">
          <Terrain.layers><item material="Assets/Materials/Terrain/meadow_grass.material" minHeight="-1" maxHeight="1"><color><item>0.12</item><item>0.23</item><item>0.06</item></color></item></Terrain.layers>
        </components:Terrain>
        <components:Material Material.useVertexColors="true" Material.metallicFactor="0" Material.roughnessFactor="1" Material.castsShadows="false">
          <Material.diffuse><item>1</item><item>1</item><item>1</item></Material.diffuse>
        </components:Material>
        <components:MeshObjectCollider MeshObjectCollider.collisionEnabled="true" MeshObjectCollider.convex="false" />
        <components:RigidBody RigidBody.bodyType="Static" RigidBody.useGravity="false" RigidBody.friction="0.9" />
      </components>
      <children>
        <item name="Grass Layer" enabled="true">''' + transform(0, 0, 0) + '''
          <components>
            <components:TerrainObjectLayer TerrainObjectLayer.layerName="Grass" TerrainObjectLayer.viewDistance="18" TerrainObjectLayer.lod1Distance="6" TerrainObjectLayer.lod2Distance="10" TerrainObjectLayer.lodHysteresis="0.75" TerrainObjectLayer.shadowDistance="8">
              <TerrainObjectLayer.placements>''' + "\n".join(records) + '''</TerrainObjectLayer.placements>
            </components:TerrainObjectLayer>
            <components:GrassField GrassField.viewerObjectName="Grass Test Capsule" GrassField.shortMesh="Assets/Mesh/Grass/meadow_short.obj" GrassField.tallMesh="Assets/Mesh/Grass/field_tall.obj" GrassField.visibleRadius="11" GrassField.tileSize="4" GrassField.maxTileBuildsPerUpdate="8" GrassField.castShadows="true" />
          </components>
          <children>
            <item name="Grass Preview" enabled="true">''' + transform(0, 0, 0) + '''
              <components /><children>
''' + "\n".join(preview) + '''
              </children>
            </item>
          </children>
        </item>
      </children>
    </item>
  </objects>
</Scene>
'''


if __name__ == "__main__":
    SCENE.write_text(build(), encoding="utf-8", newline="\n")
    print(SCENE)
