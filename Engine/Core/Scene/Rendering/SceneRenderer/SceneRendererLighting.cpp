#include "Core/Scene/Rendering/SceneRenderer/SceneRendererLighting.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"

#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Model/LightingData.h"
#include "Core/Scene/Scene.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

namespace Engine::Scene::Renderer
{
SceneRendererLighting::ClusterLightingConfig SceneRendererLighting::PrepareClusteredLighting(
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
    double* preparationMs,
    uint64_t* uploadBytes,
    uint64_t* cacheHits)
{
    ClusterLightingConfig config{};
    if (shadowOnly || !graphicsProvider || !frameLights || frameLightCount == 0u)
        return config;

    const uint32_t clusterTileSize = std::clamp(
        distanceLightingSettings.clusterTileSize, 16u, 256u);
    const uint32_t clusterTilesX = viewportWidth > 0u
        ? (viewportWidth + clusterTileSize - 1u) / clusterTileSize : 0u;
    const uint32_t clusterTilesY = viewportHeight > 0u
        ? (viewportHeight + clusterTileSize - 1u) / clusterTileSize : 0u;
    const uint32_t clusterDepthSlices = std::clamp(
        distanceLightingSettings.clusterDepthSlices, 1u, 64u);
    const uint32_t maximumLightsPerCluster = std::clamp(
        distanceLightingSettings.maximumLightsPerCluster, 1u,
        Engine::Model::MaxRealtimeLights);
    const uint64_t clusterCount64 = static_cast<uint64_t>(clusterTilesX) *
        clusterTilesY * clusterDepthSlices;
    const bool clusteredLightingEnabled = !shadowOnly &&
        distanceLightingSettings.clusteredLighting &&
        frameLightCount > 0u && clusterCount64 > 0u &&
        clusterCount64 <= std::numeric_limits<uint32_t>::max();
    if (!clusteredLightingEnabled)
        return config;

    const float clusterNear = std::max(0.001f, cameraNear);
    const float clusterFar = std::max(clusterNear + 0.001f, cameraFar);
    const float clusterLogScale = static_cast<float>(clusterDepthSlices) /
        std::max(std::log2(clusterFar / clusterNear), 0.0001f);

    config.clusterTilesX = clusterTilesX;
    config.clusterTilesY = clusterTilesY;
    config.clusterDepthSlices = clusterDepthSlices;
    config.maximumLightsPerCluster = maximumLightsPerCluster;
    config.enabled = true;

    const auto clusterStart = std::chrono::steady_clock::now();
    const uint32_t clusterCount = static_cast<uint32_t>(clusterCount64);
    const uint64_t indexCount64 = static_cast<uint64_t>(clusterCount) *
        maximumLightsPerCluster;
    if (indexCount64 > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Forward+ cluster index capacity overflow");
    config.clusterCount = clusterCount;
    config.indexCount = static_cast<uint32_t>(indexCount64);

    auto* bufferFactory = graphicsProvider->GetBufferFactory();
    if (!bufferFactory)
        return config;

    const auto ensureClusterBuffer = [&](std::unique_ptr<Engine::Graphics::IGraphicsBuffer>& buffer,
        void*& mapped, uint32_t& capacity, uint32_t required,
        uint32_t stride, const char* description)
    {
        if (capacity >= required && buffer && mapped)
            return;
        const uint32_t expanded = Engine::Scene::Renderer::ExpandedCapacity(capacity, required);
        auto replacement = bufferFactory->CreateBuffer(
            Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
            Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
            static_cast<uint64_t>(expanded) * stride, nullptr, stride);
        void* replacementMapped = replacement ? replacement->Map() : nullptr;
        if (!replacementMapped)
            throw std::runtime_error(std::string("Failed to allocate ") + description);
        if (buffer && mapped)
            buffer->Unmap();
        if (buffer)
            retiredRenderBuffers.push_back(std::move(buffer));
        buffer = std::move(replacement);
        mapped = replacementMapped;
        capacity = expanded;
        directionalClusterLayouts.clear();
    };

    ensureClusterBuffer(clusterHeaderBuffer, clusterHeaderMapped,
        clusterHeaderCapacity, clusterCount, sizeof(glm::uvec2),
        "Forward+ cluster headers");
    ensureClusterBuffer(clusterLightIndexBuffer, clusterLightIndexMapped,
        clusterLightIndexCapacity, config.indexCount, sizeof(uint32_t),
        "Forward+ cluster light indices");

    const auto bufferKey = std::make_pair(static_cast<const void*>(clusterHeaderBuffer.get()),
        static_cast<const void*>(clusterLightIndexBuffer.get()));
    const std::array<uint32_t, 5> layout{clusterTilesX, clusterTilesY, clusterDepthSlices,
        maximumLightsPerCluster, frameLightCount};
    const bool directionalOnly = std::all_of(frameLights, frameLights + frameLightCount,
        [](const auto& light) { return std::abs(light.params.y - 1.f) < .25f; });
    config.directionalOnly = directionalOnly;
    const auto cached = directionalClusterLayouts.find(bufferKey);
    const bool reuse = directionalOnly && cached != directionalClusterLayouts.end() && cached->second == layout;
    if (reuse && cacheHits)
        ++(*cacheHits);
    if (!reuse)
    {
        directionalClusterLayouts.erase(bufferKey);
        clusterHeaderScratch.assign(clusterCount, glm::uvec2(0u));
        clusterLightIndexScratch.assign(config.indexCount, 0u);
        std::vector<glm::uvec2>& headers = clusterHeaderScratch;
        std::vector<uint32_t>& indices = clusterLightIndexScratch;
        for (uint32_t cluster = 0; cluster < clusterCount; ++cluster)
            headers[cluster].x = cluster * maximumLightsPerCluster;
        const auto depthSlice = [&](float depth)
        {
            const float normalized = std::log2(std::max(depth, clusterNear) /
                clusterNear) * clusterLogScale;
            return std::min(clusterDepthSlices - 1u,
                static_cast<uint32_t>(std::max(0.f, normalized)));
        };
        const auto appendLight = [&](uint32_t cluster, uint32_t light)
        {
            glm::uvec2& header = headers[cluster];
            if (header.y < maximumLightsPerCluster)
                indices[header.x + header.y++] = light;
        };
        for (uint32_t lightIndex = 0; lightIndex < frameLightCount; ++lightIndex)
        {
            const auto& light = frameLights[lightIndex];
            if (std::abs(light.params.y - 1.f) < 0.25f)
            {
                for (uint32_t cluster = 0; cluster < clusterCount; ++cluster)
                    appendLight(cluster, lightIndex);
                continue;
            }
            const glm::vec4 viewPosition = view * glm::vec4(
                glm::vec3(light.positionRange), 1.f);
            const float range = std::max(0.f, light.positionRange.w);
            if (viewPosition.z + range < 0.001f ||
                viewPosition.z - range > 128.f)
                continue;
            const glm::vec4 clip = proj * viewPosition;
            if (std::abs(clip.w) < 0.0001f)
                continue;
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            const float radiusPixels = std::abs(proj[1][1]) * range /
                std::max(viewPosition.z, 0.001f) *
                static_cast<float>(viewportHeight) * 0.5f;
            const float centerX = (ndc.x * 0.5f + 0.5f) * viewportWidth;
            const float centerY = (1.f - (ndc.y * 0.5f + 0.5f)) * viewportHeight;
            const int32_t minTileX = std::clamp(static_cast<int32_t>(
                std::floor((centerX - radiusPixels) / clusterTileSize)),
                0, static_cast<int32_t>(clusterTilesX) - 1);
            const int32_t maxTileX = std::clamp(static_cast<int32_t>(
                std::floor((centerX + radiusPixels) / clusterTileSize)),
                0, static_cast<int32_t>(clusterTilesX) - 1);
            const int32_t minTileY = std::clamp(static_cast<int32_t>(
                std::floor((centerY - radiusPixels) / clusterTileSize)),
                0, static_cast<int32_t>(clusterTilesY) - 1);
            const int32_t maxTileY = std::clamp(static_cast<int32_t>(
                std::floor((centerY + radiusPixels) / clusterTileSize)),
                0, static_cast<int32_t>(clusterTilesY) - 1);
            const uint32_t minSlice = depthSlice(viewPosition.z - range);
            const uint32_t maxSlice = depthSlice(viewPosition.z + range);
            for (uint32_t slice = minSlice; slice <= maxSlice; ++slice)
            for (int32_t tileY = minTileY; tileY <= maxTileY; ++tileY)
            for (int32_t tileX = minTileX; tileX <= maxTileX; ++tileX)
            {
                const uint32_t cluster = slice * clusterTilesX * clusterTilesY +
                    static_cast<uint32_t>(tileY) * clusterTilesX +
                    static_cast<uint32_t>(tileX);
                appendLight(cluster, lightIndex);
            }
        }
        std::memcpy(clusterHeaderMapped, headers.data(),
            headers.size() * sizeof(glm::uvec2));
        std::memcpy(clusterLightIndexMapped, indices.data(),
            indices.size() * sizeof(uint32_t));
        clusterHeaderBuffer->FlushMappedWrites(0,
            headers.size() * sizeof(glm::uvec2));
        clusterLightIndexBuffer->FlushMappedWrites(0,
            indices.size() * sizeof(uint32_t));
        if (uploadBytes)
            *uploadBytes = static_cast<uint64_t>(headers.size()) * sizeof(glm::uvec2) +
                static_cast<uint64_t>(indices.size()) * sizeof(uint32_t);
        if (directionalOnly)
            directionalClusterLayouts[bufferKey] = layout;
    }
    if (preparationMs)
        *preparationMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - clusterStart).count();
    return config;
}

SceneRendererLighting::ViewLightingState SceneRendererLighting::ResolveViewLighting(
    Scene& scene,
    const glm::vec3& cameraPosition,
    bool shadowOnly,
    const Engine::Model::LightData* frameLights,
    uint32_t frameLightCount,
    const std::array<std::string, 4>& cookiePaths,
    const std::array<std::string, 4>& iesPaths)
{
    ViewLightingState state{};
    if (shadowOnly)
        return state;

    for (uint32_t index = 0; index < frameLightCount; ++index)
    {
        const auto& light = frameLights[index];
        if (std::abs(light.params.y - 1.f) >= 0.25f)
            continue;

        const glm::vec3 radiance = glm::vec3(light.colorIntensity) * light.colorIntensity.w;
        if (state.terrainSunIndex >= 0 &&
            glm::length(radiance) <= glm::length(state.terrainSunRadiance))
        {
            continue;
        }

        state.terrainSunDirection = glm::normalize(glm::vec3(light.positionRange));
        state.terrainSunRadiance = radiance;
        state.terrainSunIndex = static_cast<int>(index);
    }

    for (size_t index = 0; index < cookiePaths.size(); ++index)
    {
        if (!cookiePaths[index].empty())
        {
            state.photometryTextures[index] = Engine::Components::Texture::Acquire(
                cookiePaths[index], false);
            state.photometryTextures[index]->Prepare(scene.m_graphicsProvider);
        }
        if (!iesPaths[index].empty())
        {
            state.photometryTextures[index + 4u] = Engine::Components::Texture::Acquire(
                iesPaths[index], false);
            state.photometryTextures[index + 4u]->Prepare(scene.m_graphicsProvider);
        }
    }

    for (uint32_t index = 0; index < state.photometryTextures.size(); ++index)
    {
        if (state.photometryTextures[index])
            state.photometryTextures[index]->RequestMipFootprint(std::numeric_limits<float>::infinity());
    }

    (void)cameraPosition;
    return state;
}

void SceneRendererLighting::BindPhotometryTextures(
    Engine::Graphics::IGraphicsContext* context,
    const std::array<std::shared_ptr<Engine::Components::Texture>, 8>& photometryTextures)
{
    if (!context)
        return;

    for (uint32_t index = 0; index < photometryTextures.size(); ++index)
    {
        if (photometryTextures[index])
            photometryTextures[index]->RequestMipFootprint(std::numeric_limits<float>::infinity());
        context->SetTexture(16u + index, photometryTextures[index]
            ? photometryTextures[index]->GetGraphicsTexture() : nullptr);
    }
}

void SceneRendererLighting::PrepareSkyboxAndEnvironment(
    Scene& scene,
    Engine::Graphics::IGraphicsContext* context,
    const glm::mat4& view,
    const glm::mat4& proj,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool shadowOnly)
{
    const Engine::Components::Texture* skybox = shadowOnly
        ? nullptr
        : scene.ResolveSkyboxTexture();
    if (!shadowOnly)
        scene.UpdateEnvironmentLighting(skybox);

    if (!shadowOnly && skybox && skybox->GetGraphicsTexture() && scene.m_skyboxPipeline)
    {
        // A panorama repeat spans the full environment, beyond the camera FOV.
        Engine::Components::Texture::RequestGraphicsFootprint(
            skybox->GetGraphicsTexture(),
            4.f * float(std::max(viewportWidth, viewportHeight)));

        struct SkyboxCBData { glm::mat4 invVP; glm::vec4 displayParams; };
        SkyboxCBData skyboxData{};
        // A skybox represents direction only. Excluding camera translation
        // prevents cancellation in the shader's farPoint - nearPoint math
        // when the editor camera is far from the world origin.
        const glm::mat4 skyboxView = glm::mat4(glm::mat3(view));
        skyboxData.invVP = glm::inverse(proj * skyboxView);
        skyboxData.displayParams = {
            scene.m_editorMode2D ? 1.f : 0.f,
            std::exp2(scene.settings.hdriExposure),
            glm::radians(scene.settings.hdriRotation),
            0.f};

        std::memcpy(scene.m_skyboxCBMapped, &skyboxData, sizeof(skyboxData));
        context->SetPipeline(scene.m_skyboxPipeline.get());
        context->SetConstantBuffer(0, scene.m_skyboxConstantBuffer.get(), 0);
        context->SetTexture(0, skybox->GetGraphicsTexture());
        context->DrawInstanced(3, 1, 0, 0);
    }
}
}