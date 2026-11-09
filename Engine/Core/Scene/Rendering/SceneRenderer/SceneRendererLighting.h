#pragma once

#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Scene/Scene.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace Engine::Scene::Renderer
{
class LightingContext
{
public:
    struct CascadeConfig
    {
        float split = 0.1f;
        float bias = 0.0f;
        std::uint32_t count = 4u;
    };

    explicit LightingContext(CascadeConfig config = {}) : m_config(config) {}

    void SetCascadeConfig(CascadeConfig config)
    {
        m_config = config;
    }

    const CascadeConfig& GetCascadeConfig() const
    {
        return m_config;
    }

private:
    CascadeConfig m_config;
};

class SceneRendererLighting
{
public:
    using CascadeConfig = LightingContext::CascadeConfig;

    struct ViewLightingState
    {
        glm::vec3 terrainSunDirection{ 0.f, 1.f, 0.f };
        glm::vec3 terrainSunRadiance{ 0.f };
        int terrainSunIndex = -1;
        std::array<std::shared_ptr<Engine::Components::Texture>, 8> photometryTextures{};
    };

    struct ClusterLightingConfig
    {
        uint32_t clusterTilesX = 0u;
        uint32_t clusterTilesY = 0u;
        uint32_t clusterDepthSlices = 0u;
        uint32_t maximumLightsPerCluster = 0u;
        uint32_t clusterCount = 0u;
        uint32_t indexCount = 0u;
        bool enabled = false;
        bool directionalOnly = false;
    };

    explicit SceneRendererLighting(CascadeConfig config = {}) : m_context(config) {}

    void Prepare(const CascadeConfig& config)
    {
        m_context.SetCascadeConfig(config);
    }

    const CascadeConfig& GetCascadeConfig() const
    {
        return m_context.GetCascadeConfig();
    }

    static ViewLightingState ResolveViewLighting(
        Scene& scene,
        const glm::vec3& cameraPosition,
        bool shadowOnly,
        const Engine::Model::LightData* frameLights,
        uint32_t frameLightCount,
        const std::array<std::string, 4>& cookiePaths,
        const std::array<std::string, 4>& iesPaths);

    static ClusterLightingConfig PrepareClusteredLighting(
        Engine::Graphics::IGraphicsProvider* graphicsProvider,
        const glm::mat4& view,
        const glm::mat4& proj,
        float cameraNear, float cameraFar,
        uint32_t viewportWidth,
        uint32_t viewportHeight,
        const Engine::Model::LightData* frameLights,
        uint32_t frameLightCount,
        const Engine::Model::DistanceLightingSettings& distanceLightingSettings,
        bool shadowOnly,
        std::unique_ptr<Engine::Graphics::IGraphicsBuffer>& clusterHeaderBuffer,
        void*& clusterHeaderMapped,
        uint32_t& clusterHeaderCapacity,
        std::unique_ptr<Engine::Graphics::IGraphicsBuffer>& clusterLightIndexBuffer,
        void*& clusterLightIndexMapped,
        uint32_t& clusterLightIndexCapacity,
        std::vector<glm::uvec2>& clusterHeaderScratch,
        std::vector<uint32_t>& clusterLightIndexScratch,
        std::vector<std::unique_ptr<Engine::Graphics::IGraphicsBuffer>>& retiredRenderBuffers,
        std::map<std::pair<const void*, const void*>, std::array<uint32_t, 5>>& directionalClusterLayouts,
        double* preparationMs = nullptr,
        uint64_t* uploadBytes = nullptr,
        uint64_t* cacheHits = nullptr);

    static void BindPhotometryTextures(
        Engine::Graphics::IGraphicsContext* context,
        const std::array<std::shared_ptr<Engine::Components::Texture>, 8>& photometryTextures);

    static void PrepareSkyboxAndEnvironment(
        Scene& scene,
        Engine::Graphics::IGraphicsContext* context,
        const glm::mat4& view,
        const glm::mat4& proj,
        uint32_t viewportWidth,
        uint32_t viewportHeight,
        bool shadowOnly);

private:
    LightingContext m_context;
};
}
