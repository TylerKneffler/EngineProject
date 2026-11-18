#pragma once

#include "Core/Rendering/SceneRenderer.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Rendering/ObjectGpuData.h"

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace Engine::Rendering
{
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
    // Quality x project-layer detail scale already applied to
    // gpuLodDistances; detail distances derived later (final LOD cull) use it.
    float lodScale = 1.f;
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
        const SceneRenderer::ViewRenderItem& item,
        const Engine::Components::Mesh* mesh,
        bool hasSprite,
        bool belongsToPreview,
        bool shadowOnly,
        const Engine::Components::Camera* camera,
        uint32_t viewportHeight,
        float minimumProjectedPixels);
    static Engine::Components::Mesh* ScreenLodPolicy(
        SceneRenderer& renderer,
        const PreparedDraw& draw,
        bool hasPortalViews,
        bool includeEditorVisuals);
    static void BindObjectGeometry(
        PreparedDraw& draw,
        const SceneRenderer::FrameRenderItem& item,
        bool useSourceChart,
        bool useSoftSurface);
    static ResolvedMaterialRecord ResolveMaterialRecord(
        SceneRenderer& renderer,
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
        SceneRenderer& renderer,
        const SceneRenderer::FrameRenderItem& item,
        PreparedDraw& draw,
        ObjectGPUData& objectData,
        bool includeEditorVisuals,
        bool forceUnlitMode,
        TerrainMaterialState& state);

};
}
