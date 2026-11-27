#pragma once

#include <cstdint>

#include <glm/glm.hpp>

// GPU record layouts shared by every object draw. They mirror the cbuffer and
// structured-buffer declarations in Shaders/Object/Object.hlsl; static_asserts
// next to the renderer's upload code keep the sizes in step.
namespace Engine::Rendering
{
struct DrawCBData
{
    uint32_t objectIndex;
    uint32_t lightCount;
    uint32_t flags;
    // Instanced ground cover: Mesh::InstanceRange::reachScale in 1/1024ths
    // (0 = 1.0) in bits 0-9, and bit 10 set when the range is not thinned
    // (InstanceRange::thinned). Object.hlsl reads it as draw.placementReach.
    uint32_t padding;
};

constexpr uint32_t kPortalApertureNearClampFlag = 0x40000000u;
constexpr uint32_t kPortalDepthResetFlag = 0x80000000u;
constexpr uint32_t kClusteredLightingFlag = 0x00000001u;

// Large, indexed records live in shader-readable buffers on every backend.
struct ObjectGPUData
{
    // Kept separate from world so adjoining meshes transform a shared world
    // position through exactly the same view-projection arithmetic. Baking
    // world into this matrix per object can expose sub-pixel cracks between
    // otherwise identical terrain borders.
    glm::mat4 viewProjection;
    glm::mat4 sphereView; // View rotation for PMX normal-based sphere coordinates.
    glm::mat4 world;
    glm::vec4 baseColor;
    glm::vec4 ambientUnlit;
    glm::vec4 emissiveOcclusion;
    glm::vec4 materialParams; // metallic, roughness, normal scale, texture mask
    glm::vec4 viewPositionAlphaCutoff;
    glm::vec4 bakedDirectional;
    glm::vec4 bakedLightDirection;
    glm::vec4 parallaxParams; // scale, minimum steps, maximum steps, flags (1 = height is depth)
    glm::vec4 spriteUvRect; // offset.xy, scale.xy
    glm::vec4 textureUvSets0;
    glm::vec4 textureUvSets1;
    glm::vec4 skinParams; // palette offset, joint count, reserved, reserved
    glm::vec4 morphParams; // target count, vertex count, reserved, reserved
    glm::vec4 environmentParams; // intensity, rotation radians, diffuse, reflections
    glm::vec4 environmentSH[9]; // RGB radiance coefficients
    glm::vec4 reflectionEnvironmentParams; // exposure scale, rotation, custom enabled, reserved
    glm::vec4 reflectionEnvironmentSH[9];
    glm::mat4 shadowViewProjections[4];
    // depth bias, normal/slope bias, strength, PCF radius
    glm::vec4 shadowParams;
    // View-distance end for each directional cascade.
    glm::vec4 shadowCascadeSplits;
    // cascade count, atlas tile scale, boundary blend fraction, max distance
    glm::vec4 shadowCascadeData;
    // Camera forward direction and near distance used for cascade selection.
    glm::vec4 shadowCameraData;
    // tile size, tiles X, tiles Y, depth slices
    glm::vec4 lightingClusterParams;
    // camera forward, logarithmic depth scale
    glm::vec4 lightingCameraData;
    // Enabled per portal draw through DrawCBData::flags; dot(world, plane)
    // selects the connected half-space at the target aperture.
    glm::vec4 portalClipPlane;
    // Independent from portal-view clipping: active traversal bodies render
    // their local and remote chart instances from one untouched mesh buffer.
    glm::vec4 traversalClipPlane;
    glm::uvec4 lightingChannels{ 0xffffffffu, 0u, 0u, 0u };
    glm::vec4 subsurfaceColorWeight{ 0.f }; // RGB color, enabled weight
    glm::vec4 subsurfaceRadiusThickness{ 0.f }; // RGB scene units, thickness in scene units
    glm::vec4 subsurfaceTransmission{ 0.f }; // x = transmitted-light weight, y = foliage leaf translucency
    glm::vec4 blinnPhongSpecularShininess{ 0.f }; // RGB reflectance; w = shininess + 1 when enabled
    glm::vec4 pmxTextureTint { 1.f };
    glm::vec4 pmxSphereTint { 1.f };
    glm::vec4 pmxToonTint { 1.f };
    glm::vec4 pmxEdgeColor { 0.f };
    glm::vec4 pmxParams { 0.f }; // edge width, sphere mode, toon source, edge enabled
    glm::uvec4 terrainMapMasks{ 0u }; // normal, roughness, height, reserved
    glm::vec4 terrainLayerParams[6]{}; // roughness, normal scale, height blend, reserved
    glm::vec4 terrainMacroColors[6]{}; // measured linear albedo averages
    glm::vec4 geometryFade{0.f, 1.f, 0.f, 0.f}; // dither interval, reserved, enabled
    glm::vec4 shadowCasterFade{0.f}; // range, fade width, shadow pass enabled, reserved
    glm::vec4 grassAnimation{0.f}; // scene time, grass enabled, reserved
    glm::vec4 textureLodParams{0.f}; // near, far, maximum mip bias, enabled/shading tier
    glm::vec4 alphaLodParams{0.f};
    glm::vec4 grassCapsules[8]{}; // four start/radius + end/reach pairs
    // Height fog, packed by PackAtmosphereFog (AtmosphereFog.hlsli layout);
    // zero leaves fog disabled for editor/debug draws.
    glm::vec4 atmosphereFog[4]{};
    // Terrain long-range sun shadow map (texture slot 4): world X/Z of the
    // map corner, 1 / extent X, 1 / extent Z. Zero extent disables it.
    glm::vec4 terrainSunShadow{0.f};
    // Terrain macro color map (texture slot 5): world X/Z of the texture
    // corner, 1 / extent X, 1 / extent Z; params: blend start and full
    // distance for loaded cells, z = 1 when bound.
    glm::vec4 terrainMacroMap{0.f};
    glm::vec4 terrainMacroParams{0.f};
    // Instanced ground cover (Mesh::placementFadeStart/End): fade start and
    // end distance; each placement shrinks toward its root across the band.
    glm::vec4 placementFade{0.f};
    // Terrain under ground cover (Terrain::GroundCover): cover colour and
    // tint strength; fade start, fade end, layer mask, root shade.
    glm::vec4 groundCoverColor{0.f};
    glm::vec4 groundCoverRange{0.f};
    // Instanced ground cover thinning (Mesh::placementThin*): start, end,
    // minimum share kept, w = 1 keeps survivors' width (no widening).
    glm::vec4 placementThin{0.f, 0.f, 1.f, 0.f};
    // Terrain trails (TerrainManager::GetTrailGpuData, Object.hlsl
    // TerrainTrailSample), read from texture slot 3: bin grid origin x, z,
    // 1 / bin size, bins x; bins z, list offset, segment offset, texture
    // width (0 = no trails); the manager's world position x, z (the grid is
    // in manager space).
    glm::vec4 terrainTrailGrid{0.f};
    glm::uvec4 terrainTrailKeys{0u};
    glm::vec4 terrainTrailOffset{0.f};
    // Volume chunks shaded from their cell's paint map (slot 15): the
    // volume's steep blend, re-applied per pixel from the surface normal
    // (VolumeTerrain steepStart, steepEnd, steepLayer, w = 1 when on).
    glm::vec4 terrainSteep{0.f};
    glm::vec4 terrainTrailReserved{0.f};
    // x = mip bias of the view (<= 0): -log2(output / render pixels) while
    // TAA upscales it, so textures keep the output's sharpness (Object.hlsl
    // TextureLodBias, AlphaLodBias). yzw: the near/far cell handover grid
    // (Rendering/CellHandover.h): the camera cell's corner x, z, and
    // 1 / cell span, negative for far impostors (0: no handover).
    glm::vec4 viewTextureParams{0.f};
    // Directional shadow atlas tiles (SceneRendererShadows, refresh blend):
    // x = each cascade's tile, 3 bits per cascade (cascade 0 lowest); y, z =
    // blend slots: cascade + 8 * the tile of its previous image, or -1 when
    // idle; w = atlas rows of tiles. A refreshed cascade fades in from its
    // previous image over a few frames instead of changing at once.
    glm::vec4 shadowAtlasTiles{0.f + 8.f * 1.f + 64.f * 2.f + 512.f * 3.f, -1.f, -1.f, 2.f};
    // Per blend slot: clip-space offset from the new image's projection to
    // the previous one's (xyz), and the new image's weight (w).
    glm::vec4 shadowRefreshBlend[2]{};
    // Near/far cell handover (Rendering/CellHandover.h): up to 12 cells
    // mid-handover, one per uint (CellHandover::Entry); the grid and role are
    // in viewTextureParams.yzw. The record is at D3D12's 2048-byte limit for a
    // structured buffer element.
    glm::uvec4 handoverCells[3]{};
};
}
