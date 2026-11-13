#pragma once

#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Materials/Material.h"

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace Engine::Scene::Renderer
{
struct DrawCBData
{
    uint32_t objectIndex;
    uint32_t lightCount;
    uint32_t flags;
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
    glm::vec4 subsurfaceTransmission{ 0.f }; // x = transmitted-light weight
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
};

struct PreparedDraw
{
    Engine::Core::Object* object = nullptr;
    Engine::Components::Mesh* mesh = nullptr;
    Engine::Components::Mesh* shadowMesh = nullptr;
    Engine::Components::Material* material = nullptr;
    Engine::Graphics::IGraphicsBuffer* vertexBuffer = nullptr;
    Engine::Graphics::IGraphicsBuffer* indexBuffer = nullptr;
    Engine::Graphics::IGraphicsBuffer* shadowVertexBuffer = nullptr;
    Engine::Graphics::IGraphicsBuffer* shadowIndexBuffer = nullptr;
    Engine::Graphics::IGraphicsBuffer* morphDeltaBuffer = nullptr;
    Engine::Graphics::IGraphicsBuffer* morphWeightBuffer = nullptr;
    Engine::Graphics::IPipelineState* pipeline = nullptr;
    bool surfaceShaderResolved = false;
    bool treeGeometry = false;
    std::array<const Engine::Graphics::IGraphicsTexture*, 30> textures{};
    uint32_t textureSlotCount = 12;
    uint64_t constantBufferOffset = 0;
    DrawCBData drawData{};
    ObjectGPUData objectData{};
    uint32_t vertexStride = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t shadowVertexStride = 0;
    uint32_t shadowVertexCount = 0;
    uint32_t shadowIndexCount = 0;
    int gpuLod = -1;
    int gpuMaximumLod = 0;
    glm::vec2 gpuLodDistances{ 0.f };
    uint32_t automaticGroup = UINT32_MAX;
    uint32_t frameItemIndex = 0;

    const Engine::Components::SpatialManipulator* traversalChartPortal = nullptr;
    const Engine::Components::SpatialManipulator* spatialManipulator = nullptr;
    bool preview = false;
    bool terrain = false;
    bool terrainCell = false;
    bool blended = false;
    bool pmxOutline = false;
    bool castsShadow = false;
    bool doubleSided = false;
    bool shadowDoubleSided = false;
    bool occlusionCandidate = false;
    uint64_t occlusionId = 0;
};

struct ResolvedMaterialRecord
{
    ObjectGPUData data{};
    std::array<const Engine::Graphics::IGraphicsTexture*, 30> textures{};
    bool doubleSided = false;
    Engine::Components::MaterialAlphaMode alphaMode{};
};

struct TerrainMaterialState
{
    std::shared_ptr<Engine::Components::Texture> measurementGridTexture;
    bool measurementGridReady = false;
};

struct ObjectLightingPolicy
{
    const Engine::Model::DistanceLightingBand* band = nullptr;
    bool beyondMaximumDistance = false;
    bool allowNormalMapping = true;
    bool allowParallaxMapping = true;
    bool allowSubsurfaceScattering = true;
    bool allowEnvironmentDiffuse = true;
    bool allowReflections = true;
    bool allowRealtimeShadows = true;
    float minimumProjectedPixels = 0.f;
};

class SceneRendererDrawPreparation
{
public:
    static ObjectLightingPolicy ResolveLightingPolicy(
        const Engine::Model::DistanceLightingSettings& settings,
        float lightingDistanceSquared,
        bool bakedStaticReceiver);
    static bool IsBelowProjectedSizeThreshold(
        const Scene::ViewRenderItem& item,
        const Engine::Components::Mesh* mesh,
        bool hasSprite,
        bool belongsToPreview,
        bool shadowOnly,
        const Engine::Components::Camera* camera,
        uint32_t viewportHeight,
        float minimumProjectedPixels);
    static Engine::Components::Mesh* ScreenLodPolicy(
        Scene& scene,
        const PreparedDraw& draw,
        bool hasPortalViews,
        bool includeEditorVisuals);
    static void BindObjectGeometry(
        PreparedDraw& draw,
        const Scene::FrameRenderItem& item,
        bool useSourceChart,
        bool useSoftSurface);
    static ResolvedMaterialRecord ResolveMaterialRecord(
        Scene& scene,
        Engine::Components::Material& material,
        const ObjectGPUData& environmentSeed,
        const std::array<const Engine::Graphics::IGraphicsTexture*, 30>& textures,
        const glm::vec3& cameraPosition,
        bool allowNormalMapping,
        bool allowParallaxMapping,
        bool allowSubsurfaceScattering,
        bool allowEnvironmentDiffuse,
        bool allowReflections);
    static void ApplyMaterialRecord(
        const ResolvedMaterialRecord& material,
        const glm::vec3& cameraPosition,
        const glm::vec3& irradiance,
        ObjectGPUData& objectData);
    static void PrepareTerrainMaterial(
        Scene& scene,
        const Scene::FrameRenderItem& item,
        PreparedDraw& draw,
        ObjectGPUData& objectData,
        bool includeEditorVisuals,
        bool forceUnlitMode,
        TerrainMaterialState& state);

};
}
