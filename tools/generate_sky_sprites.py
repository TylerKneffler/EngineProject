#!/usr/bin/env python3
"""Generate cloud and valley-mist sprites (noise-based) plus soft particle and
card prefabs for them.

Writes (under Engine/Core/Assets):
  Textures/Atmosphere/Cloud_Puff_{A,B}.ktx2     lit cumulus puffs (sun from the top)
  Textures/Atmosphere/Mist_Wisp_{A,B}.ktx2      flat, streaky low-contrast wisps
  Textures/Atmosphere/Mist_Sheet.ktx2           soft-edged sheet for mist cards
  Models/Woodland/Atmosphere/Meshes/MistCard.mesh
  Models/Woodland/Atmosphere/Materials/MistCard.material
  Prefabs/Woodland/Atmosphere/ValleyMist.prefab   ParticleEmitter: drifting ground mist
  Prefabs/Woodland/Atmosphere/LakeMist.prefab     ParticleEmitter: the same at lake scale
  Prefabs/Woodland/Atmosphere/LowClouds.prefab    ParticleEmitter: low cloud puffs on peaks
  Prefabs/Woodland/Atmosphere/MistCard.prefab     static horizontal mist card

Sprites are straight-alpha (Particle.hlsl multiplies colour by the texture and
alpha by its alpha). Colour is defined under every texel (no black fringe in
mips), and alpha fades to zero well inside the border. Particles are unlit,
so emitter colours are radiances: ground mist sits near the lit terrain's
level (~0.35) rather than white, or it glows against the scene. The emitters use the
soft-particle fade (ParticleEmitter.softness) so sprites meet terrain without a
hard line, a near-camera fade so the viewer can fly through, and a fade-in so
new sprites do not pop.

The mist card is an ordinary Blend-mode mesh lying 2 m above its pivot: place
it over lakes and flat valley floors (TerrainObjectLayer or by hand); unlike
the particles it has no depth fade, so keep it off slopes.

Usage: python Tools/generate_sky_sprites.py
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import woodland_assets as wa  # noqa: E402

TEXTURES = wa.ASSETS / "Textures" / "Atmosphere"
MODELS = wa.ASSETS / "Models" / "Woodland" / "Atmosphere"
PREFABS = wa.ASSETS / "Prefabs" / "Woodland" / "Atmosphere"
SOURCES = wa.WORK / "sky"


def radial(shape, cx=0.5, cy=0.5, sx=0.5, sy=0.5):
    h, w = shape
    y, x = np.meshgrid((np.arange(h) + 0.5) / h, (np.arange(w) + 0.5) / w, indexing="ij")
    return np.sqrt(((x - cx) / sx) ** 2 + ((y - cy) / sy) ** 2), x, y


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def cloud_puff(seed, size=512):
    """A cumulus puff: several overlapping blobs eroded by noise, lit from
    above (bright crowns, grey flat-ish base)."""
    rng = np.random.default_rng(seed)
    density = np.zeros((size, size))
    for _ in range(9):
        cx, cy = rng.uniform(0.3, 0.7), rng.uniform(0.38, 0.62)
        r = rng.uniform(0.13, 0.24)
        d, _, _ = radial((size, size), cx, cy, r, r * rng.uniform(0.8, 1.1))
        density = np.maximum(density, np.clip(1 - d, 0, 1) ** 0.7)
    # Flatter base: fade the lower part faster.
    _, x, y = radial((size, size))
    density *= smoothstep(0.86, 0.66, y)
    noise = wa.fbm_2d((size, size), (6, 6), 5, seed + 1)
    detail = wa.fbm_2d((size, size), (24, 24), 3, seed + 2)
    density = np.clip(density * 1.25 - 0.45 * (1 - noise) - 0.15 * (1 - detail), 0, 1)
    # Keep the border empty.
    d, _, _ = radial((size, size))
    density *= smoothstep(0.98, 0.8, d)
    alpha = smoothstep(0.02, 0.45, density)
    # Lighting: brighter where the density above is thin (toward image top).
    above = np.cumsum(density, axis=0) / size * 6.0
    light = np.exp(-above * 0.8)
    shade = 0.62 + 0.38 * light + 0.06 * (detail - 0.5)
    rgb = np.stack([shade * 1.0, shade * 1.0, shade * 1.02], axis=-1)
    rgb = wa.push_pull_fill(rgb, alpha)
    return np.clip(rgb, 0, 1), alpha


def mist_wisp(seed, width=512, height=256):
    """Long horizontal streaks of thin mist."""
    streak = wa.fbm_2d((height, width), (12, 3), 4, seed)
    detail = wa.fbm_2d((height, width), (24, 8), 3, seed + 1)
    d, x, y = radial((height, width), 0.5, 0.5, 0.48, 0.42)
    envelope = smoothstep(1.0, 0.25, d)
    density = np.clip((streak * 0.75 + detail * 0.35 - 0.38) * 2.2, 0, 1) * envelope
    alpha = smoothstep(0.0, 0.9, density) * 0.85
    shade = 0.88 + 0.12 * (1 - y) + 0.05 * (detail - 0.5)
    rgb = np.stack([shade, shade * 1.005, shade * 1.02], axis=-1)
    return np.clip(rgb, 0, 1), alpha


def mist_sheet(seed, size=1024):
    """Large soft sheet with holes and streaks, for the static mist card."""
    base = wa.fbm_2d((size, size), (4, 4), 5, seed)
    streak = wa.fbm_2d((size, size), (3, 10), 3, seed + 3)
    d, _, _ = radial((size, size))
    envelope = smoothstep(1.0, 0.45, d)
    density = np.clip((base * 0.7 + streak * 0.5 - 0.42) * 2.0, 0, 1) * envelope
    alpha = smoothstep(0.0, 1.0, density) * 0.25
    rgb = np.ones((size, size, 3)) * np.array([0.9, 0.91, 0.93])
    return rgb, alpha


def cloud_shadow(seed=6613, size=1024):
    """Tiling cloud-shadow cookie for the sun (Light.cookieTexture, linear):
    1 in clear sky, ~0.3 under cloud, ~40 % cover with soft edges. Scrolled
    with the wind by Light.cookieScrollX/Y (.md/foliage.md Phase 3)."""
    noise = wa.fbm_2d((size, size), (6, 6), 5, seed, gain=0.55, tile=True)
    noise = (noise - noise.min()) / max(noise.max() - noise.min(), 1e-6)
    cloud = smoothstep(0.42, 0.6, noise)
    return 1.0 - 0.7 * cloud


def write_cloud_shadow():
    """Writes Textures/Atmosphere/Cloud_Shadow.png (linear data, read as .r)."""
    path = TEXTURES / "Cloud_Shadow.png"
    shadow = cloud_shadow()
    wa.write_png(path, shadow)
    return path, shadow


def write_sprite(name, rgb, alpha):
    png = SOURCES / f"{name}.png"
    wa.write_png(png, np.concatenate([wa.linear_to_srgb(rgb), alpha[..., None]], axis=2))
    return wa.cook(png, TEXTURES / f"{name}.ktx2", "color")


def emitter_prefab(path: Path, name: str, attrs: dict, vectors: dict):
    attr_text = " ".join(f'ParticleEmitter.{k}="{v}"' for k, v in attrs.items())
    vec_text = "\n".join(
        f'          <ParticleEmitter.{k} _arrayEncoding="spaceSeparatedNumbers">{" ".join(f"{x:g}" for x in v)}'
        f"</ParticleEmitter.{k}>" for k, v in vectors.items())
    text = f"""<?xml version='1.0' encoding='utf-8'?>
<Prefab xmlns:components="urn:engine-components" version="1" type="prefab">
  <object name="{name}" enabled="true">
    <components>
        <components:ParticleEmitter {attr_text}>
{vec_text}
        </components:ParticleEmitter>
    </components>
    <children />
    <transform>
      <Transform.position _arrayEncoding="spaceSeparatedNumbers">0 0 0</Transform.position>
      <Transform.rotation _arrayEncoding="spaceSeparatedNumbers">0 0 0</Transform.rotation>
      <Transform.scale _arrayEncoding="spaceSeparatedNumbers">1 1 1</Transform.scale>
    </transform>
  </object>
</Prefab>
"""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def mist_card_mesh(path: Path, size=40.0, height=2.0, rings=6, sides=24):
    """A round, slightly domed horizontal sheet; vertex alpha fades to the rim."""
    mesh = wa.MeshBuilder()
    center = mesh.vertex((0, height, 0), (0, 1, 0), (0.5, 0.5), (1, 1, 1, 1))
    prev = None
    for r in range(1, rings + 1):
        t = r / rings
        ring = []
        for k in range(sides + 1):
            a = math.tau * k / sides
            x, z = math.cos(a) * t * size / 2, math.sin(a) * t * size / 2
            y = height + 0.6 * (1 - t * t)
            alpha = float(np.clip((1 - t) / 0.7, 0, 1)) ** 2.0
            ring.append(mesh.vertex((x, y, z), (0, 1, 0), (0.5 + 0.5 * t * math.cos(a), 0.5 + 0.5 * t * math.sin(a)),
                                    (1, 1, 1, alpha)))
        if prev is None:
            for k in range(sides):
                mesh.tri(center, ring[k + 1], ring[k])
        else:
            for k in range(sides):
                mesh.quad(prev[k], ring[k], ring[k + 1], prev[k + 1])
        prev = ring
    # Upward-facing winding: flip triangles whose normal points down.
    p = np.array(mesh.p)
    fixed = []
    for a, b, c in mesh.idx:
        n = np.cross(p[b] - p[a], p[c] - p[a])
        fixed.append((a, b, c) if n[1] >= 0 else (a, c, b))
    mesh.idx = fixed
    return mesh.write_native(path)


def main():
    path, shadow = write_cloud_shadow()
    print(f"Cloud_Shadow: mean light {shadow.mean():.2f}, cover {(shadow < 0.65).mean():.0%}")
    sprites = {}
    for name, (fn, seed) in {"Cloud_Puff_A": (cloud_puff, 6101), "Cloud_Puff_B": (cloud_puff, 6203),
                             "Mist_Wisp_A": (mist_wisp, 6307), "Mist_Wisp_B": (mist_wisp, 6409),
                             "Mist_Sheet": (mist_sheet, 6511)}.items():
        rgb, alpha = fn(seed)
        sprites[name] = write_sprite(name, rgb, alpha)
        print(f"{name}: {alpha.shape[1]}x{alpha.shape[0]}, mean alpha {alpha.mean():.2f}")

    # Ground mist: wide, low, slow; soft against the terrain and the camera.
    emitter_prefab(PREFABS / "ValleyMist.prefab", "Valley mist", {
        "rate": "1.2", "burstOnStart": "36", "maxParticles": "72", "lifetime": "45", "speed": "0.35",
        "spreadDegrees": "35", "drag": "0", "size": "16", "shape": "2", "worldSpace": "true",
        "playOnStart": "true", "duration": "0", "loop": "false", "seed": "4417",
        "texturePath": wa.asset_path(sprites["Mist_Wisp_A"]), "opacity": "0.1", "blendMode": "1",
        "softness": "6", "nearFadeDistance": "20", "fadeInTime": "8", "sizeVariation": "0.4",
        "randomRotation": "false", "collisionMode": "0", "useGpuSimulation": "false"},
        {"direction": (1, 0, 0.25), "gravity": (0, 0, 0), "color": (0.3, 0.32, 0.35),
         "boxExtents": (70, 3, 70)})
    # Lake mist: the same ground mist at landscape scale, for lakes and wide
    # valley floors seen from far off (scale the instance in x/z to the lake).
    emitter_prefab(PREFABS / "LakeMist.prefab", "Lake mist", {
        "rate": "0.6", "burstOnStart": "60", "maxParticles": "90", "lifetime": "120", "speed": "0.5",
        "spreadDegrees": "30", "drag": "0", "size": "90", "shape": "2", "worldSpace": "true",
        "playOnStart": "true", "duration": "0", "loop": "false", "seed": "4639",
        "texturePath": wa.asset_path(sprites["Mist_Wisp_B"]), "opacity": "0.14", "blendMode": "1",
        "softness": "20", "nearFadeDistance": "60", "fadeInTime": "15", "sizeVariation": "0.4",
        "randomRotation": "false", "collisionMode": "0", "useGpuSimulation": "false"},
        {"direction": (1, 0, 0.3), "gravity": (0, 0, 0), "color": (0.32, 0.34, 0.37),
         "boxExtents": (250, 5, 250)})
    # Low clouds clinging to the peaks.
    emitter_prefab(PREFABS / "LowClouds.prefab", "Low clouds", {
        "rate": "0.25", "burstOnStart": "28", "maxParticles": "48", "lifetime": "160", "speed": "1.2",
        "spreadDegrees": "20", "drag": "0", "size": "140", "shape": "2", "worldSpace": "true",
        "playOnStart": "true", "duration": "0", "loop": "false", "seed": "5521",
        "texturePath": wa.asset_path(sprites["Cloud_Puff_A"]), "opacity": "0.85", "blendMode": "1",
        "softness": "40", "nearFadeDistance": "90", "fadeInTime": "20", "sizeVariation": "0.35",
        "randomRotation": "true", "collisionMode": "0", "useGpuSimulation": "false"},
        {"direction": (1, 0, 0.1), "gravity": (0, 0, 0), "color": (0.7, 0.71, 0.74),
         "boxExtents": (500, 60, 500)})

    mesh_path = MODELS / "Meshes" / "MistCard.mesh"
    mist_card_mesh(mesh_path)
    material = wa.write_material(
        MODELS / "Materials" / "MistCard.material", baseColor=[0.5, 0.52, 0.55],
        baseColorTexture=wa.asset_path(sprites["Mist_Sheet"]), useVertexColors=True,
        alphaMode="Blend", doubleSided=True, castsShadows=False, roughness=1.0, metallic=0,
        reflectionStrength=0, environmentDiffuseStrength=1.0)
    wa.write_prefab(PREFABS / "MistCard.prefab", "Mist card", [
        dict(mesh=wa.asset_path(mesh_path), material=wa.engine_path(material), lod1=100000, lod2=100000)])
    print("prefabs: ValleyMist, LakeMist, LowClouds, MistCard")


if __name__ == "__main__":
    main()
