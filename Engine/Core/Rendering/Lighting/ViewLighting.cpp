#include "Core/Rendering/Lighting/ViewLighting.h"

#include "Core/Components/Materials/Texture.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Rendering/RenderUtilities.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

namespace Engine::Rendering
{
ViewLighting::ClusterLightingConfig ViewLighting::PrepareClusteredLighting(
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
        const uint32_t expanded = Engine::Rendering::ExpandedCapacity(capacity, required);
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

void ViewLighting::BindPhotometryTextures(
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

std::array<glm::vec4, 4> ViewLighting::PackAtmosphereFog(
    const Engine::Model::AtmosphereFogSettings& fog,
    const glm::vec3& sunDirection,
    const glm::vec3& sunRadiance,
    bool hasSun)
{
    std::array<glm::vec4, 4> packed{};
    if (!fog.enabled || fog.density <= 0.f)
        return packed;
    // The glow takes the sun's hue, not its absolute radiance, so the
    // strength slider behaves the same across differently lit scenes.
    glm::vec3 sunTint(0.f);
    if (hasSun)
    {
        const float peak = std::max({ sunRadiance.r, sunRadiance.g,
            sunRadiance.b, 1e-4f });
        sunTint = sunRadiance / peak;
    }
    const glm::vec3 toSun = glm::length(sunDirection) > 1e-4f
        ? glm::normalize(sunDirection) : glm::vec3(0.f, 1.f, 0.f);
    // w: 1 = fog; 1 + samples + 0.999 * strength adds light shafts
    // (decoded by AtmosphereShaftSampleCount/Strength in AtmosphereFog.hlsli).
    float enabledAndShafts = 1.f;
    const int shaftSamples = std::clamp(fog.lightShaftSamples, 0,
        Engine::Model::AtmosphereFogSettings::MaximumLightShaftSamples);
    const float shaftStrength = std::clamp(fog.lightShaftStrength, 0.f, 1.f);
    if (fog.lightShafts && hasSun && shaftSamples > 0 && shaftStrength > 0.f &&
        fog.sunGlowStrength > 0.f)
        enabledAndShafts += static_cast<float>(shaftSamples) +
            0.999f * shaftStrength;
    packed[0] = { fog.density, fog.heightFalloff, fog.baseHeight,
        enabledAndShafts };
    packed[1] = { fog.inscatterColor * fog.inscatterIntensity,
        fog.startDistance };
    packed[2] = { toSun, fog.sunGlowAnisotropy };
    packed[3] = { sunTint * fog.sunGlowStrength * fog.inscatterIntensity,
        fog.maxOpacity };
    return packed;
}

void ViewLighting::SetSkyboxRays(SkyboxConstants& constants,
    const glm::mat4& proj, const glm::mat4& directionOnlyView)
{
    // Near-to-far plane vector through an NDC point (proj is reverse-Z). Both planes map to
    // planes for perspective and orthographic projections, so the vector is
    // affine in NDC and three samples reproduce it exactly.
    const glm::mat4 invVP = glm::inverse(proj * directionOnlyView);
    const auto ray = [&](float x, float y)
    {
        const glm::vec4 nearPoint = invVP *
            glm::vec4(x, y, Engine::Graphics::Depth::Near, 1.f);
        const glm::vec4 farPoint = invVP *
            glm::vec4(x, y, Engine::Graphics::Depth::Far, 1.f);
        return glm::vec3(farPoint) / farPoint.w - glm::vec3(nearPoint) / nearPoint.w;
    };
    const glm::vec3 origin = ray(0.f, 0.f);
    constants.rayX = glm::vec4(ray(1.f, 0.f) - origin, constants.rayX.w);
    constants.rayY = glm::vec4(ray(0.f, 1.f) - origin, constants.rayY.w);
    constants.rayOrigin = glm::vec4(origin, constants.rayOrigin.w);
}
}
