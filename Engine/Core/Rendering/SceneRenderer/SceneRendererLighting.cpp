#include "Core/Rendering/RenderingEffects.h"
#include "Core/Rendering/SceneRenderer/SceneRendererLighting.h"
#include "Core/Rendering/RenderUtilities.h"

#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Model/LightingData.h"
#include "Core/Rendering/SceneRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

namespace Engine::Rendering
{
using Scene = Engine::Scene::Scene;
SceneRendererLighting::ViewLightingState SceneRendererLighting::ResolveViewLighting(
    SceneRenderer& renderer,
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

    // Cookie and IES paths are portable asset paths ("Assets/Textures/..."),
    // resolved like mesh and material paths. Unresolved, they loaded only when
    // the working directory was the asset root; from anywhere else the decode
    // failed, the null texture was bound and it multiplied the light by zero
    // (the landscape's sun with its cloud-shadow cookie went dark).
    thread_local std::unordered_map<std::string, std::string> resolvedPaths;
    const auto acquire = [&](const std::string& path)
    {
        auto found = resolvedPaths.find(path);
        if (found == resolvedPaths.end())
            found = resolvedPaths.emplace(path, Engine::Components::Mesh::ResolveFilePath(path)).first;
        auto texture = Engine::Components::Texture::Acquire(found->second, false);
        texture->Prepare(renderer.m_graphicsProvider);
        return texture;
    };
    // A cookie or profile that is missing or still loading must not black
    // out its light: a white texel stands in until the real one is resident.
    const auto usable = [&](std::shared_ptr<Engine::Components::Texture> texture)
    {
        if (texture->GetGraphicsTexture()) return texture;
        thread_local std::shared_ptr<Engine::Components::Texture> white;
        if (!white)
            white = Engine::Components::Texture::CreateFromPixels("Lighting.WhitePhotometry", 1, 1, 1,
                Engine::Graphics::GraphicsTextureFormat::Rgba8, { 255, 255, 255, 255 }, false);
        if (white && !white->GetGraphicsTexture()) white->PrepareImmediate(renderer.m_graphicsProvider);
        return white ? white : texture;
    };
    for (size_t index = 0; index < cookiePaths.size(); ++index)
    {
        if (!cookiePaths[index].empty())
            state.photometryTextures[index] = usable(acquire(cookiePaths[index]));
        if (!iesPaths[index].empty())
            state.photometryTextures[index + 4u] = usable(acquire(iesPaths[index]));
    }

    for (uint32_t index = 0; index < state.photometryTextures.size(); ++index)
    {
        if (state.photometryTextures[index])
            state.photometryTextures[index]->RequestMipFootprint(std::numeric_limits<float>::infinity());
    }

    (void)cameraPosition;
    return state;
}

void SceneRendererLighting::ResolveSkySun(const SceneRenderer& renderer,
    const glm::vec3& sunDirection, const glm::vec3& sunRadiance,
    glm::vec4& direction, glm::vec4& radiance)
{
    const Scene& scene = renderer.GetScene();
    const auto& settings = scene.settings;
    direction = glm::vec4(0.f);
    radiance = glm::vec4(0.f);
    if (!settings.sunDiscEnabled || scene.IsMode2D() ||
        glm::dot(sunDirection, sunDirection) < 1e-8f ||
        glm::dot(sunRadiance, sunRadiance) <= 0.f)
        return;
    const float radius = glm::radians(std::clamp(
        settings.sunDiscAngularDiameter, 0.05f, 20.f) * 0.5f);
    direction = glm::vec4(glm::normalize(sunDirection), std::cos(radius));
    radiance = glm::vec4(sunRadiance, std::max(settings.sunDiscIntensity, 0.f));
}

void SceneRendererLighting::ApplyPhysicalAtmosphere(
    SceneRenderer& renderer,
    Engine::Model::LightData* frameLights,
    uint32_t frameLightCount,
    const glm::vec3& cameraPosition)
{
    renderer.m_physicalAtmosphereReady = false;
    const Scene& scene = renderer.GetScene();
    const auto& sky = scene.settings.physicalSky;
    if (!sky.enabled || scene.IsMode2D())
    {
        renderer.m_physicalSky.Reset();
        return;
    }
    // The sun: the strongest directional light (as ResolveViewLighting picks).
    int sunIndex = -1;
    float strongest = 0.f;
    for (uint32_t index = 0; frameLights && index < frameLightCount; ++index)
    {
        const auto& light = frameLights[index];
        if (std::abs(light.params.y - 1.f) >= 0.25f) continue;
        const float strength = glm::length(glm::vec3(light.colorIntensity)) * light.colorIntensity.w;
        if (sunIndex < 0 || strength > strongest) { sunIndex = static_cast<int>(index); strongest = strength; }
    }
    PhysicalSky::Inputs inputs;
    inputs.parameters.rayleighScale = sky.rayleighScale;
    inputs.parameters.mieScale = sky.mieScale;
    inputs.parameters.mieScaleHeight = sky.mieScaleHeight;
    inputs.parameters.mieAnisotropy = sky.mieAnisotropy;
    inputs.parameters.ozoneScale = sky.ozoneScale;
    inputs.parameters.groundAlbedo = sky.groundAlbedo;
    inputs.altitude = std::max(cameraPosition.y - sky.groundHeight, 0.f);
    if (sunIndex >= 0)
    {
        auto& sun = frameLights[sunIndex];
        inputs.toSun = glm::normalize(glm::vec3(sun.positionRange));
        inputs.sunIlluminance = glm::vec3(sun.colorIntensity) * sun.colorIntensity.w;
    }
    else
        inputs.sunIlluminance = glm::vec3(0.f);
    renderer.m_physicalSky.Update(inputs);
    const auto& atmosphere = renderer.m_physicalSky.Atmosphere();
    if (!atmosphere)
        return;
    const glm::vec3 transmittance = atmosphere->Transmittance(inputs.altitude, inputs.toSun.y);
    if (sunIndex >= 0 && sky.attenuateSun)
    {
        auto& sun = frameLights[sunIndex];
        sun.colorIntensity = glm::vec4(glm::vec3(sun.colorIntensity) * transmittance,
            sun.colorIntensity.w);
    }
    renderer.m_physicalAtmosphereReady = true;
    renderer.m_physicalToSun = inputs.toSun;
    renderer.m_physicalSunIlluminance = inputs.sunIlluminance * transmittance;
    renderer.m_physicalMultipleScattering = inputs.sunIlluminance *
        atmosphere->MultipleScattering(inputs.altitude, inputs.toSun.y);
}

const Engine::Graphics::IGraphicsBuffer* SceneRendererLighting::AmbientOcclusionBuffer(
    SceneRenderer& renderer)
{
    if (renderer.m_ambientOcclusionReady &&
        renderer.m_ambientOcclusionBuffers[renderer.m_ambientOcclusionLatest])
        return renderer.m_ambientOcclusionBuffers[renderer.m_ambientOcclusionLatest].get();
    if (!renderer.m_ambientOcclusionOff && renderer.m_graphicsProvider &&
        renderer.m_graphicsProvider->GetBufferFactory())
    {
        // Header only, all zero: "not valid", so shaders skip the lookup.
        const std::array<uint32_t, 16> header{};
        renderer.m_ambientOcclusionOff = renderer.m_graphicsProvider->GetBufferFactory()
            ->CreateStaticShaderResource(sizeof(header), header.data(), sizeof(uint32_t));
    }
    return renderer.m_ambientOcclusionOff.get();
}

void SceneRendererLighting::ComputeAmbientOcclusion(
    SceneRenderer& renderer,
    Engine::Graphics::IGraphicsContext* context,
    const glm::mat4& view,
    const glm::mat4& proj,
    uint32_t viewportWidth,
    uint32_t viewportHeight)
{
    const Scene& scene = renderer.GetScene();
    // Called once per main view: a previous view's depth copy is stale.
    renderer.m_sceneDepthCopied = false;
    renderer.m_sceneDepthWidth = viewportWidth;
    renderer.m_sceneDepthHeight = viewportHeight;
    renderer.m_sceneDepthReconstruct = std::abs(proj[2][3]) > 1e-6f
        ? glm::vec2(proj[2][2] / proj[2][3], proj[3][2]) : glm::vec2(0.f);
    const auto& settings = scene.settings.ambientOcclusion;
    const bool lit = scene.settings.renderMode == Engine::Model::SceneRenderMode::Lit;
    const bool projectAllows =
        Engine::Rendering::RenderingEffects::Get().Settings().postEffects.ambientOcclusion;
    if (!settings.enabled || !projectAllows || !lit || scene.IsMode2D() ||
        !renderer.m_ambientOcclusionPipeline ||
        !context->SupportsSceneDepthCopy() || viewportWidth < 16u || viewportHeight < 16u ||
        viewportWidth > 8192u || viewportHeight > 8192u)
    {
        renderer.m_ambientOcclusionReady = false;
        return;
    }
    auto* buffers = renderer.m_graphicsProvider
        ? renderer.m_graphicsProvider->GetBufferFactory() : nullptr;
    if (!buffers)
        return;
    using Engine::Graphics::IGraphicsBuffer;
    const uint32_t halfWidth = (viewportWidth + 1u) / 2u;
    const uint32_t halfHeight = (viewportHeight + 1u) / 2u;
    const uint64_t depthBytes = uint64_t(Engine::Graphics::SceneDepthRowPitch(viewportWidth)) *
        viewportHeight * sizeof(float);
    const uint64_t occlusionBytes = (16u + uint64_t(halfWidth) * halfHeight) * sizeof(uint32_t);
    // Zero-filled: an unwritten depth reads as sky (unoccluded) and an
    // unwritten AO header as "not valid".
    if (!renderer.m_ambientOcclusionDepth || renderer.m_ambientOcclusionDepthBytes < depthBytes)
    {
        const std::vector<uint8_t> zeros(size_t(depthBytes), 0u);
        renderer.m_ambientOcclusionDepth = buffers->CreateBuffer(IGraphicsBuffer::Usage::StorageBuffer,
            IGraphicsBuffer::AccessMode::Default, depthBytes, zeros.data(), sizeof(float));
        renderer.m_ambientOcclusionDepthBytes = renderer.m_ambientOcclusionDepth ? depthBytes : 0;
    }
    if (!renderer.m_ambientOcclusionBuffers[0] || renderer.m_ambientOcclusionBufferBytes < occlusionBytes)
    {
        const std::vector<uint8_t> zeros(size_t(occlusionBytes), 0u);
        for (auto& buffer : renderer.m_ambientOcclusionBuffers)
            buffer = buffers->CreateBuffer(IGraphicsBuffer::Usage::StorageBuffer,
                IGraphicsBuffer::AccessMode::Default, occlusionBytes, zeros.data(), sizeof(uint32_t));
        renderer.m_ambientOcclusionBufferBytes = renderer.m_ambientOcclusionBuffers[0] &&
            renderer.m_ambientOcclusionBuffers[1] ? occlusionBytes : 0;
        renderer.m_ambientOcclusionReady = false;
    }
    if (!renderer.m_ambientOcclusionDepth || !renderer.m_ambientOcclusionBufferBytes)
        return;

    // Camera frame for reconstruction and reprojection: world axes of the
    // view, the projection's x/y scales, and view depth w = B / (depth - A).
    struct Constants
    {
        glm::vec4 cameraPositionScaleX;
        glm::vec4 cameraRightScaleY;
        glm::vec4 cameraUpDepthA;
        glm::vec4 cameraForwardDepthB;
        glm::uvec4 extent;
        glm::vec4 params;
    } constants{};
    static_assert(sizeof(Constants) == 96, "AmbientOcclusion.hlsl constants");
    const glm::mat4 inverseView = glm::inverse(view);
    const glm::mat3 axes(inverseView);
    // clip.w grows along proj's last row: the view-space forward direction.
    const glm::vec3 forwardView(proj[0][3], proj[1][3], proj[2][3]);
    const float forwardScale = proj[2][3];
    if (std::abs(forwardScale) < 1e-6f)
        return; // not a perspective projection
    constants.cameraPositionScaleX = glm::vec4(glm::vec3(inverseView[3]), proj[0][0]);
    constants.cameraRightScaleY = glm::vec4(glm::normalize(axes[0]), proj[1][1]);
    constants.cameraUpDepthA = glm::vec4(glm::normalize(axes[1]), proj[2][2] / forwardScale);
    constants.cameraForwardDepthB = glm::vec4(glm::normalize(axes * forwardView), proj[3][2]);
    constants.extent = glm::uvec4(viewportWidth, viewportHeight, halfWidth | (halfHeight << 16),
        Engine::Graphics::SceneDepthRowPitch(viewportWidth));
    const bool history = renderer.m_ambientOcclusionReady;
    constants.params = glm::vec4(settings.radius, settings.intensity,
        float(renderer.m_ambientOcclusionFrame++ % 64u), history ? settings.temporalBlend : 1.f);

    const uint32_t target = 1u - renderer.m_ambientOcclusionLatest;
    const auto* previous = renderer.m_ambientOcclusionBuffers[renderer.m_ambientOcclusionLatest].get();
    auto* output = renderer.m_ambientOcclusionBuffers[target].get();
    if (!context->BeginComputePass())
        return;
    context->BeginGpuTiming(Engine::Graphics::GpuTimingStage::AmbientOcclusion);
    context->CopySceneDepth(renderer.m_ambientOcclusionDepth.get(), viewportWidth, viewportHeight);
    renderer.m_sceneDepthCopied = true;
    if (context->SetComputePipeline(renderer.m_ambientOcclusionPipeline.get()))
    {
        context->SetComputeReadBuffer(renderer.m_ambientOcclusionDepth.get(), 0);
        context->SetComputeReadBuffer(previous, 1);
        context->SetComputeWriteBuffer(output);
        context->SetComputeConstants(&constants, sizeof(constants));
        context->DispatchCompute(static_cast<uint32_t>(
            (uint64_t(halfWidth) * halfHeight + 63u) / 64u));
        context->StorageBarrier(output);
        renderer.m_ambientOcclusionLatest = target;
        renderer.m_ambientOcclusionReady = true;
    }
    context->EndGpuTiming(Engine::Graphics::GpuTimingStage::AmbientOcclusion);
    context->EndComputePass();
    context->SetStructuredBuffer(47, AmbientOcclusionBuffer(renderer));
}

namespace
{
// Level-0 width of the GPU pyramid (matches OcclusionBuffer::Width) and its
// level count: 256 x 144 down to 32 x 18 at 16:9. Coarser levels would make
// single cells read too many pixels (each cell reads its footprint directly).
constexpr uint32_t DepthPyramidWidth = 256u;
constexpr uint32_t DepthPyramidLevels = 4u;
constexpr uint32_t DepthPyramidHeaderFloats = 80u;

glm::vec3 ViewForward(const glm::mat4& view)
{
    // Row 2 of the view matrix is the camera's +z axis in world space; the
    // camera looks down -z.
    return -glm::normalize(glm::vec3(view[0][2], view[1][2], view[2][2]));
}

glm::vec3 ViewPosition(const glm::mat4& view)
{
    return glm::vec3(glm::inverse(view)[3]);
}
}

void SceneRendererLighting::BuildDepthPyramid(
    SceneRenderer& renderer,
    Engine::Graphics::IGraphicsContext* context,
    const void* camera,
    const glm::mat4& view,
    const glm::mat4& viewProjection,
    const glm::mat4& proj,
    uint32_t viewportWidth,
    uint32_t viewportHeight)
{
    renderer.m_depthPyramidValid = false;
    if (!context || !renderer.m_sceneDepthCopied || !renderer.m_depthPyramidPipeline ||
        !renderer.m_ambientOcclusionDepth || std::abs(proj[2][3]) < 1e-6f ||
        viewportWidth < 16u || viewportHeight < 16u)
        return;
    auto* buffers = renderer.m_graphicsProvider
        ? renderer.m_graphicsProvider->GetBufferFactory() : nullptr;
    auto* contexts = renderer.m_graphicsProvider
        ? renderer.m_graphicsProvider->GetContextFactory() : nullptr;
    if (!buffers || !contexts)
        return;
    const uint32_t height0 = std::max(1u, static_cast<uint32_t>(std::lround(
        float(DepthPyramidWidth) * float(viewportHeight) / float(viewportWidth))));
    uint32_t cells = 0;
    for (uint32_t level = 0; level < DepthPyramidLevels; ++level)
        cells += std::max(1u, (DepthPyramidWidth + (1u << level) - 1u) >> level) *
            std::max(1u, (height0 + (1u << level) - 1u) >> level);
    const uint64_t bytes = uint64_t(DepthPyramidHeaderFloats + cells) * sizeof(float);
    if (!renderer.m_depthPyramid || renderer.m_depthPyramidBytes < bytes)
    {
        // Zero-filled: zero levels reads as "no pyramid" until written.
        const std::vector<uint8_t> zeros(size_t(bytes), 0u);
        renderer.m_depthPyramid = buffers->CreateBuffer(
            Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer,
            Engine::Graphics::IGraphicsBuffer::AccessMode::Default, bytes, zeros.data(), sizeof(float));
        renderer.m_depthPyramidBytes = renderer.m_depthPyramid ? bytes : 0;
    }
    if (!renderer.m_depthPyramid)
        return;
    struct Constants
    {
        glm::mat4 viewProjection;
        glm::vec4 depth;
        glm::uvec4 extent;
        glm::uvec4 pyramid;
    } constants{};
    static_assert(sizeof(Constants) == 112, "DepthPyramid.hlsl constants");
    constants.viewProjection = viewProjection;
    // View depth w = B / (depth - A), as for ambient occlusion.
    constants.depth = glm::vec4(proj[2][2] / proj[2][3], proj[3][2], 0.f, 0.f);
    constants.extent = glm::uvec4(viewportWidth, viewportHeight,
        Engine::Graphics::SceneDepthRowPitch(viewportWidth), DepthPyramidLevels);
    constants.pyramid = glm::uvec4(DepthPyramidWidth, height0, cells, 0u);
    if (!context->BeginComputePass())
        return;
    if (context->SetComputePipeline(renderer.m_depthPyramidPipeline.get()))
    {
        context->SetComputeReadBuffer(renderer.m_ambientOcclusionDepth.get(), 0);
        context->SetComputeWriteBuffer(renderer.m_depthPyramid.get());
        context->SetComputeConstants(&constants, sizeof(constants));
        context->DispatchCompute((cells + 63u) / 64u);
        context->StorageBarrier(renderer.m_depthPyramid.get());
        renderer.m_depthPyramidValid = true;
        renderer.m_depthPyramidCamera = camera;
        renderer.m_depthPyramidPosition = ViewPosition(view);
        renderer.m_depthPyramidForward = ViewForward(view);
        renderer.m_depthPyramidViewportWidth = viewportWidth;
        renderer.m_depthPyramidViewportHeight = viewportHeight;
        renderer.m_depthPyramidFrameSerial = contexts->GetPreparedFrameSerial();
    }
    context->EndComputePass();
}

const Engine::Graphics::IGraphicsBuffer* SceneRendererLighting::DepthPyramidForCulling(
    SceneRenderer& renderer,
    const void* camera,
    const glm::mat4& view,
    uint32_t viewportWidth,
    uint32_t viewportHeight)
{
    static const bool enabled = ReadEnvFlag("ENGINE_GPU_OCCLUSION", true);
    auto* contexts = renderer.m_graphicsProvider
        ? renderer.m_graphicsProvider->GetContextFactory() : nullptr;
    if (!enabled || !contexts || !renderer.m_depthPyramidValid || !renderer.m_depthPyramid ||
        renderer.m_depthPyramidCamera != camera ||
        renderer.m_depthPyramidViewportWidth != viewportWidth ||
        renderer.m_depthPyramidViewportHeight != viewportHeight ||
        renderer.m_depthPyramidFrameSerial + 1u != contexts->GetPreparedFrameSerial())
        return nullptr;
    // The pyramid is last frame's view: valid while the camera has moved and
    // turned little (a cut, teleport or fast turn falls back to the CPU
    // terrain pyramid for one frame instead of hiding newly visible things).
    const glm::vec3 position = ViewPosition(view);
    if (glm::length(position - renderer.m_depthPyramidPosition) > 1.5f ||
        glm::dot(ViewForward(view), renderer.m_depthPyramidForward) < 0.99f)
        return nullptr;
    return renderer.m_depthPyramid.get();
}

const Engine::Graphics::IGraphicsBuffer* SceneRendererLighting::SceneDepthForPixelShaders(
    SceneRenderer& renderer, Engine::Graphics::IGraphicsContext* context)
{
    using Engine::Graphics::IGraphicsBuffer;
    const uint32_t width = renderer.m_sceneDepthWidth, height = renderer.m_sceneDepthHeight;
    if (!context || !context->SupportsSceneDepthCopy() || width < 1u || height < 1u ||
        width > 8192u || height > 8192u || renderer.m_sceneDepthReconstruct.y == 0.f)
        return nullptr;
    auto* buffers = renderer.m_graphicsProvider
        ? renderer.m_graphicsProvider->GetBufferFactory() : nullptr;
    const uint64_t depthBytes = uint64_t(Engine::Graphics::SceneDepthRowPitch(width)) *
        height * sizeof(float);
    if (!renderer.m_sceneDepthCopied &&
        (!renderer.m_ambientOcclusionDepth || renderer.m_ambientOcclusionDepthBytes < depthBytes))
    {
        if (!buffers) return nullptr;
        const std::vector<uint8_t> zeros(size_t(depthBytes), 0u);
        renderer.m_ambientOcclusionDepth = buffers->CreateBuffer(IGraphicsBuffer::Usage::StorageBuffer,
            IGraphicsBuffer::AccessMode::Default, depthBytes, zeros.data(), sizeof(float));
        renderer.m_ambientOcclusionDepthBytes = renderer.m_ambientOcclusionDepth ? depthBytes : 0;
    }
    if (!renderer.m_ambientOcclusionDepth || !context->BeginComputePass())
        return nullptr;
    if (!renderer.m_sceneDepthCopied)
    {
        context->CopySceneDepth(renderer.m_ambientOcclusionDepth.get(), width, height);
        renderer.m_sceneDepthCopied = true;
    }
    // Into a state pixel shaders can read (DX12), visible to them (Vulkan).
    context->StorageBarrier(renderer.m_ambientOcclusionDepth.get());
    context->EndComputePass();
    return renderer.m_ambientOcclusionDepth.get();
}

void SceneRendererLighting::MarchVolumetricClouds(
    SceneRenderer& renderer,
    Engine::Graphics::IGraphicsContext* context,
    const glm::mat4& view,
    const glm::mat4& proj,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool shadowOnly,
    const glm::vec3& cameraPosition,
    const glm::vec3& sunDirection,
    const glm::vec3& sunRadiance)
{
    renderer.m_cloudsMarched = false;
    if (shadowOnly || !context || !viewportWidth || !viewportHeight)
        return;
    Scene& scene = renderer.GetScene();
    const auto& clouds = scene.settings.volumetricClouds;
    if (!clouds.enabled || scene.IsMode2D() || !renderer.m_cloudPipeline ||
        !renderer.m_cloudMarchPipeline || !renderer.m_skyboxCBMapped)
        return;

    // Half resolution, rounded up; the grid size travels bit-packed in the
    // constants (12 bits per axis).
    const uint32_t width = std::min((viewportWidth + 1u) / 2u, 4095u);
    const uint32_t height = std::min((viewportHeight + 1u) / 2u, 4095u);
    const uint64_t count = static_cast<uint64_t>(width) * height;
    if (!renderer.m_cloudSamples || renderer.m_cloudSampleCapacity < count)
    {
        auto* buffers = renderer.m_graphicsProvider
            ? renderer.m_graphicsProvider->GetBufferFactory() : nullptr;
        if (!buffers)
            return;
        // The previous buffer may still be read by frames in flight.
        if (renderer.m_cloudSamples)
            renderer.RetiredSceneBuffers().push_back(std::move(renderer.m_cloudSamples));
        renderer.m_cloudSamples = buffers->CreateBuffer(
            Engine::Graphics::IGraphicsBuffer::Usage::StorageBuffer,
            Engine::Graphics::IGraphicsBuffer::AccessMode::Default,
            count * sizeof(glm::vec4), nullptr, sizeof(glm::vec4));
        renderer.m_cloudSampleCapacity = renderer.m_cloudSamples ? count : 0;
        if (!renderer.m_cloudSamples)
            return;
    }

    ViewLighting::SkyboxConstants rays{};
    ViewLighting::SetSkyboxRays(rays, proj, glm::mat4(glm::mat3(view)));
    const float time = static_cast<float>(scene.GetElapsedTime());
    const float wind = glm::radians(clouds.windDirection);
    const glm::vec3 toSun = glm::dot(sunDirection, sunDirection) > 1e-8f
        ? glm::normalize(sunDirection) : glm::vec3(0.f, 1.f, 0.f);
    ViewLighting::CloudConstants data{};
    data.rayX = glm::vec4(glm::vec3(rays.rayX), time);
    data.rayY = glm::vec4(glm::vec3(rays.rayY), clouds.baseHeight);
    data.rayOrigin = glm::vec4(glm::vec3(rays.rayOrigin), clouds.baseHeight + clouds.thickness);
    data.camera = glm::vec4(cameraPosition, clouds.coverage);
    data.sunDirection = glm::vec4(toSun, clouds.density);
    data.sunRadiance = glm::vec4(sunRadiance * clouds.sunIntensity, clouds.maxDistance);
    data.ambient = glm::vec4(clouds.ambientColor * clouds.ambientIntensity, clouds.shapeScale);
    // Wind moves the noise field; wrap the offset at a large multiple of the
    // shape scale so float precision holds over long sessions.
    const float travel = std::fmod(clouds.windSpeed * time, 4096.f * clouds.shapeScale);
    const uint32_t packed = width | (height << 12u) |
        (static_cast<uint32_t>(std::clamp(clouds.steps, 1, 255)) << 24u);
    float packedBits = 0.f;
    std::memcpy(&packedBits, &packed, sizeof(packedBits));
    data.shape = glm::vec4(-std::cos(wind) * travel, clouds.detail,
        -std::sin(wind) * travel, packedBits);
    // The composite reads the same constants from the sky ring.
    std::memcpy(static_cast<uint8_t*>(renderer.m_skyboxCBMapped) +
        static_cast<uint64_t>(SceneRenderer::kCloudConstantSlot) * SceneRenderer::kCBStride,
        &data, sizeof(data));

    if (!context->BeginComputePass())
        return;
    if (context->SetComputePipeline(renderer.m_cloudMarchPipeline.get()))
    {
        context->BeginGpuTiming(Engine::Graphics::GpuTimingStage::Clouds);
        context->SetComputeWriteBuffer(renderer.m_cloudSamples.get());
        context->SetComputeConstants(&data, sizeof(data));
        context->DispatchCompute(static_cast<uint32_t>((count + 63u) / 64u));
        context->StorageBarrier(renderer.m_cloudSamples.get());
        context->EndGpuTiming(Engine::Graphics::GpuTimingStage::Clouds);
        renderer.m_cloudsMarched = true;
    }
    context->EndComputePass();
}

void SceneRendererLighting::PrepareSkyboxAndEnvironment(
    SceneRenderer& renderer,
    Engine::Graphics::IGraphicsContext* context,
    const glm::mat4& view,
    const glm::mat4& proj,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool shadowOnly,
    const glm::vec3& cameraPosition,
    const std::array<glm::vec4, 4>& atmosphereFog,
    const glm::vec3& sunDirection,
    const glm::vec3& sunRadiance)
{
    Scene& scene = renderer.GetScene();
    if (!shadowOnly)
        ResolveSkySun(renderer, sunDirection, sunRadiance,
            renderer.m_skySunDirection, renderer.m_skySunRadiance);
    const Engine::Components::Texture* skybox = shadowOnly
        ? nullptr
        : renderer.ResolveSkyboxTexture();
    if (!shadowOnly)
        renderer.UpdateEnvironmentLighting(skybox);

    if (!shadowOnly && skybox && skybox->GetGraphicsTexture() && renderer.m_skyboxPipeline)
    {
        // A panorama repeat spans the full environment, beyond the camera FOV.
        Engine::Components::Texture::RequestGraphicsFootprint(
            skybox->GetGraphicsTexture(),
            4.f * float(std::max(viewportWidth, viewportHeight)));

        ViewLighting::SkyboxConstants skyboxData{};
        skyboxData.sunDirection = renderer.m_skySunDirection;
        skyboxData.sunRadiance = renderer.m_skySunRadiance;
        // A skybox represents direction only. Excluding camera translation
        // prevents cancellation in the far - near ray math when the editor
        // camera is far from the world origin.
        ViewLighting::SetSkyboxRays(skyboxData, proj, glm::mat4(glm::mat3(view)));
        if (!scene.IsMode2D())
            ViewLighting::SetSkyboxFog(skyboxData, atmosphereFog,
                scene.settings.atmosphereFog.skyDistance, cameraPosition.y);
        // The physical panorama is world-space radiance: no HDRI exposure
        // or rotation, just its own intensity.
        const bool physicalSky = skybox == renderer.m_physicalSky.Panorama().get();
        skyboxData.displayParams = {
            scene.IsMode2D() ? 1.f : 0.f,
            physicalSky ? scene.settings.physicalSky.skyIntensity
                : std::exp2(scene.settings.hdriExposure),
            physicalSky ? 0.f : glm::radians(scene.settings.hdriRotation),
            // The physical panorama already holds the Mie aureole.
            physicalSky ? 0.f : std::max(scene.settings.sunGlowIntensity, 0.f)};

        std::memcpy(renderer.m_skyboxCBMapped, &skyboxData, sizeof(skyboxData));
        context->SetPipeline(renderer.m_skyboxPipeline.get());
        context->SetConstantBuffer(0, renderer.m_skyboxConstantBuffer.get(), 0);
        context->SetTexture(0, skybox->GetGraphicsTexture());
        context->BeginGpuTiming(Engine::Graphics::GpuTimingStage::Sky);
        context->DrawInstanced(3, 1, 0, 0);

        // Volumetric clouds: composite this view's half-resolution march
        // (MarchVolumetricClouds) over the sky. The samples borrow
        // structured-buffer slot 6, which holds the scene lights; restore it.
        if (renderer.m_cloudsMarched && renderer.m_cloudPipeline && renderer.m_cloudSamples)
        {
            const uint64_t offset = static_cast<uint64_t>(SceneRenderer::kCloudConstantSlot) *
                SceneRenderer::kCBStride;
            context->SetPipeline(renderer.m_cloudPipeline.get());
            context->SetConstantBuffer(0, renderer.m_skyboxConstantBuffer.get(), offset);
            context->SetStructuredBuffer(6, renderer.m_cloudSamples.get());
            context->DrawInstanced(3, 1, 0, 0);
            context->SetStructuredBuffer(6, renderer.m_lightDataBuffer.get());
        }
        context->EndGpuTiming(Engine::Graphics::GpuTimingStage::Sky);
    }
}
}