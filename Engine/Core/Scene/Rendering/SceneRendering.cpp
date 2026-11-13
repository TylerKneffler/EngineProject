#include "pch.h"
#include "Core/Rendering/ShadowReceiverCulling.h"
#include "Core/Rendering/StableShadowCascade.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Rendering/PreparedInstanceDraw.h"
#include "Core/Scene/Scene.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Terrain/Terrain.h"
#include "Core/Compoonents/Terrain/TerrainManager.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Compoonents/Terrain/TerrainBatchAppearance.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Compoonents/Sprite/Sprite.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Rendering/TextureFootprint.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Rendering/Lighting/EnvironmentMap.h"
#include "Core/Compoonents/Lighting/LightProbe.h"
#include "Core/Rendering/Portal/PortalRenderPolicy.h"
#include "Core/Model/LightingData.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IShader.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/PostProcess.h"
#include "Core/Renderers/UIRenderer.h"
#include "Core/Scene/Rendering/SceneRenderer.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneInstanceBatching.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneTextureFootprints.h"
#include "Core/Scene/Rendering/SceneRenderer/ScenePortalViewScheduling.h"
#include "Core/Scene/Rendering/SceneRenderer/ScenePortalPassPreparation.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneSpatialDebugGeometry.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneNativeRenderPacket.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererShadows.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererViewPreparation.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererMaterialShaders.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererObjectRecordUpload.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererGeometrySubmission.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererLodSelection.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDirectionalShadowSubmission.h"
#include "Core/Scene/Rendering/SceneRenderer/ScenePortalRendering.h"
#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <set>

#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

namespace Engine::Scene
{
void Scene::CaptureEmbeddedUiInput(Engine::Core::NativeInputSnapshot& input) const
{
    AssertOwnerThread();
    if (m_uiRenderer) m_uiRenderer->CaptureEmbeddedPointer(input);
}

void Scene::SetUiPointerInput(float x, float y, float viewportWidth,
    float viewportHeight, bool hovered, bool mouseDown)
{
    if (m_uiRenderer)
        m_uiRenderer->SetPointerInput(x, y, viewportWidth, viewportHeight,
            hovered, mouseDown);
}


#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace
{
    using Engine::Scene::Renderer::ExpandedCapacity;
    using Engine::Scene::Renderer::EngineShaderPath;
    using Engine::Scene::Renderer::HashRevision;
    using Engine::Scene::Renderer::IntegrateBrdf;
    using Engine::Scene::Renderer::IsFastFoliageSurface;
    using Engine::Scene::Renderer::IsObjectOrDescendant;
    using Engine::Scene::Renderer::RadicalInverseVdc;
    using Engine::Scene::Renderer::ReadEnvFlag;

    std::vector<float> GenerateBrdfIntegrationLut(uint32_t resolution)
    {
        std::vector<float> pixels(static_cast<size_t>(resolution) *
            resolution * 4u, 0.f);
        for (uint32_t y = 0; y < resolution; ++y)
        for (uint32_t x = 0; x < resolution; ++x)
        {
            const float noV = (static_cast<float>(x) + 0.5f) /
                static_cast<float>(resolution);
            const float roughness = (static_cast<float>(y) + 0.5f) /
                static_cast<float>(resolution);
            const glm::vec2 integrated = IntegrateBrdf(noV, roughness);
            const size_t offset = (static_cast<size_t>(y) * resolution + x) * 4u;
            pixels[offset] = integrated.x;
            pixels[offset + 1u] = integrated.y;
            pixels[offset + 3u] = 1.f;
        }
        return pixels;
    }
}

// ---------------------------------------------------------------------------
// Constant buffer data structures
// ---------------------------------------------------------------------------

// Per-draw constants stay deliberately small. Vulkan sends these as push
// constants; DirectX binds them through its native constant-buffer path.
using Renderer::DrawCBData;
using Renderer::ObjectGPUData;
using Renderer::kPortalApertureNearClampFlag;
using Renderer::kPortalDepthResetFlag;
using Renderer::kClusteredLightingFlag;

// Value-only boundary: resources are resolved by the scene owner before assembly.
// Texture pointers are binding identities; this function never dereferences them.
struct ResolvedMaterialRecord
{
    ObjectGPUData data{};
    std::array<const Engine::Graphics::IGraphicsTexture*, 30> textures{};
    bool doubleSided = false;
    Engine::Components::MaterialAlphaMode alphaMode{};
};

void ApplyResolvedMaterialRecord(const ResolvedMaterialRecord& material,
    const glm::vec3& cameraPosition, const glm::vec3& irradiance,
    ObjectGPUData& objectData)
{
    const auto& cachedData = material.data;
    objectData.ambientUnlit=cachedData.ambientUnlit;
    objectData.baseColor=cachedData.baseColor;
    objectData.blinnPhongSpecularShininess=cachedData.blinnPhongSpecularShininess;
    objectData.emissiveOcclusion=cachedData.emissiveOcclusion;
    objectData.environmentParams=cachedData.environmentParams;
    objectData.materialParams=cachedData.materialParams;
    objectData.parallaxParams=cachedData.parallaxParams;
    objectData.pmxEdgeColor=cachedData.pmxEdgeColor;
    objectData.pmxParams=cachedData.pmxParams;
    objectData.pmxSphereTint=cachedData.pmxSphereTint;
    objectData.pmxTextureTint=cachedData.pmxTextureTint;
    objectData.pmxToonTint=cachedData.pmxToonTint;
    objectData.reflectionEnvironmentParams=cachedData.reflectionEnvironmentParams;
    std::copy(std::begin(cachedData.reflectionEnvironmentSH),std::end(cachedData.reflectionEnvironmentSH),objectData.reflectionEnvironmentSH);
    objectData.subsurfaceColorWeight=cachedData.subsurfaceColorWeight;
    objectData.subsurfaceRadiusThickness=cachedData.subsurfaceRadiusThickness;
    objectData.subsurfaceTransmission=cachedData.subsurfaceTransmission;
    objectData.textureUvSets0=cachedData.textureUvSets0;
    objectData.textureUvSets1=cachedData.textureUvSets1;
    objectData.viewPositionAlphaCutoff=cachedData.viewPositionAlphaCutoff;
    objectData.ambientUnlit += glm::vec4(irradiance, 0.f);
    objectData.viewPositionAlphaCutoff = glm::vec4(cameraPosition,
        cachedData.viewPositionAlphaCutoff.w);
}

static_assert(sizeof(DrawCBData) == 16, "Draw constants must remain small");
static_assert(offsetof(ObjectGPUData, grassCapsules) == 1552, "Grass capsule data must follow the existing object fields");
static_assert(sizeof(ObjectGPUData) == 1680, "Object buffer layout must match Object.hlsl");
static_assert(sizeof(Engine::Model::LightData) == 128,
    "Light buffer layout must match Object.hlsl");

// ---------------------------------------------------------------------------
// Scene::Init
// ---------------------------------------------------------------------------

void Scene::Render(Engine::Graphics::IGraphicsContext* context, float aspect,
    Engine::Components::Camera* cameraOverride, bool includeEditorVisuals,
    uint32_t viewportWidth, uint32_t viewportHeight, bool shadowOnly,
    const NativeRenderPacket* nativePacket, int instanceLodDisplayOverride,
    int sceneViewRenderMode)
{
    // Render phase 1: validate the prepared frame and resolve its active camera.
    m_screenLodCounts={};
    const auto quality = settings.graphicsQuality.Resolve();
    const bool previousNativeRecording = m_recordingNativeCommands;
    m_recordingNativeCommands = nativePacket && IsOwnerThread();
    struct NativeRecordingGuard
    {
        bool& current;
        bool previous;
        ~NativeRecordingGuard() { current = previous; }
    } nativeRecordingGuard{m_recordingNativeCommands, previousNativeRecording};
    struct NativePass
    {
        Engine::Graphics::IGraphicsContext* context;
        Engine::Graphics::NativePhase previous;
        ~NativePass() { if(context) context->SetNativeTimingPhase(previous); }
    } nativePass{context,context ? context->GetNativeTimingPhase():Engine::Graphics::NativePhase::Other};
    if(context) context->SetNativeTimingPhase(shadowOnly ? Engine::Graphics::NativePhase::Shadow:Engine::Graphics::NativePhase::Camera);
    using RenderClock = std::chrono::steady_clock;
    m_lastRenderCpuTimings={};
    struct RenderMeasurement
    {
        RenderCpuTimings& metrics;
        const uint32_t& draws; const uint64_t& triangles;
        Engine::Rendering::RenderJobs::Statistics jobs=Engine::Rendering::RenderJobs::Get().ReadStatistics();
        RenderClock::time_point start=RenderClock::now();
        ~RenderMeasurement()
        {
            metrics.totalMs=std::chrono::duration<double,std::milli>(RenderClock::now()-start).count();
            const auto endJobs=Engine::Rendering::RenderJobs::Get().ReadStatistics();
            metrics.workerActiveMs=double(endJobs.workerNs-jobs.workerNs)/1.e6;
            metrics.mainJobMs=double(endJobs.callerNs-jobs.callerNs)/1.e6;
            metrics.workerWaitMs=double(endJobs.waitNs-jobs.waitNs)/1.e6;
            metrics.workerQueueMs=double(endJobs.queueNs-jobs.queueNs)/1.e6;
            metrics.workerDependencyMs=double(endJobs.dependencyNs-jobs.dependencyNs)/1.e6;
            metrics.configuredWorkers=endJobs.workers;
            metrics.workerJobs=endJobs.jobs-jobs.jobs; metrics.workerTasks=endJobs.workerTasks-jobs.workerTasks;
            metrics.otherMs=std::max(0.,metrics.totalMs-metrics.startupMs-metrics.terrainPreparationMs-
                metrics.cullingMs-metrics.sortingMs-metrics.occlusionMs-metrics.materialMs-
                metrics.batchingMs-metrics.uploadMs-metrics.submissionMs-metrics.retainedUiMs-metrics.portalMs);
            metrics.physicalDraws=draws; metrics.candidateTriangles=triangles; metrics.views=1;
        }
    } measurement{m_lastRenderCpuTimings,m_lastOrdinaryDrawCount,m_lastOrdinaryTriangleCount};
    const auto shadowCpuStart = std::chrono::steady_clock::now();
    if (shadowOnly)
        m_lastShadowCpuTiming = {};
    const auto shadowElapsedMs = [](auto start, auto end)
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    };
    m_lastObjectDataUploadBytes = 0;
    m_lastOcclusionCulledCount = 0;
    m_lastTerrainOcclusionCulledCount = 0;
    m_lastParticlePortalDrawCount = 0;
    if (shadowOnly) m_shadowDebugSnapshot = {};
    else
    {
        m_shadowDebugSnapshot.bakedReceiverCount = 0;
        m_shadowDebugSnapshot.dynamicReceiverCount = 0;
    }
    if (!context)
    {
        return;
    }

    if (!m_graphicsProvider || !m_objectPipeline)
    {
        return;
    }
    if(auto* packet=dynamic_cast<Engine::Graphics::GraphicsCommandPacket*>(context))
    {
        const auto* factory=m_graphicsProvider->GetContextFactory();
        packet->SetDepthRectangleClearSupported(factory && factory->SupportsDepthRectangleClear());
    }

    if (!shadowOnly)
        Engine::Graphics::SetPostProcessSettings({
            std::exp2(std::clamp(settings.outputExposure, -16.f, 16.f)),
            static_cast<uint32_t>(settings.toneMapping) });

    if (m_nativeRenderPacketTaken && !nativePacket)
        throw std::logic_error("Native frame draw list was transferred to a render packet");
    if (!m_renderFramePrepared)
        PrepareRenderFrame();
    const auto& frameRenderItems = nativePacket ? nativePacket->items : m_frameRenderItems;
    m_lastRenderCpuTimings.visibilityFrameItems += frameRenderItems.size();
    const auto& frameItemSlotOffsets = nativePacket
        ? nativePacket->renderSlotItemOffsets : m_frameItemsByRenderSlotOffsets;
    const auto& frameItemSlotIndices = nativePacket
        ? nativePacket->renderSlotItems : m_frameItemsByRenderSlot;
    uint32_t requiredObjectSlots=1;
    for(const auto& item:frameRenderItems)
    {
        if(item.objectSlot==UINT32_MAX)
            throw std::runtime_error("Render object slot exceeds buffer index range");
        requiredObjectSlots=std::max(requiredObjectSlots,item.objectSlot+1u);
    }
    // Persistent slots can be sparse after LOD/visibility changes. Size the
    // selected frame/view buffers by their highest addressed slot, not draw count.
    SelectRenderPassBuffers(requiredObjectSlots);
    // Validate on the owner before record-packing/cascade jobs can touch maps.
    const uint64_t shadowRecords=uint64_t(m_objectCapacity)*4u;
    if(requiredObjectSlots>m_objectCapacity || !m_objectDataMapped || !m_objectCBMapped ||
        !m_shadowObjectDataMapped || !m_shadowCBMapped ||
        m_objectDataBuffer->GetSize()<uint64_t(requiredObjectSlots)*sizeof(ObjectGPUData) ||
        m_objectConstantBuffer->GetSize()<uint64_t(requiredObjectSlots)*kCBStride ||
        m_shadowObjectDataBuffer->GetSize()<shadowRecords*sizeof(ObjectGPUData) ||
        m_shadowConstantBuffer->GetSize()<shadowRecords*kCBStride)
        throw std::runtime_error("Render pass upload buffers do not cover persistent object slots");
    m_lastOrdinaryDrawCount = 0;
    m_lastOrdinaryTriangleCount = 0;
    const uint32_t portalRenderSlotsPerView = m_objectCapacity + 2u;
    const uint32_t portalObjectSlotCount =
        kMaxPortalRenderViews * portalRenderSlotsPerView;

    // Scene View always uses its navigation camera. Game View supplies its
    // active scene camera explicitly, so hierarchy selection cannot hijack
    // either viewport.
    Engine::Components::Camera* cam = cameraOverride
        ? cameraOverride
        : editorCamera.GetComponent<Engine::Components::Camera>();
    if (!cam)
    {
        return;
    }

    // Render phase 2: establish the camera chart and per-view transforms.

    const auto instanceSerial=m_graphicsProvider->GetContextFactory()->GetPreparedFrameSerial();
    const bool newInstanceFrame=m_instanceViewSerial!=instanceSerial;
    if(m_instanceViewSerial!=instanceSerial) { m_instanceViewSerial=instanceSerial; m_instanceViewUses.clear(); }
    const uint32_t instanceViewOrdinal=m_instanceViewUses[{cam,shadowOnly}]++;
    const uint64_t diagnosticViewIdentity =
        (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cam)) >> 4u) ^
        (static_cast<uint64_t>(instanceViewOrdinal) << 48u) ^
        (shadowOnly ? (uint64_t(1) << 63u) : 0u);
    m_persistentInstances.BeginFrame(m_graphicsProvider);
    const auto poolBefore=m_persistentInstances.stats;
    m_lastRenderCpuTimings.instances.retirements=newInstanceFrame ? poolBefore.retirements:0;
    m_lastRenderCpuTimings.instances.residentBytes=m_persistentInstances.ResidentBytes();
    glm::mat4 authoredCameraWorld(1.f);
    bool hasAuthoredCameraWorld = false;
    bool cameraInsideInteriorOnlyWarp = false;
    if (cam->Owner)
    {
        authoredCameraWorld = cam->Owner->transform.GetWorldMatrix();
        if (cam->Owner->transform.matrixLayer.enabled)
        {
            authoredCameraWorld = cam->Owner->transform.matrixLayer.localToLayer *
                authoredCameraWorld;
        }
        hasAuthoredCameraWorld = true;
        const glm::vec3 authoredCameraPosition(authoredCameraWorld[3]);
        for(const auto* volume:m_frameSpatialManipulatorScratch)
            if(volume && volume->Owner && volume->Owner->IsEnabledInHierarchy() &&
                volume->enabled && volume->definesWarpVolume && volume->renderWarpInteriorOnly &&
                volume->ContainsWorldPoint(authoredCameraPosition))
            { cameraInsideInteriorOnlyWarp=true; break; }

    }

    glm::mat4 view = cam->GetViewMatrix();
    // Camera::GetViewMatrix uses the source chart for a camera already inside
    // a warp volume. The editor needs an explicit opt-in for that behavior so
    // its default view remains an accurate inspection of the embedded space.
    if (((!cameraOverride && !settings.sceneCameraWarpLookThrough) ||
        cameraInsideInteriorOnlyWarp) && !m_editorMode2D && cam->Owner)
    {
        glm::mat4 embeddedWorld = authoredCameraWorld;
        embeddedWorld = MapSpatialMatrix(embeddedWorld,
            { SpatialQueryDomain::Camera, cam->Owner });
        const glm::vec3 eye = glm::vec3(embeddedWorld[3]);
        if (!cam->useTransformRotation)
        {
            const glm::vec3 target = MapSpatialPoint(cam->target,
                { SpatialQueryDomain::Camera });
            view = glm::lookAtLH(eye, target, cam->up);
        }
        else
        {
            const glm::vec3 forward = glm::normalize(glm::vec3(embeddedWorld[2]));
            const glm::vec3 up = glm::normalize(glm::vec3(embeddedWorld[1]));
            view = glm::lookAtLH(eye, eye + forward, up);
        }
    }
    if (m_editorMode2D && cam->Owner)
    {
        const glm::vec3 cameraWorld = cam->Owner->transform.GetWorldPosition();
        const glm::vec3 eye(cameraWorld.x, cameraWorld.y, -10.f);
        view = glm::lookAtLH(eye, eye + glm::vec3(0.f, 0.f, 1.f),
            glm::vec3(0.f, 1.f, 0.f));
    }
    const glm::mat4 proj = cam->GetProjectionMatrix(aspect, m_editorMode2D);
    const glm::mat4 inverseView = glm::inverse(view);
    const glm::vec3 cameraPosition = glm::vec3(inverseView[3]);
    const glm::vec3 cameraDirection = glm::normalize(glm::vec3(inverseView[2]));
    const auto grassInteraction = Engine::Rendering::SelectGrassInteractionCapsules(
        nativePacket ? nativePacket->grassCapsules : m_frameGrassCapsules, cameraPosition);
    bool cameraUsesSourceWarpChart = false;
    if (hasAuthoredCameraWorld)
    {
        cameraUsesSourceWarpChart = Engine::Scene::Renderer::SceneRendererViewPreparation::ResolveCameraUsesSourceWarpChart(
            *this,
            cam,
            authoredCameraWorld,
            cameraInsideInteriorOnlyWarp,
            cameraOverride,
            m_editorMode2D,
            proj,
            view,
            m_frameSpatialManipulatorScratch,
            settings.sceneCameraWarpLookThrough);
    }
    // A source-chart view continues through a finite nonlinear volume instead
    // of projecting its already embedded bend with a straight raster camera.
    // Editor views stay embedded unless Spatial Debug explicitly enables the
    // Scene Camera Warp Look-Through option.
    const auto sourceChartWorld = [](const FrameRenderItem& item)
    {
        return item.authoredWorld;
    };
    const auto useSourceChartMesh = [&](const FrameRenderItem& item)
    {
        return cameraUsesSourceWarpChart && item.mesh && !item.sprite &&
            glm::all(glm::equal(item.traversalClipPlane, glm::vec4(0.f)));
    };
    // Render phase 3: prepare view lighting, clustered lights, and shadows.
    // Light data is view-chart-specific. Rebuild it for every view so an
    // inside-volume game camera cannot leave source-chart lights bound when
    // the editor view subsequently renders the embedded chart (or vice versa).
    Engine::Model::RealtimeShadowSelection shadowSelection{};
    std::array<std::string, 4> cookiePaths{};
    std::array<std::string, 4> iesPaths{};
    m_frameLightCount = m_realtimeLightingPipeline.CollectLights(*this,
        static_cast<Engine::Model::LightData*>(m_lightDataMapped),
        std::min(kMaxLights,
            m_distanceLightingSettings.maximumRealtimeLights),
        !cameraUsesSourceWarpChart, &m_realtimeShadowSettings,
        &shadowSelection, &cameraPosition, &cookiePaths, &iesPaths);
    if (shadowOnly && !shadowSelection.valid)
    {
        m_shadowDebugSnapshot.enabled = m_realtimeShadowSettings.enabled;
        m_lastShadowCpuTiming.scenePreparationMs = shadowElapsedMs(
            shadowCpuStart, std::chrono::steady_clock::now());
        m_lastShadowCpuTiming.totalMs =
            m_lastShadowCpuTiming.scenePreparationMs;
        return;
    }
    auto viewLighting = Engine::Scene::Renderer::SceneRendererLighting::ResolveViewLighting(
        *this,
        cameraPosition,
        shadowOnly,
        static_cast<const Engine::Model::LightData*>(m_lightDataMapped),
        m_frameLightCount,
        cookiePaths,
        iesPaths);
    const glm::vec3 terrainSunDirection = viewLighting.terrainSunDirection;
    const glm::vec3 terrainSunRadiance = viewLighting.terrainSunRadiance;
    const int terrainSunIndex = viewLighting.terrainSunIndex;
    const auto photometryTextures = viewLighting.photometryTextures;
    if (!shadowOnly)
        Engine::Scene::Renderer::SceneRendererLighting::BindPhotometryTextures(
            context,
            photometryTextures);
    m_shadowDebugSnapshot.enabled = m_realtimeShadowSettings.enabled;

    const auto clusterConfig = Engine::Scene::Renderer::SceneRendererLighting::PrepareClusteredLighting(
        m_graphicsProvider,
        view,
        proj,
        cam->nearPlane, cam->farPlane,
        viewportWidth,
        viewportHeight,
        static_cast<const Engine::Model::LightData*>(m_lightDataMapped),
        m_frameLightCount,
        m_distanceLightingSettings,
        shadowOnly,
        m_clusterHeaderBuffer,
        m_clusterHeaderMapped,
        m_clusterHeaderCapacity,
        m_clusterLightIndexBuffer,
        m_clusterLightIndexMapped,
        m_clusterLightIndexCapacity,
        m_clusterHeaderScratch,
        m_clusterLightIndexScratch,
        m_retiredRenderBuffers,
        m_directionalClusterLayouts,
        &m_lastRenderCpuTimings.clusterPreparationMs,
        &m_lastRenderCpuTimings.clusterUploadBytes,
        &m_lastRenderCpuTimings.clusterCacheHits);
    RecordFrameUploadBytes(m_lastRenderCpuTimings.clusterUploadBytes);
    const uint32_t clusterTileSize = std::clamp(m_distanceLightingSettings.clusterTileSize, 16u, 256u);
    const float clusterNear = std::max(0.001f, cam->nearPlane);
    const float clusterFar = std::max(clusterNear + 0.001f, cam->farPlane);
    const float clusterLogScale = static_cast<float>(std::clamp(m_distanceLightingSettings.clusterDepthSlices, 1u, 64u)) /
        std::max(std::log2(clusterFar / clusterNear), 0.0001f);
    const bool clusteredLightingEnabled = clusterConfig.enabled;
    const uint32_t clusterTilesX = clusterConfig.clusterTilesX;
    const uint32_t clusterTilesY = clusterConfig.clusterTilesY;
    const uint32_t clusterDepthSlices = clusterConfig.clusterDepthSlices;

    if (shadowSelection.valid)
    {
        const uint32_t resolution = std::clamp(static_cast<uint32_t>(
            static_cast<float>(m_realtimeShadowSettings.directionalResolution) *
            shadowSelection.resolutionScale), 256u, 8192u);
        if (!m_directionalShadowMap ||
            m_directionalShadowResolution != resolution)
        {
            auto* textureFactory = m_graphicsProvider->GetTextureFactory();
            m_directionalShadowMap = textureFactory
                ? textureFactory->CreateDepthTexture2D(resolution, resolution)
                : nullptr;
            m_directionalShadowResolution = m_directionalShadowMap
                ? resolution : 0u;
            if (m_directionalShadowMap)
                ++m_directionalShadowAllocationGeneration;
        }
        if (!m_directionalShadowMap)
        {
            auto* lights = static_cast<Engine::Model::LightData*>(
                m_lightDataMapped);
            lights[shadowSelection.lightIndex].params.z = -1.f;
            lights[shadowSelection.lightIndex].params.w = 0.f;
            shadowSelection.valid = false;
        }
    }
    m_lightDataBuffer->FlushMappedWrites();
    RecordFrameUploadBytes(static_cast<uint64_t>(m_frameLightCount) *
        sizeof(Engine::Model::LightData));
    m_shadowDebugSnapshot.hasSelectedLight = shadowSelection.valid;
    m_shadowDebugSnapshot.selectedLight = shadowSelection.stableLightKey;
    m_shadowDebugSnapshot.directionToLight =
        shadowSelection.directionToLight;
    m_shadowDebugSnapshot.atlasResolution = shadowSelection.valid
        ? m_directionalShadowResolution : 0u;
    m_shadowDebugSnapshot.atlasAllocationGeneration =
        m_directionalShadowAllocationGeneration;
    // Display modes belong to the editor scene viewport. Runtime/game views
    // always use the authored lit rendering path.
    const auto renderMode = includeEditorVisuals && sceneViewRenderMode >= 0
        ? static_cast<Engine::Model::SceneRenderMode>(sceneViewRenderMode)
        : Engine::Model::SceneRenderMode::Lit;
    const bool wireframeMode = renderMode ==
        Engine::Model::SceneRenderMode::Wireframe;
    const bool forceUnlitMode = renderMode ==
        Engine::Model::SceneRenderMode::Unlit;
    const auto cascadePreparation = Engine::Scene::Renderer::SceneRendererShadows::PrepareDirectionalShadowCascades(
        *this,
        context,
        shadowSelection,
        view,
        cameraPosition,
        quality,
        cam,
        shadowOnly);
    const auto& mainShadowCascades = cascadePreparation.cascades;
    const auto& directionalShadowDirtyMask = cascadePreparation.dirtyMask;
    const bool renderMainDirectionalShadow = cascadePreparation.renderMainDirectionalShadow;
    const bool preserveDirectionalShadowAtlas = cascadePreparation.preserveDirectionalShadowAtlas;
    const auto& shadowViewProjections = mainShadowCascades.viewProjections;
    const glm::vec4 shadowCascadeSplits = mainShadowCascades.splits;
    const uint32_t shadowCascadeCount = mainShadowCascades.count;
    if (shadowSelection.valid)
    {
        m_shadowDebugSnapshot.atlasResolution =
            m_directionalShadowResolution;
        m_shadowDebugSnapshot.cascadeCount = shadowCascadeCount;
        m_shadowDebugSnapshot.cascadeSplits = shadowCascadeSplits;
        m_shadowDebugSnapshot.cascadeCenters = mainShadowCascades.centers;
        m_shadowDebugSnapshot.cascadeRadii = mainShadowCascades.radii;
    }

    const auto* skybox = shadowOnly ? nullptr : ResolveSkyboxTexture();
    Engine::Scene::Renderer::SceneRendererLighting::PrepareSkyboxAndEnvironment(
        *this,
        context,
        view,
        proj,
        viewportWidth,
        viewportHeight,
        shadowOnly);

    // Render phase 4: build the visible, sorted draw candidates.
    // Opaque and masked materials render first. Blended materials render
    // back-to-front with depth writes disabled. Build the complete view sort
    // key once so the comparator performs field comparisons only.
    const glm::mat4 viewProjection = proj * view;
    const auto resolvePortalTarget = [&](Engine::Components::SpatialManipulator* source)
        -> Engine::Components::SpatialManipulator*
    {
        if (!source)
            return nullptr;
        if (Engine::Components::SpatialManipulator* target = source->ResolveTarget())
            return target;
        // Support older and edit-mode scenes whose serialized link exists on
        // only one endpoint. Connections belong to manipulators, not meshes,
        // so inspect every scene object rather than the render draw list.
        for (Engine::Components::SpatialManipulator* candidate :
            m_frameSpatialManipulatorScratch)
        {
            if (!candidate || candidate->Owner == source->Owner)
                continue;
            if (candidate->ResolveTarget() == source)
                return candidate;
        }
        return nullptr;
    };
    const bool hasPortalViews = std::any_of(
        m_frameSpatialManipulatorScratch.begin(),
        m_frameSpatialManipulatorScratch.end(),
        [&](Engine::Components::SpatialManipulator* portal)
        {
            if (!portal || !portal->Owner ||
                !portal->Owner->IsEnabledInHierarchy() || !portal->enabled)
                return false;
            const auto mode = static_cast<Engine::Components::
                SpatialManipulator::ConnectionMode>(portal->connectionMode);
            if (mode != Engine::Components::SpatialManipulator::
                    ConnectionMode::Portal &&
                mode != Engine::Components::SpatialManipulator::
                    ConnectionMode::LinkedPortal)
                return false;
            const auto* target = resolvePortalTarget(portal);
            return target && target->enabled && target->Owner &&
                portal->HasCompatiblePortalShapeWith(*target);
        });
    // A terrain cell covers its rectangular footprint. Bound the entry and
    // exit of each sightline through that surface; a draw is hidden only when
    // every sightline crosses a nearer cell. Rebuild for each camera position.
    using TerrainFootprint = Engine::Scene::Renderer::TerrainFootprint;
    std::vector<TerrainFootprint> terrainBlockers;
    std::vector<TerrainFootprint> solidBlockers;
    const bool terrainWarpActive = std::any_of(
        m_frameSpatialManipulatorScratch.begin(),
        m_frameSpatialManipulatorScratch.end(),
        [](const Engine::Components::SpatialManipulator* manipulator)
        {
            return manipulator && manipulator->enabled &&
                manipulator->definesWarpVolume && manipulator->Owner &&
                manipulator->Owner->IsEnabledInHierarchy();
        });
    const bool useTerrainOcclusion = !shadowOnly && !includeEditorVisuals &&
        !hasPortalViews && !m_editorMode2D && !terrainWarpActive;
    const bool terrainOcclusionEnabled = useTerrainOcclusion &&
        ReadEnvFlag("ENGINE_VISIBILITY_TERRAIN_OCCLUSION", true);
    const bool solidOcclusionEnabled = useTerrainOcclusion &&
        ReadEnvFlag("ENGINE_VISIBILITY_SOLID_OCCLUSION", false);
    auto& viewVisibility = shadowOnly
        ? m_lastRenderCpuTimings.shadowVisibility
        : m_lastRenderCpuTimings.cameraVisibility;
    viewVisibility.viewIdentity=diagnosticViewIdentity;
    viewVisibility.viewIdentities.push_back({diagnosticViewIdentity,false,shadowOnly});
    ++viewVisibility.views;
    viewVisibility.frameItems += frameRenderItems.size();
    Engine::Scene::Renderer::SceneRendererVisibility::PrepareTerrainOcclusion(
        *this,
        frameRenderItems,
        cameraPosition,
        shadowOnly,
        includeEditorVisuals,
        hasPortalViews,
        m_editorMode2D,
        terrainWarpActive,
        terrainBlockers,
        solidBlockers);
    using VisibleSlotsQuery = Engine::Rendering::RenderSpatialIndex::VisibleSlotsQuery;
    std::future<VisibleSlotsQuery> cameraVisibilityJob;
    struct CameraVisibilityFence
    {
        std::future<VisibleSlotsQuery>& job;
        ~CameraVisibilityFence() { if (job.valid()) job.wait(); }
    } cameraVisibilityFence{cameraVisibilityJob};
    if (terrainOcclusionEnabled || solidOcclusionEnabled)
        cameraVisibilityJob = Engine::Rendering::RenderJobs::Get().Submit(
            [this, viewProjection] {
                return m_renderSpatialIndex.QueryVisibleSlotsUncached(viewProjection);
            });
    const auto terrainPreparationStart=RenderClock::now();
    m_lastRenderCpuTimings.startupMs=std::chrono::duration<double,std::milli>(terrainPreparationStart-measurement.start).count();
    m_lastRenderCpuTimings.terrainPreparationMs=std::chrono::duration<double,std::milli>(RenderClock::now()-terrainPreparationStart).count();
    const auto cullingStart = RenderClock::now();
    const bool useSpatialCandidates = !shadowOnly && !hasPortalViews &&
        !m_editorMode2D;
    std::vector<uint32_t> visibleRenderSlots;
    Engine::Rendering::RenderSpatialQueryStatistics spatialQueryStatistics;
    const bool hasPreparedCameraVisibility = cameraVisibilityJob.valid();
    if (hasPreparedCameraVisibility)
    {
        auto query = cameraVisibilityJob.get();
        visibleRenderSlots = std::move(query.slots);
        spatialQueryStatistics = query.statistics;
    }
    else if (useSpatialCandidates)
        visibleRenderSlots = m_renderSpatialIndex.QueryVisibleSlots(viewProjection,
            &spatialQueryStatistics);
    std::vector<uint32_t> candidateFrameItems;
    if (useSpatialCandidates)
    {
        size_t candidateCount = 0;
        for (const uint32_t slot : visibleRenderSlots)
            if (size_t(slot) + 1u < frameItemSlotOffsets.size())
                candidateCount += frameItemSlotOffsets[size_t(slot) + 1u] -
                    frameItemSlotOffsets[slot];
        candidateFrameItems.reserve(candidateCount);
        for (const uint32_t slot : visibleRenderSlots)
        {
            if (size_t(slot) + 1u >= frameItemSlotOffsets.size()) continue;
            const auto first = frameItemSlotOffsets[slot];
            const auto last = frameItemSlotOffsets[size_t(slot) + 1u];
            candidateFrameItems.insert(candidateFrameItems.end(),
                frameItemSlotIndices.begin() + first,
                frameItemSlotIndices.begin() + last);
        }
    }
    m_lastRenderCpuTimings.visibilityCandidates += useSpatialCandidates
        ? candidateFrameItems.size() : frameRenderItems.size();
    viewVisibility.candidateItems += useSpatialCandidates
        ? candidateFrameItems.size() : frameRenderItems.size();
    viewVisibility.visibleSlots += spatialQueryStatistics.visibleSlots;
    viewVisibility.bvhNodesVisited += spatialQueryStatistics.nodesVisited;
    viewVisibility.bvhBoundsTested += spatialQueryStatistics.boundsTested;
    const auto hiddenByTerrain = [&](const FrameRenderItem& item)
    {
        if (!terrainOcclusionEnabled || !item.terrainSurface)
            return false;
        return Engine::Scene::Renderer::SceneRendererVisibility::HiddenByTerrain(
            item, cameraPosition, terrainBlockers);
    };
    const auto hiddenBySolid = [&](const FrameRenderItem& item)
    {
        if (!solidOcclusionEnabled || !item.mesh || item.blended ||
            item.sprite || item.warpedVertexBuffer ||
            item.traversalChartPortal || !item.hasBounds)
            return false;
        return Engine::Scene::Renderer::SceneRendererVisibility::HiddenBySolid(
            item, cameraPosition, solidBlockers);
    };
    auto visibilityState = Engine::Scene::Renderer::SceneRendererViewPreparation::PrepareVisibleSet(
        *this,
        frameRenderItems,
        candidateFrameItems,
        useSpatialCandidates,
        cameraPosition,
        shadowOnly,
        includeEditorVisuals,
        hasPortalViews,
        m_editorMode2D,
        cameraUsesSourceWarpChart,
        hiddenByTerrain,
        hiddenBySolid,
        [viewProjection](const FrameRenderItem& item, const glm::mat4& itemWorld)
        {
            return Engine::Scene::Renderer::SceneRendererViewPreparation::IsOutsideViewFrustum(
                item, itemWorld, viewProjection);
        },
        [cameraPosition](const FrameRenderItem& item, const glm::mat4& itemWorld)
        {
            return Engine::Scene::Renderer::SceneRendererViewPreparation::ComputeNearestLightingDistanceSquared(
                item, itemWorld, cameraPosition);
        },
        useSourceChartMesh,
        sourceChartWorld);
    auto& renderObjects = m_viewRenderItemScratch;
    renderObjects = std::move(visibilityState.renderObjects);
    m_lastRenderCpuTimings.visibilityAccepted += renderObjects.size();
    viewVisibility.acceptedItems += renderObjects.size();
    m_lastTerrainOcclusionCulledCount += visibilityState.terrainOcclusionCulled;
    m_lastRenderCpuTimings.terrainOcclusionRejected +=
        visibilityState.terrainOcclusionCulled;
    m_lastRenderCpuTimings.solidOcclusionRejected +=
        visibilityState.solidOcclusionCulled;
    const auto sortingStart = RenderClock::now();
    m_lastRenderCpuTimings.cullingMs = std::chrono::duration<double,std::milli>(sortingStart-cullingStart).count();
    std::future<void> sortJob;
    if (renderObjects.size() >= 128)
        sortJob = Engine::Rendering::RenderJobs::Get().Submit(
            [&renderObjects, editorMode2D = m_editorMode2D]()
            {
                Engine::Scene::Renderer::SceneRendererViewPreparation::SortViewItems(
                    renderObjects, editorMode2D);
            });
    else
        Engine::Scene::Renderer::SceneRendererViewPreparation::SortViewItems(
            renderObjects, m_editorMode2D);
    bool hasSceneLightProbeContributors = false;
    for (auto* group : GetComponents<Engine::Components::LightProbeGroup>())
        hasSceneLightProbeContributors |= group->valid && group->Owner->IsEnabledInHierarchy();
    for (auto* probe : GetComponents<Engine::Components::LightProbe>())
        hasSceneLightProbeContributors |= probe->valid && probe->Owner->IsEnabledInHierarchy();
    if (sortJob.valid()) sortJob.get();

    m_lastRenderCpuTimings.sortingMs = std::chrono::duration<double,std::milli>(RenderClock::now()-sortingStart).count();
    const auto occlusionStart=RenderClock::now();
    // Render phase 5: prepare per-object draws and their material records.
    // Occlusion results are deliberately tied to the exact camera and scene
    // transforms that produced them. Any movement restores all objects to the
    // visible state; the backend consumes only completed GPU query results and
    // never stalls this frame waiting for an answer.
    const uint64_t occlusionSignature =
        Engine::Scene::Renderer::SceneRendererViewPreparation::BuildOcclusionSignature(
            renderObjects, viewProjection, useSourceChartMesh, sourceChartWorld);
    const uint64_t occlusionViewId =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this)) ^
        (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cam)) << 1u);
    const bool occlusionQueriesEnabled = context->BeginOcclusionFrame(
        occlusionViewId, occlusionSignature);

    m_lastRenderCpuTimings.occlusionMs=std::chrono::duration<double,std::milli>(RenderClock::now()-occlusionStart).count();
    // Shadow maps need less geometry than the camera color pass. Keep the
    // per-placement culling bands and pixel dither bands on the same policy,
    // including independently authored instance LOD bands.
    const auto scaledShadowLodDistances = [&](const glm::vec2 distances)
    {
        return Renderer::SceneInstanceBatching::ScaleShadowLodDistances(distances, shadowOnly);
    };
    // Draw preparation has been decomposed into the SceneRenderer subsystem.
    // Keep the alias here while the render loop is being gradually migrated.
    using PreparedDraw = Engine::Scene::Renderer::PreparedDraw;
    std::vector<PreparedDraw> preparedDraws;
    m_diagnosticOpaqueDepthEqualReady = true;
    preparedDraws.reserve(renderObjects.size());
    // All streamed grass tiles share the same foliage asset and a small set
    // of base pipeline variants. Resolve each variant once per view.
    std::map<std::pair<IPipelineState*, std::string>,
        std::pair<IPipelineState*, std::string>> foliagePipelines;
    Renderer::TerrainMaterialState terrainMaterialState;

    const auto materialStart = RenderClock::now();
    // Scene texture policy is identical for every draw in this view. Resolve
    // it once; only materials with an explicit override need per-draw work.
    const auto sceneTextureLod = settings.graphicsQuality.ResolveTexture(settings.textureLod);
    const auto sceneAlphaLodParameters = sceneTextureLod.AlphaGpuParameters();
    const auto sceneTextureLodParameters = sceneTextureLod.GpuParameters();
    const auto applyTextureLod = [&](ObjectGPUData& data,
        const Engine::Components::Material* material)
    {
        if (material && material->overrideTextureLod)
        {
            const auto textureLod = settings.graphicsQuality.ResolveTexture(material->textureLod);
            data.alphaLodParams = textureLod.AlphaGpuParameters();
            data.textureLodParams = textureLod.GpuParameters();
        }
        else
        {
            data.alphaLodParams = sceneAlphaLodParameters;
            data.textureLodParams = sceneTextureLodParameters;
        }
    };
    double materialPipelineMs = 0.0;
    double materialGeometryBindingMs = 0.0;
    UINT slot = 0;
    for (const ViewRenderItem& sortedItem : renderObjects)
    {
        // Reject candidates that cannot contribute to this camera or shadow view.
        const FrameRenderItem* renderItem = sortedItem.source;
        slot=renderItem->objectSlot;
        Engine::Core::Object* obj = renderItem->object;
        Engine::Components::Mesh* mesh = renderItem->mesh;
        const bool fixedShadowBand = shadowOnly && mesh &&
            mesh->fixedShadowLodBand>=0;
        if (mesh)
        {
            if (!shadowOnly && mesh->shadowProxyOnly) continue;
            if (shadowOnly && mesh->dedicatedShadowProxy &&
                !mesh->shadowProxyOnly) continue;
            if(fixedShadowBand && mesh->instanceLodBand>=0 &&
                mesh->instanceLodBand!=mesh->fixedShadowLodBand) continue;
        }
        if (!shadowOnly && mesh && mesh->HasBounds() && (quality.viewDistance < 1.f || quality.vegetation < 1.f)) {
            const auto bounds = Engine::Rendering::TransformBounds(mesh->GetBoundsMin(), mesh->GetBoundsMax(), renderItem->world);
            const auto delta = glm::clamp(cameraPosition, bounds.minimum, bounds.maximum) - cameraPosition;
            const bool deforming = mesh->GetDeformationBoundsPadding() > 0.f;
            const float range = cam->farPlane * quality.viewDistance * (deforming ? quality.vegetation : 1.f);
            if (glm::dot(delta, delta) > range * range) continue;
        }
        Engine::Components::Sprite* sprite = renderItem->sprite;
        Engine::Components::Material* mat = renderItem->material;
        const Engine::Rendering::BakedLightingData* bakedLighting =
            renderItem->bakedLighting;
        const bool belongsToPreview = renderItem->belongsToPreview;
        const bool isPreview = belongsToPreview;
        // Resolve one compact lighting policy for material, shadow, and size gates.
        const bool bakedStaticReceiver = bakedLighting && bakedLighting->valid;
        const auto lightingPolicy =
            Renderer::SceneRendererDrawPreparation::ResolveLightingPolicy(
                m_distanceLightingSettings,
                sortedItem.lightingDistanceSquared, bakedStaticReceiver);
        const auto* lightingBand = lightingPolicy.band;
        const bool beyondLightingDistance = lightingPolicy.beyondMaximumDistance;
        const bool allowNormalMapping = lightingPolicy.allowNormalMapping;
        const bool allowParallaxMapping = lightingPolicy.allowParallaxMapping;
        const bool allowSubsurfaceScattering =
            lightingPolicy.allowSubsurfaceScattering;
        const bool allowEnvironmentDiffuse = lightingPolicy.allowEnvironmentDiffuse;
        const bool allowReflections = lightingPolicy.allowReflections;
        const bool allowRealtimeShadows = lightingPolicy.allowRealtimeShadows;
        if (Renderer::SceneRendererDrawPreparation::IsBelowProjectedSizeThreshold(
            sortedItem, mesh, sprite != nullptr, belongsToPreview, shadowOnly,
            cam, viewportHeight, lightingPolicy.minimumProjectedPixels))
            continue;

        // A valid bake owns receiver shading, but the object can still cast
        // real-time shadows onto dynamic receivers.
        if (bakedStaticReceiver)
            ++m_shadowDebugSnapshot.bakedReceiverCount;
        else
            ++m_shadowDebugSnapshot.dynamicReceiverCount;        PreparedDraw preparedDraw{};
        // Carry frame-item and shadow policy into a backend-ready draw record.
        preparedDraw.morphDeltaBuffer = m_emptyMorphDeltaBuffer.get();
        preparedDraw.morphWeightBuffer = m_emptyMorphWeightBuffer.get();
        preparedDraw.textures[8] = m_brdfIntegrationLut.get();
        preparedDraw.frameItemIndex=static_cast<uint32_t>(renderItem-frameRenderItems.data());
        preparedDraw.gpuLod=renderItem->gpuLod;
        preparedDraw.gpuMaximumLod=renderItem->gpuMaximumLod;
        preparedDraw.gpuLodDistances=renderItem->gpuLodDistances;
        if (mesh && mesh->instanceLodBand>=0)
        {
            preparedDraw.gpuLod=mesh->instanceLodBand;
            preparedDraw.gpuMaximumLod=2;
            preparedDraw.gpuLodDistances=mesh->instanceLodDistances;
            if(fixedShadowBand) preparedDraw.gpuLod=-1;
        }
        preparedDraw.gpuLodDistances *= quality.geometry;
        preparedDraw.object = obj;
        preparedDraw.mesh = mesh;
        preparedDraw.material = sprite ? nullptr : mat;
        if (mesh)
        {
            const auto* source = mesh->GetInstanceSource() ? mesh->GetInstanceSource() : mesh;
            const auto& path = source->GetFilePath();
            // Explicit scope of the bundled forest audit; wind may be disabled.
            preparedDraw.treeGeometry = mesh->treeTimingGeometry || path.find("/Woodland/Oaks/") != std::string::npos ||
                path.find("\\Woodland\\Oaks\\") != std::string::npos;
        }
        if (!shadowOnly && mat) mat->shaderError.clear();
        preparedDraw.spatialManipulator = renderItem->spatialManipulator;
        preparedDraw.preview = isPreview;
        preparedDraw.terrain = mesh && mesh->UsesTerrainVertexFormat();
        preparedDraw.terrainCell = mesh &&
            (obj->GetComponent<Engine::Components::Terrain>() != nullptr ||
                obj->GetComponent<Engine::Components::TerrainBatchAppearance>() != nullptr);
        preparedDraw.blended = sortedItem.blended;
        preparedDraw.pmxOutline = mat && mat->edgeEnabled &&
            mat->edgeWidth > 0.f && mat->edgeColor.a > 0.f &&
            !preparedDraw.terrain && !isPreview;
        preparedDraw.castsShadow = shadowSelection.valid && mesh && !sprite &&
            !sortedItem.blended && !belongsToPreview &&
            (!mat || mat->castsShadows);
        glm::vec2 casterFade=renderItem->shadowCasterFade * quality.shadows;
        if(mesh && mesh->usesGrassShadowBudget)
        {
            const float budget=std::max(0.f,m_realtimeShadowSettings.grassCasterDistance);
            if(budget<=0.f) preparedDraw.castsShadow=false;
            else if(casterFade.x<=0.f || budget<casterFade.x) casterFade.x=budget;
        }
        else if(mesh && mesh->foliageWindStrength>0.f)
        {
            const float budget=std::max(0.f,m_realtimeShadowSettings.foliageCasterDistance);
            if(budget<=0.f) preparedDraw.castsShadow=false;
            else if(casterFade.x<=0.f || budget<casterFade.x) casterFade.x=budget;
        }
        if(preparedDraw.castsShadow && casterFade.x>0.f && mesh->HasBounds())
        {
            const auto bounds=Engine::Rendering::TransformBounds(mesh->GetBoundsMin(),mesh->GetBoundsMax(),renderItem->world);
            const auto delta=glm::clamp(cameraPosition,bounds.minimum,bounds.maximum)-cameraPosition;
            preparedDraw.castsShadow=glm::dot(delta,delta)<=casterFade.x*casterFade.x;
        }
        if (shadowOnly)
        {
            // Shadow views need alpha, geometry, and caster state only. Finish
            // their compact record here instead of resolving camera materials.
            if (!preparedDraw.castsShadow)
                continue;
            const bool useSourceChart = useSourceChartMesh(*renderItem);
            const glm::mat4 world = useSourceChart
                ? sourceChartWorld(*renderItem) : renderItem->world;
            const bool useSoftSurface = !m_editorMeshEditPose &&
                mesh->GetPosedVertexBuffer() &&
                !mesh->IsPortalPoseSnapshot() &&
                (useSourceChart || !renderItem->warpedVertexBuffer);
            preparedDraw.objectData.geometryFade=mesh->instanceLodBand>=0 ? glm::vec4(float(mesh->instanceLodBand),scaledShadowLodDistances(mesh->instanceLodDistances),4.f) : (hasPortalViews && renderItem->geometryFade.w>=2.f ? mesh->GetRuntimeGeometryFade():renderItem->geometryFade);
            preparedDraw.objectData.shadowCasterFade={casterFade.x,std::min(casterFade.x,casterFade.y),1.f,0.f};
            preparedDraw.objectData.grassAnimation={0.f,0.f,0.f,0.f};
            preparedDraw.objectData.viewPositionAlphaCutoff=glm::vec4(cameraPosition,.001f);
            applyTextureLod(preparedDraw.objectData, mat);
            if(fixedShadowBand)
            {
                // Camera distance must not reshape a fixed shadow proxy, via
                // either geometry dithering or distance-biased leaf cutouts.
                preparedDraw.objectData.geometryFade=glm::vec4(0.f);
                preparedDraw.objectData.textureLodParams=glm::vec4(0.f);
                preparedDraw.objectData.alphaLodParams=glm::vec4(0.f);
            }
            if (mat && mat->GetAlphaMode() ==
                Engine::Components::MaterialAlphaMode::Mask)
                preparedDraw.objectData.alphaLodParams.z =
                    std::clamp(mat->alphaCoverageTransmission, 0.f, 1.f);
            preparedDraw.objectData.world = world;
            preparedDraw.objectData.viewProjection = viewProjection;
            preparedDraw.objectData.traversalClipPlane =
                renderItem->traversalClipPlane;
            preparedDraw.objectData.spriteUvRect = { 0.f, 0.f, 1.f, 1.f };
            if (renderItem->skinJointCount > 0 && !useSoftSurface)
                preparedDraw.objectData.skinParams = {
                    static_cast<float>(renderItem->skinPaletteOffset),
                    static_cast<float>(renderItem->skinJointCount), 0.f, 0.f };
            if (!useSoftSurface && !m_editorMeshEditPose &&
                !mesh->IsPortalPoseSnapshot() && mesh->HasMorphTargets() &&
                mesh->GetMorphDeltaBuffer() && mesh->GetMorphWeightBuffer() &&
                (useSourceChart || !renderItem->warpedVertexBuffer))
            {
                preparedDraw.morphDeltaBuffer = mesh->GetMorphDeltaBuffer();
                preparedDraw.morphWeightBuffer = mesh->GetMorphWeightBuffer();
                preparedDraw.objectData.morphParams = {
                    static_cast<float>(mesh->GetActiveMorphTargetCount()),
                    static_cast<float>(mesh->GetVertexCount()), 0.f,
                    static_cast<float>(mesh->GetMorphDeltaStrideFloat4()) };
            }
            if (mat && mat->GetAlphaMode() ==
                Engine::Components::MaterialAlphaMode::Mask &&
                !preparedDraw.terrainCell)
            {
                uint32_t flags = 32u;
                if (!mat->useVertexColors) flags |= 4096u;
                preparedDraw.textures[0] = mat->baseColorTexture
                    ? mat->baseColorTexture->GetGraphicsTexture() : nullptr;
                if (preparedDraw.textures[0]) flags |= 1u;
                preparedDraw.objectData.materialParams.w =
                    static_cast<float>(flags);
                preparedDraw.objectData.baseColor.a = mat->baseColorAlpha;
                preparedDraw.objectData.pmxTextureTint.a =
                    mat->baseColorTextureTintAlpha;
                preparedDraw.objectData.textureUvSets0.x =
                    static_cast<float>(mat->baseColorUvSet);
                preparedDraw.objectData.viewPositionAlphaCutoff.w =
                    mat->alphaCutoff;
            }
            preparedDraw.doubleSided = !preparedDraw.terrainCell &&
                (mat && mat->doubleSided);
            // Independent prefab cards carry their authored caster sidedness.
            if (mesh->instanceLodBand>=0)
                preparedDraw.shadowDoubleSided=preparedDraw.doubleSided;
            preparedDraw.drawData = { slot, 0u, mesh->IsInstanced() ? (0x20000000u | (renderItem->geometryFade.w>=2.f && !hasPortalViews ? 0x00100000u:0u)):0u, 0u };
            preparedDraw.traversalChartPortal =
                renderItem->traversalChartPortal;
            preparedDraw.vertexBuffer = !useSourceChart &&
                renderItem->warpedVertexBuffer
                ? renderItem->warpedVertexBuffer
                : (useSoftSurface ? mesh->GetPosedVertexBuffer()
                    : mesh->GetGraphicsBuffer());
            preparedDraw.vertexStride = mesh->GetVertexStride();
            preparedDraw.vertexCount = mesh->GetVertexCount();
            preparedDraw.shadowMesh = mesh;
            if (!mesh->IsInstanced() && preparedDraw.gpuLod > 0 &&
                mesh->gpuLodSelection)
            {
                if (auto* runtimeShadowMesh =
                    mesh->GetRuntimeLodMesh(preparedDraw.gpuLod))
                    preparedDraw.shadowMesh = runtimeShadowMesh;
            }
            preparedDraw.shadowVertexBuffer = preparedDraw.shadowMesh->GetGraphicsBuffer();
            preparedDraw.shadowVertexStride = preparedDraw.shadowMesh->GetVertexStride();
            preparedDraw.shadowVertexCount = preparedDraw.shadowMesh->GetVertexCount();
            if (!useSourceChart && !renderItem->warpedVertexBuffer &&
                mesh->GetIndexBuffer())
            {
                preparedDraw.indexBuffer = mesh->GetIndexBuffer();
                preparedDraw.indexCount = mesh->GetIndexCount();
            }
            if (!useSourceChart && !renderItem->warpedVertexBuffer &&
                preparedDraw.shadowMesh->GetIndexBuffer())
            {
                preparedDraw.shadowIndexBuffer = preparedDraw.shadowMesh->GetIndexBuffer();
                preparedDraw.shadowIndexCount = preparedDraw.shadowMesh->GetIndexCount();
            }
            preparedDraws.push_back(std::move(preparedDraw));
            ++slot;
            continue;
        }
        preparedDraw.occlusionCandidate = occlusionQueriesEnabled &&
            !wireframeMode &&
            !sortedItem.blended && !sprite &&
            renderItem->geometryFade.w < .5f &&
            !renderItem->warpedVertexBuffer &&
            !renderItem->traversalChartPortal;
        preparedDraw.occlusionId = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(obj));
        // Version 3 and later bake lighting into generated material assets.
        // Keep the component values for inspection, but do not add them again
        // at runtime or the baked result would be double-lit.
        const bool usesLegacyProbeBake = bakedLighting && bakedLighting->valid &&
            bakedLighting->version < 3 && !(mesh&&mesh->useLightProbes);
        const bool usesInterpolatedProbes=mesh&&mesh->useLightProbes;
        const bool evaluateProbeLighting = hasSceneLightProbeContributors &&
            (usesInterpolatedProbes||!bakedLighting||!bakedLighting->valid);
        const auto resolveProbeLighting = [&]()
        {
            AssertOwnerThread();
            return evaluateProbeLighting
                ? Engine::Components::SampleLightProbes(*this,
                    glm::vec3(renderItem->world[3]))
                : Engine::Components::ProbeLightingSample{};
        };
        const Engine::Components::ProbeLightingSample probeLighting = resolveProbeLighting();
        const glm::vec3 bakedIrradiance =
            usesLegacyProbeBake
                ? bakedLighting->irradiance
                : (probeLighting.valid
                    ? probeLighting.irradiance : glm::vec3(0.f));
        const bool useSourceChart = useSourceChartMesh(*renderItem);
        glm::mat4 world = useSourceChart
            ? sourceChartWorld(*renderItem) : renderItem->world;
        if (sprite)
        {
            if (m_editorMode2D)
                world[3].z = 0.f;
            world = world * glm::scale(glm::mat4(1.f),
                glm::vec3(renderItem->spriteWorldSize, 1.f));
        }
        UINT64 offset = static_cast<UINT64>(slot) * kCBStride;

        ObjectGPUData objectData{};
        // Resolve probe/environment inputs before applying per-object materials.
        objectData.lightingChannels.x = mat
            ? static_cast<uint32_t>(mat->lightingChannels) : 0xffffffffu;
        objectData.lightingClusterParams = glm::vec4(
            static_cast<float>(clusterTileSize),
            static_cast<float>(clusterTilesX),
            static_cast<float>(clusterTilesY),
            static_cast<float>(clusterDepthSlices));
        objectData.lightingCameraData = glm::vec4(cameraDirection, clusterLogScale);
        objectData.shadowCameraData.w = clusterNear;
        if (settings.hdriLightingEnabled && skybox &&
            (allowEnvironmentDiffuse || allowReflections))
        {
            objectData.environmentParams = {
                std::max(0.f, settings.hdriIntensity) *
                    std::exp2(std::clamp(settings.hdriExposure, -16.f, 16.f)),
                glm::radians(settings.hdriRotation), 1.f, 1.f };
            std::copy(m_environmentSH.begin(), m_environmentSH.end(),
                objectData.environmentSH);
            preparedDraw.textures[6] = m_environmentMap
                ? m_environmentMap->GetSpecularTexture() : nullptr;
            if (preparedDraw.textures[6])
                objectData.reflectionEnvironmentParams.w = 1.f;
        }
        objectData.viewProjection = viewProjection;
        objectData.sphereView = view;
        objectData.world = world;
        objectData.traversalClipPlane = renderItem->traversalClipPlane;
        objectData.spriteUvRect = { 0.f, 0.f, 1.f, 1.f };
        const bool useSoftSurface = !m_editorMeshEditPose && mesh &&
            mesh->GetPosedVertexBuffer() &&
            !mesh->IsPortalPoseSnapshot() &&
            (useSourceChart || !renderItem->warpedVertexBuffer);
        if (renderItem->skinJointCount > 0 && !useSoftSurface)
        {
            objectData.skinParams = {
                static_cast<float>(renderItem->skinPaletteOffset),
                static_cast<float>(renderItem->skinJointCount), 0.f, 0.f };
        }
        const bool useGpuMorphs = !useSoftSurface && !m_editorMeshEditPose && mesh && !sprite &&
            !mesh->IsPortalPoseSnapshot() && mesh->HasMorphTargets() &&
            mesh->GetMorphDeltaBuffer() && mesh->GetMorphWeightBuffer() &&
            (useSourceChart || !renderItem->warpedVertexBuffer);
        if (useGpuMorphs)
        {
            preparedDraw.morphDeltaBuffer = mesh->GetMorphDeltaBuffer();
            preparedDraw.morphWeightBuffer = mesh->GetMorphWeightBuffer();
            objectData.morphParams = {
                static_cast<float>(mesh->GetActiveMorphTargetCount()),
                static_cast<float>(mesh->GetVertexCount()), 0.f,
                static_cast<float>(mesh->GetMorphDeltaStrideFloat4()) };
        }
        Engine::Components::MaterialAlphaMode alphaMode = Engine::Components::MaterialAlphaMode::Opaque;
        bool doubleSided = false;

        if (sprite)
        {
            const Engine::Components::Texture* texture = renderItem->spriteTexture;
            const Engine::Graphics::IGraphicsTexture* graphicsTexture = texture
                ? texture->GetGraphicsTexture() : nullptr;
            preparedDraw.textures[0] = graphicsTexture;
            objectData.baseColor = glm::vec4(sprite->tint,
                std::clamp(sprite->alpha, 0.f, 1.f));
            objectData.ambientUnlit = { 0.f, 0.f, 0.f, 1.f };
            objectData.emissiveOcclusion = { 0.f, 0.f, 0.f, 1.f };
            // Sprites use the base-colour texture and alpha testing in addition
            // to blending. Discarding empty atlas pixels prevents their black
            // RGB values from ever reaching the render target, while the low
            // cutoff preserves anti-aliased translucent edge pixels.
            constexpr uint32_t kBaseColorTextureFlag = 1u;
            constexpr uint32_t kAlphaMaskFlag = 32u;
            const uint32_t spriteTextureFlags = graphicsTexture
                ? kBaseColorTextureFlag | kAlphaMaskFlag
                : kAlphaMaskFlag;
            objectData.materialParams = { 0.f, 1.f, 1.f,
                static_cast<float>(spriteTextureFlags) };
            objectData.viewPositionAlphaCutoff = glm::vec4(cameraPosition, 0.01f);
            objectData.spriteUvRect = renderItem->spriteUvRect;
            alphaMode = Engine::Components::MaterialAlphaMode::Blend;
            doubleSided = true;
        }
        else if (mat)
        {
            // Material values and texture bindings are cached by revision and
            // lighting feature variant in SceneRendererDrawPreparation.
            const auto resolvedMaterial =
                Renderer::SceneRendererDrawPreparation::ResolveMaterialRecord(
                    *this, *mat, objectData, preparedDraw.textures,
                    cameraPosition, allowNormalMapping, allowParallaxMapping,
                    allowSubsurfaceScattering, allowEnvironmentDiffuse,
                    allowReflections);
            Renderer::SceneRendererDrawPreparation::ApplyMaterialRecord(
                resolvedMaterial, cameraPosition, bakedIrradiance, objectData);
            preparedDraw.textures = resolvedMaterial.textures;
            doubleSided = resolvedMaterial.doubleSided;
            alphaMode = resolvedMaterial.alphaMode;
        }
        else
        {
            objectData.baseColor = { 0.8f, 0.8f, 0.8f, 1.f };
            objectData.ambientUnlit = glm::vec4(
                settings.ambientColor + bakedIrradiance, 0.f);
            objectData.emissiveOcclusion = { 0.f, 0.f, 0.f, 1.f };
            objectData.materialParams = { 0.f, 1.f, 1.f, 0.f };
            objectData.viewPositionAlphaCutoff = glm::vec4(
                cameraPosition, 0.5f);
        }
        Renderer::SceneRendererDrawPreparation::PrepareTerrainMaterial(
            *this, *renderItem, preparedDraw, objectData, includeEditorVisuals,
            forceUnlitMode, terrainMaterialState);
        if (includeEditorVisuals && mesh == m_editorWeightPaintMesh &&
            m_editorWeightPaintBone >= 0)
        {
            // The visualization is transient GPU state. Authored materials,
            // textures, vertex colors, and mesh data remain untouched.
            preparedDraw.textures.fill(nullptr);
            objectData.baseColor = { 0.65f, 0.65f, 0.65f, 1.f };
            objectData.ambientUnlit = { 0.f, 0.f, 0.f, 1.f };
            objectData.emissiveOcclusion = { 0.f, 0.f, 0.f, 1.f };
            objectData.materialParams = { 0.f, 1.f, 1.f, 0.f };
            objectData.morphParams.z = static_cast<float>(m_editorWeightPaintBone + 1);
            objectData.environmentParams.z = 0.f;
            objectData.environmentParams.w = 0.f;
            alphaMode = Engine::Components::MaterialAlphaMode::Opaque;
        }
        // Resolve after cached material assembly: scene settings must also affect
        // static objects and material-less meshes without rebuilding their assets.
        applyTextureLod(objectData, mat);
        if (alphaMode == Engine::Components::MaterialAlphaMode::Mask && mat)
            objectData.alphaLodParams.z =
                std::clamp(mat->alphaCoverageTransmission, 0.f, 1.f);
        bool terrainShadowDoubleSided = doubleSided;
        // Height fields have a single upward-facing surface, including cached
        // terrain and streamed batches. Backfaces do not contribute color or shadows.
        if (preparedDraw.terrainCell)
            doubleSided = terrainShadowDoubleSided = false;
        preparedDraw.doubleSided = doubleSided;
        preparedDraw.shadowDoubleSided = terrainShadowDoubleSided;
        if (shadowSelection.valid)
        {
            std::copy(shadowViewProjections.begin(),
                shadowViewProjections.end(), objectData.shadowViewProjections);
            objectData.shadowParams = {
                shadowSelection.depthBias,
                shadowSelection.normalBias,
                allowRealtimeShadows ? 1.f : 0.f,
                static_cast<float>(std::min(
                    std::min(m_realtimeShadowSettings.pcfRadius,
                        lightingBand
                            ? lightingBand->maximumShadowPcfRadius : 4u),
                    static_cast<uint32_t>(std::round(4.f * quality.shadows *
                        shadowSelection.filterScale)))) };
            objectData.shadowCascadeSplits = shadowCascadeSplits;
            objectData.shadowCascadeData = mainShadowCascades.data;
            // Terrain patches are subdivisions of one continuous surface.
            // Applying an object-level lighting band to each patch creates
            // chunk-shaped shadow cutoffs even though the cascade already
            // performs a smooth per-pixel distance fade.
            objectData.shadowCascadeData.w *= preparedDraw.terrain
                ? 1.f
                : (lightingBand
                    ? std::clamp(lightingBand->shadowDistanceScale, 0.f, 1.f)
                    : 1.f);
            objectData.shadowCameraData = mainShadowCascades.cameraData;
            preparedDraw.textures[7] = m_directionalShadowMap.get();
        }
        if (usesLegacyProbeBake)
        {
            objectData.bakedDirectional = glm::vec4(
                bakedLighting->directionalIrradiance, 0.f);
            objectData.bakedLightDirection = glm::vec4(bakedLighting->lightDirection, 1.f);
        }
        else if (probeLighting.valid)
        {
            objectData.bakedDirectional = glm::vec4(probeLighting.directionalIrradiance, 0.f);
            objectData.bakedLightDirection = glm::vec4(probeLighting.direction, 1.f);
        }
        if (isPreview)
            objectData.baseColor.a *= 0.45f;
        if (forceUnlitMode)
            objectData.ambientUnlit.w = 1.f;

        // Static terrain's inexpensive directional pass must keep the sun
        // through the detailed-to-far-field join. The generic lighting
        // distance cap otherwise leaves an ambient-only dark ring before the
        // unlit distant terrain starts.
        const bool fastTerrainLighting =
            (static_cast<uint32_t>(objectData.materialParams.w) & 8192u) != 0u &&
            objectData.ambientUnlit.w < 0.5f;
        const bool farTerrainVertexLighting =
            (static_cast<uint32_t>(objectData.materialParams.w) & 16384u) != 0u;
        if (fastTerrainLighting || farTerrainVertexLighting)
        {
            objectData.bakedDirectional = glm::vec4(terrainSunRadiance,
                fastTerrainLighting && shadowSelection.valid && terrainSunIndex ==
                    static_cast<int>(shadowSelection.lightIndex)
                    ? static_cast<float>(terrainSunIndex + 1) : 0.f);
            objectData.bakedLightDirection = glm::vec4(terrainSunDirection, 0.f);
        }
        const uint32_t qualityLightCount = fastTerrainLighting
            ? 0u : beyondLightingDistance ? 0u :
            (lightingBand
                ? std::min(m_frameLightCount, lightingBand->maxRealtimeLights)
                : m_frameLightCount);
        const DrawCBData drawData{ slot,
            forceUnlitMode ? 0u : qualityLightCount,
            (clusteredLightingEnabled ? kClusteredLightingFlag : 0u) |
                (mesh && mesh->IsInstanced() ? (0x20000000u | (renderItem->geometryFade.w>=2.f && !hasPortalViews ? 0x00100000u:0u)):0u), 0u };
        preparedDraw.drawData = drawData;
        objectData.geometryFade = mesh && mesh->instanceLodBand>=0 ? glm::vec4(float(mesh->instanceLodBand),mesh->instanceLodDistances.x,mesh->instanceLodDistances.y,4.f) : (hasPortalViews && renderItem->geometryFade.w>=2.f ? mesh->GetRuntimeGeometryFade():renderItem->geometryFade);
        if (objectData.geometryFade.w >= 2.f) {
            objectData.geometryFade.y *= quality.geometry;
            objectData.geometryFade.z *= quality.geometry;
        }
        objectData.shadowCasterFade={casterFade.x,std::min(casterFade.x,casterFade.y),0.f,0.f};
        objectData.grassAnimation={mesh && mesh->foliageWindStrength>0.f ? static_cast<float>(GetElapsedTime()):0.f,
            mesh && mesh->foliageWindStrength>0.f ? 1.f : 0.f,
            mesh && mesh->foliageWindStrength>0.f ? std::clamp(mesh->foliageWindStrength,0.f,1.f):1.f, 0.f};
        objectData.grassAnimation.w = static_cast<float>(grassInteraction.count);
        std::copy(grassInteraction.data.begin(), grassInteraction.data.end(), objectData.grassCapsules);
        preparedDraw.objectData = objectData;
        preparedDraw.traversalChartPortal = renderItem->traversalChartPortal;

        // Finish view constants, select raster state, then bind draw geometry.
        Engine::Graphics::IPipelineState* materialPipeline = nullptr;
        if (isPreview)
                materialPipeline = wireframeMode
                    ? (doubleSided
                        ? m_objectPreviewWireDoubleSidedPipeline.get()
                        : m_objectPreviewWirePipeline.get())
                    : (doubleSided
                        ? m_objectPreviewDoubleSidedPipeline.get()
                        : m_objectPreviewPipeline.get());
        else if (alphaMode == Engine::Components::MaterialAlphaMode::Blend)
                materialPipeline = wireframeMode
                    ? (doubleSided
                        ? m_objectBlendWireDoubleSidedPipeline.get()
                        : m_objectBlendWirePipeline.get())
                    : (doubleSided
                        ? m_objectBlendDoubleSidedPipeline.get()
                        : m_objectBlendPipeline.get());
        else
                materialPipeline = wireframeMode
                    ? (doubleSided
                        ? m_objectWireDoubleSidedPipeline.get()
                        : m_objectWirePipeline.get())
                    : (doubleSided
                        ? m_objectDoubleSidedPipeline.get()
                        : m_objectPipeline.get());
        const auto pipelineResolveStart = RenderClock::now();
        materialPipeline = Renderer::SceneRendererMaterialShaders::ResolveObjectPipeline(
            *this, preparedDraw, mat, mesh, materialPipeline, isPreview,
            preparedDraw.blended, sprite != nullptr, wireframeMode, shadowOnly,
            preparedDraw.terrain, preparedDraw.terrainCell,
            renderItem->skinJointCount, foliagePipelines);
        materialPipelineMs += std::chrono::duration<double,std::milli>(
            RenderClock::now()-pipelineResolveStart).count();
        preparedDraw.constantBufferOffset = offset;
        const auto geometryBindStart = RenderClock::now();
        Renderer::SceneRendererDrawPreparation::BindObjectGeometry(
            preparedDraw, *renderItem, useSourceChart, useSoftSurface);
        materialGeometryBindingMs += std::chrono::duration<double,std::milli>(
            RenderClock::now()-geometryBindStart).count();
        preparedDraws.push_back(preparedDraw);

        ++slot;
    }
    Renderer::SceneTextureFootprints::Request(preparedDraws, viewProjection,
        viewportWidth, viewportHeight, shadowViewProjections, shadowCascadeCount,
        m_directionalShadowResolution);
    // Render phase 6: apply screen LOD, upload object records, and build instance batches.
    // Fixed editor LOD selection suppresses the other bands. Remove their
    // distance crossfade before publishing records or the selected band fades
    // into missing geometry (near LOD disappears beyond its first threshold).
    if (includeEditorVisuals && !shadowOnly && instanceLodDisplayOverride >= 0)
        for (auto& draw : preparedDraws)
            draw.objectData.geometryFade = Engine::Rendering::ResolveLodPreviewFade(
                draw.objectData.geometryFade, includeEditorVisuals, shadowOnly,
                instanceLodDisplayOverride);
    if(!shadowOnly && !(includeEditorVisuals && instanceLodDisplayOverride >= 0)) for(auto& draw:preparedDraws)
        if(Renderer::SceneRendererDrawPreparation::ScreenLodPolicy(*this, draw, hasPortalViews, includeEditorVisuals)) draw.drawData.flags|=0x00800000u;
    Renderer::SceneRendererObjectRecordUpload recordUpload(
        *this, preparedDraws, shadowOnly);
    m_lastRenderCpuTimings.materialMs = std::chrono::duration<double,std::milli>(RenderClock::now()-materialStart).count();
    m_lastRenderCpuTimings.materialPipelineMs = materialPipelineMs;
    m_lastRenderCpuTimings.materialGeometryBindingMs = materialGeometryBindingMs;
    m_lastRenderCpuTimings.materialOtherMs = std::max(0.0,
        m_lastRenderCpuTimings.materialMs-materialPipelineMs-materialGeometryBindingMs);
    m_lastRenderCpuTimings.candidateDraws=preparedDraws.size();
    using AutomaticGroup = Engine::Rendering::DrawBatchPreparation::Group;
    auto instanceBatching = Renderer::SceneInstanceBatching::Prepare(*this,
        preparedDraws, shadowOnly, automaticInstancing, hasPortalViews, includeEditorVisuals);
    auto& automaticGroups = instanceBatching.automaticGroups;
    auto& instanceTiles = instanceBatching.instanceTiles;
    auto* residentPlacementPool = instanceBatching.residentPlacementPool;
    auto& drawViewStats = shadowOnly
        ? m_lastRenderCpuTimings.shadowVisibility
        : m_lastRenderCpuTimings.cameraVisibility;
    drawViewStats.drawGroups += automaticGroups.size();
    std::set<std::pair<uintptr_t, uintptr_t>> materialGroupKeys;
    for (const auto& draw : preparedDraws)
        materialGroupKeys.emplace(reinterpret_cast<uintptr_t>(draw.material),
            reinterpret_cast<uintptr_t>(draw.pipeline));
    drawViewStats.materialGroups += materialGroupKeys.size();

    const auto ensureRecordsUploaded = [&recordUpload]()
    {
        recordUpload.EnsureUploaded();
    };
    if(!shadowOnly)
    {
        context->SetStructuredBuffer(6,m_lightDataBuffer.get()); context->SetStructuredBuffer(7,m_objectDataBuffer.get());
        context->SetStructuredBuffer(8,m_boneDataBuffer.get()); context->SetStructuredBuffer(10,m_emptyMorphDeltaBuffer.get());
        context->SetStructuredBuffer(11,m_emptyMorphWeightBuffer.get());
        // The material shader declares these buffers even when its clustered
        // branch is disabled. Bind valid zero data so DX12 never leaves the
        // corresponding root descriptors unset.
        context->SetStructuredBuffer(14, clusteredLightingEnabled
            ? m_clusterHeaderBuffer.get() : m_emptyMorphDeltaBuffer.get());
        context->SetStructuredBuffer(15, clusteredLightingEnabled
            ? m_clusterLightIndexBuffer.get() : m_emptyMorphWeightBuffer.get());
    }

    const auto isSpatialManipulatorCarrierDraw = [&](const PreparedDraw& draw)
    {
        return draw.spatialManipulator && draw.spatialManipulator->enabled;
    };

    Renderer::SceneRendererGeometrySubmission geometrySubmission(*this, context);

    Renderer::SceneRendererLodSelection lodSelection;
    lodSelection.Prepare(*this, preparedDraws, cam, cameraPosition,
        viewProjection, viewportWidth, viewportHeight, instanceViewOrdinal,
        shadowOnly, hasPortalViews, includeEditorVisuals,
        instanceLodDisplayOverride);
    const auto selectedLodDraws = lodSelection.SelectedGpuLodDraws(shadowOnly);
    auto& lodDrawMetrics = shadowOnly
        ? m_lastRenderCpuTimings.shadowSelectedLodDraws
        : m_lastRenderCpuTimings.colorSelectedLodDraws;
    for (size_t lod = 0; lod < selectedLodDraws.size(); ++lod)
        lodDrawMetrics[lod] += selectedLodDraws[lod];
    auto& screenPlacementLevels = lodSelection.PlacementLevels();
    std::vector<Engine::Rendering::PreparedInstanceDraw>* instanceDrawCapture = nullptr;
    const auto submitAutomaticGroup = [&](const AutomaticGroup& group,
        const glm::mat4& cullViewProjection, uint32_t objectBase,
        Engine::Graphics::IPipelineState* pipeline,
        const std::vector<uint8_t>* visibility)
        -> Renderer::SubmittedGeometry
    {
        const auto& representative=preparedDraws[group.members.front()];
        const bool screenColor=!shadowOnly && !visibility &&
            instanceLodDisplayOverride < 0 &&
            Renderer::SceneRendererDrawPreparation::ScreenLodPolicy(*this,
                representative, hasPortalViews, includeEditorVisuals);
        if(representative.mesh->IsInstanced())
        {
            Renderer::SubmittedGeometry submitted{};
            const bool gpuLod=(representative.drawData.flags & 0x00100000u)!=0;
            const int lodCount=gpuLod ? representative.gpuMaximumLod+1:1;
            for(int residentLod=0;residentLod<lodCount;++residentLod)
            {
            auto* sourceMesh=gpuLod ? representative.mesh->GetRuntimeLodMesh(residentLod):representative.mesh;
            for(size_t rangeIndex=0;rangeIndex<sourceMesh->GetInstanceRanges().size();++rangeIndex)
            {
                const auto& sourceRange=sourceMesh->GetInstanceRanges()[rangeIndex];
                if(!sourceRange.vertexCount) continue;
                auto* sourceIndexBuffer=sourceMesh->GetIndexBuffer();
                const bool indexed=sourceIndexBuffer!=nullptr;
                std::vector<Engine::Rendering::InstanceTileRecord> tiles;
                uint32_t count=0;
                for(auto index:group.members)
                {
                    if(visibility && !(*visibility)[index]) continue;
                    const auto& draw = preparedDraws[index];
                    if (includeEditorVisuals && !shadowOnly &&
                        instanceLodDisplayOverride >= 0)
                    {
                        if (draw.mesh->instanceLodBand >= 0)
                        {
                            const auto data = draw.mesh->GetInstanceData();
                            const int maximum = data
                                ? std::max(0, data->maximumLod) : 0;
                            if (draw.mesh->instanceLodBand !=
                                std::min(instanceLodDisplayOverride, maximum))
                                continue;
                        }
                        else if (gpuLod && residentLod !=
                            std::min(instanceLodDisplayOverride,
                                std::max(0, draw.gpuMaximumLod)))
                            continue;
                        else if (!gpuLod && !lodSelection.IsSelected(draw))
                            continue;
                    }
                    const auto& tile=(*instanceTiles[index])[rangeIndex];
                    if(!tile.range.y) continue;
                    if(screenColor)
                    {
                        const auto& data=*draw.mesh->GetInstanceData();
                        auto [decisions,created]=screenPlacementLevels.try_emplace(data.lodIdentity);
                        if(created)
                        {
                            glm::vec3 low=glm::vec3(tile.minimum),high=glm::vec3(tile.maximum);
                            const int maximum=draw.mesh->instanceLodBand>=0 ? data.maximumLod:draw.gpuMaximumLod;
                            if(data.hasLodBounds) {
                                const float padding=draw.mesh->GetDeformationBoundsPadding();
                                const glm::vec3 deformationPadding(padding);
                                low=data.lodMinimum-deformationPadding;
                                high=data.lodMaximum+deformationPadding;
                            }
                            decisions->second.reserve(data.transforms.size());
                            for(size_t placement=0;placement<data.transforms.size();++placement)
                            {
                                const auto& affine=data.transforms[placement];
                                glm::mat4 matrix(1.f);
                                for(int row=0;row<3;++row) for(int column=0;column<4;++column)
                                    matrix[column][row]=affine.rows[row][column];
                                decisions->second.push_back(lodSelection.ChooseScreen(*Renderer::SceneRendererDrawPreparation::ScreenLodPolicy(*this, draw, hasPortalViews, includeEditorVisuals),low,high,
                                    draw.objectData.world*matrix,data.lodIdentity,placement,1,maximum));
                            }
                            for (int selected : decisions->second)
                            {
                                ++m_screenLodCounts[selected];
                                auto& screenStats = m_lastRenderCpuTimings
                                    .cameraVisibility;
                                ++screenStats.screenSelectedInstancesByLod[selected];
                                const auto* geometry = draw.mesh->GetRuntimeLodMesh(selected);
                                if (geometry)
                                    screenStats.screenSelectedTrianglesByLod[selected] +=
                                        (geometry->GetIndexBuffer()
                                            ? geometry->GetIndexCount()
                                            : geometry->GetVertexCount()) / 3u;
                            }
                        }
                        const int band=gpuLod ? residentLod:draw.gpuLod;
                        // Emit contiguous runs into the existing persistent placement pool.
                        // Both compute and CPU fallback consume exactly the same decisions.
                        uint32_t run=0;
                        for(uint32_t offset=0;offset<=tile.range.y;++offset)
                        {
                            const bool selected=offset<tile.range.y &&
                                decisions->second[draw.mesh->GetInstanceRanges()[rangeIndex].firstInstance+offset]==band;
                            if(selected) ++run;
                            else if(run)
                            {
                                auto selectedTile=tile;
                                selectedTile.range.x+=offset-run;
                                selectedTile.range.y=run;
                                tiles.push_back(selectedTile); count+=run; run=0;
                            }
                        }
                    }
                    else
                    {
                        // CPU render blocks can contain thousands of placements.
                        // Keep compute work bounded per group without creating
                        // more CPU objects, materials, draws or placement copies.
                        Engine::Rendering::AppendInstanceCullChunks(tiles, tile);
                        count += tile.range.y;
                    }
                }
                if(!count) continue;
                auto& viewStats = shadowOnly
                    ? m_lastRenderCpuTimings.shadowVisibility
                    : m_lastRenderCpuTimings.cameraVisibility;
                const size_t lodMetric = static_cast<size_t>(std::clamp(
                    gpuLod ? residentLod : representative.gpuLod, 0, 2));
                viewStats.instanceTilesSubmitted += tiles.size();
                // This submission path is for ordinary Mesh instance ranges;
                // terrain-managed placements have their own terrain counters.
                viewStats.nonTerrainInstanceTilesSubmitted += tiles.size();
                viewStats.placementCandidates += count;
                viewStats.placementCandidatesByLod[lodMetric] += count;
                viewStats.placementTrianglesByLod[lodMetric] +=
                    uint64_t(sourceRange.vertexCount / 3u) * count;
                // Camera sorting must not invalidate unchanged tile metadata.
                std::sort(tiles.begin(),tiles.end(),[](const auto& first,const auto& second) {
                    return std::tie(first.range.z,first.range.x)<std::tie(second.range.z,second.range.x);
                });
                bool gpu=instanceLodDisplayOverride < 0 && gpuInstanceCulling &&
                    context->SupportsIndirectDraws() &&
                    (count>=32 || gpuLod || representative.mesh->instanceLodBand>=0);
                if(gpu && !m_instanceCullAttempted)
                {
                    m_instanceCullAttempted=true;
                    auto shader=m_graphicsProvider->GetShaderCompiler()->CompileFromFile(
                        EngineShaderPath("InstanceCull.hlsl"),"CSMain",Engine::Graphics::IShaderCompiler::CompileProfile::CS_5_0);
                    if(shader) m_instanceCullPipeline=m_graphicsProvider->GetPipelineStateFactory()->CreateComputePipeline(shader.get());
                }
                gpu=gpu && m_instanceCullPipeline;
                if(gpu) gpu=context->BeginComputePass();
                DrawCBData constants=representative.drawData;
                constants.objectIndex=objectBase; constants.flags|=(uint32_t(residentLod)<<21)|0x00080000u;
                if(shadowOnly) { constants.lightCount=0; constants.flags&=~kClusteredLightingFlag; }
                Engine::Rendering::PersistentInstanceBatches::Key key{
                    uintptr_t(cam),uintptr_t(shadowOnly),instanceViewOrdinal,objectBase/m_objectCapacity,
                    uintptr_t(sourceMesh->GetGraphicsBuffer()),uintptr_t(pipeline),sourceRange.firstVertex,sourceRange.vertexCount,
                    uintptr_t(residentLod) | (uintptr_t(representative.mesh->instanceLodBand+1)<<8),representative.drawData.flags,representative.drawData.lightCount,uintptr_t(representative.doubleSided),uintptr_t(representative.castsShadow)};
                for(auto* texture:representative.textures) key.push_back(uintptr_t(texture));
                if(indexed) key.push_back(uintptr_t(sourceIndexBuffer));
                if(!automaticInstancing) key.push_back(uintptr_t(representative.object));
                const auto uploadStart=RenderClock::now();
                auto* pool=residentPlacementPool;
                auto& buffers=m_persistentBatches.Acquire(m_graphicsProvider,std::move(key),gpu,tiles,&constants,m_persistentInstances,m_lastRenderCpuTimings.changedUploadBytes);
                if (buffers.completedReadback)
                {
                    auto& completed = buffers.readbackShadow
                        ? m_lastRenderCpuTimings.shadowVisibility
                        : m_lastRenderCpuTimings.cameraVisibility;
                    const size_t completedLod=std::min<size_t>(buffers.readbackLod,2);
                    completed.gpuSelectedInstancesByLod[completedLod]+=buffers.completedGpuCandidates;
                    completed.gpuTransitionBandInstancesByLod[completedLod]+=buffers.completedTransitionCandidates;
                    ++completed.gpuReadbackSamples;
                    completed.gpuReadbackViewIdentity=buffers.readbackViewIdentity;
                    buffers.completedReadback=false;
                }
                m_lastRenderCpuTimings.uploadMs+=std::chrono::duration<double,std::milli>(RenderClock::now()-uploadStart).count();
                m_lastRenderCpuTimings.grassBatchCacheHits+=!buffers.metadataChanged;
                auto& storage=m_lastRenderCpuTimings.instances;
                storage.metadataBytes+=buffers.metadataUploadBytes; storage.visibleIdBytes+=buffers.idUploadBytes;
                storage.allocations+=buffers.allocations;
                if(buffers.cold) ++storage.cacheCold;
                else if(buffers.modeChanged) ++storage.cacheMode;
                else if(buffers.membershipChanged) ++storage.cacheMembership;
                else if(buffers.metadataChanged) ++storage.cacheMetadata;
                if(gpu)
                {
                    ++m_lastRenderCpuTimings.gpuGrassBatches;
                    if(shadowOnly) ++m_lastShadowCpuTiming.gpuGrassBatches;
                    context->SetStructuredBuffer(45,m_emptyMorphDeltaBuffer.get());
                    context->SetComputePipeline(m_instanceCullPipeline.get());
                    Engine::Rendering::InstanceCullConstants cull;
                    cull.viewProjection=cullViewProjection; cull.cameraPosition=glm::vec4(cameraPosition,1);
                    cull.counts={uint32_t(tiles.size()),sourceRange.vertexCount,sourceRange.firstVertex,1};
                    cull.selection={screenColor ? -1:gpuLod ? residentLod:representative.gpuLod,2,representative.gpuMaximumLod,
                        (shadowOnly ? 1:0) | (representative.mesh->instanceLodBand>=0 ? 2:0)};
                    context->SetComputeReadBuffer(buffers.input.get()); context->SetComputeReadBuffer(pool,1); context->SetComputeWriteBuffer(buffers.output.get());
                    context->SetComputeConstants(&cull,sizeof(cull)); context->DispatchCompute(1); context->StorageBarrier(buffers.output.get());
                    cull.counts.w=0;
                    context->SetComputePipeline(m_instanceCullPipeline.get());
                    context->SetComputeReadBuffer(buffers.input.get()); context->SetComputeReadBuffer(pool,1); context->SetComputeWriteBuffer(buffers.output.get());
                    context->SetComputeConstants(&cull,sizeof(cull)); context->DispatchCompute(uint32_t(tiles.size()));
                    context->StorageBarrier(buffers.output.get()); context->EndComputePass(); ++m_lastRenderCpuTimings.gpuBatches;
                    Engine::Rendering::PersistentInstanceBatches::ScheduleVisibilityReadback(
                        context,buffers,static_cast<uint32_t>(lodMetric),shadowOnly,diagnosticViewIdentity);
                }
                ensureRecordsUploaded();
                const Engine::Rendering::PreparedInstanceDraw instanceDraw{
                    buffers.draw.get(), buffers.output.get(), pool,
                    sourceMesh->GetGraphicsBuffer(), sourceIndexBuffer,
                    sourceMesh->GetVertexStride(), sourceMesh->GetIndexCount(),
                    sourceRange.vertexCount, sourceRange.firstVertex, buffers.candidates, gpu};
                instanceDraw.Submit(*context, pipeline, m_emptyMorphDeltaBuffer.get(),
                    m_emptyMorphWeightBuffer.get());
                if (instanceDrawCapture) instanceDrawCapture->push_back(instanceDraw);
                ++submitted.draws;
                submitted.instances += buffers.candidates;
                submitted.triangles += static_cast<uint64_t>(
                    sourceRange.vertexCount / 3u) * buffers.candidates;
                ++m_lastRenderCpuTimings.automaticBatches;
                m_lastRenderCpuTimings.automaticInstances+=buffers.candidates;
            }
            }
            const auto& stats=m_persistentInstances.stats;
            auto& storage=m_lastRenderCpuTimings.instances;
            storage.newBytes=stats.newBytes-poolBefore.newBytes;
            storage.editedBytes=stats.editedBytes-poolBefore.editedBytes;
            storage.growthBytes=stats.growthBytes-poolBefore.growthBytes;
            storage.residentBytes=m_persistentInstances.ResidentBytes();
            storage.preparedRecords=stats.preparedRecords-poolBefore.preparedRecords;
            storage.preparationWorkerPasses=stats.workerPasses-poolBefore.workerPasses;
            m_lastRenderCpuTimings.grassPlacementUploadBytes=stats.newBytes+stats.editedBytes+stats.growthBytes-
                poolBefore.newBytes-poolBefore.editedBytes-poolBefore.growthBytes;
            return submitted;
        }
        bool gpu=instanceLodDisplayOverride < 0 && gpuInstanceCulling &&
            context->SupportsIndirectDraws() &&
            (group.members.size()>=32 || representative.gpuLod>=0);
        if (gpu && !m_instanceCullAttempted)
        {
            m_instanceCullAttempted=true;
            auto shader=m_graphicsProvider->GetShaderCompiler()->CompileFromFile(
                EngineShaderPath("InstanceCull.hlsl"),"CSMain",Engine::Graphics::IShaderCompiler::CompileProfile::CS_5_0);
            if(shader) m_instanceCullPipeline=m_graphicsProvider->GetPipelineStateFactory()->CreateComputePipeline(shader.get());
        }
        gpu=gpu && m_instanceCullPipeline;
        if(gpu) gpu=context->BeginComputePass();
        auto& instances=m_automaticInstanceCache.scratch;
        instances.clear();
        for(auto index:group.members)
        {
            const auto& draw=preparedDraws[index];
            if(visibility && !(*visibility)[index]) continue;
            if((!gpu || screenColor) && !lodSelection.IsSelected(draw, visibility || shadowOnly)) continue;
            const auto bounds=Engine::Rendering::TransformBounds(draw.mesh->GetBoundsMin(),draw.mesh->GetBoundsMax(),draw.objectData.world);
            Engine::Rendering::AutomaticInstance instance;
            instance.sphere=glm::vec4((bounds.minimum+bounds.maximum)*.5f,glm::length((bounds.maximum-bounds.minimum)*.5f));
            const auto origin=glm::vec3(draw.objectData.world[3]);
            instance.meta={draw.drawData.objectIndex,uint32_t(draw.gpuMaximumLod),
                glm::floatBitsToUint(origin.x),glm::floatBitsToUint(origin.y)};
            const auto* lodPolicy = draw.object
                ? draw.object->GetComponent<Engine::Components::Mesh>() : draw.mesh;
            const float finalCullDistance = lodPolicy && lodPolicy->cullAfterLastLod
                ? std::max(0.f, lodPolicy->finalLodCullDistance) : 0.f;
            instance.lod={draw.gpuLodDistances,origin.z,finalCullDistance}; instances.push_back(instance);
        }
        if(instances.empty()) { if(gpu) context->EndComputePass(); return {}; }
        DrawCBData constants=representative.drawData;
        constants.objectIndex=objectBase; constants.flags|=0x10000000u;
        if(shadowOnly) { constants.lightCount=0; constants.flags&=~kClusteredLightingFlag; }
        const auto batchUploadStart=RenderClock::now();
        auto& buffers=m_automaticInstanceCache.Acquire(m_graphicsProvider,gpu,instances,&constants,m_lastRenderCpuTimings.changedUploadBytes);
        if (buffers.completedReadback)
        {
            auto& completed=buffers.readbackShadow
                ? m_lastRenderCpuTimings.shadowVisibility
                : m_lastRenderCpuTimings.cameraVisibility;
            const size_t completedLod=std::min<size_t>(buffers.readbackLod,2);
            completed.gpuSelectedInstancesByLod[completedLod]+=buffers.completedGpuCandidates;
            ++completed.gpuReadbackSamples;
            completed.gpuReadbackViewIdentity=buffers.readbackViewIdentity;
            buffers.completedReadback=false;
        }
        m_lastRenderCpuTimings.uploadMs+=std::chrono::duration<double,std::milli>(RenderClock::now()-batchUploadStart).count();
        if(gpu)
        {
            context->SetStructuredBuffer(45,m_emptyMorphDeltaBuffer.get());
            context->SetComputePipeline(m_instanceCullPipeline.get());
            Engine::Rendering::InstanceCullConstants cull;
            cull.viewProjection=cullViewProjection; cull.cameraPosition=glm::vec4(cameraPosition,1);
            cull.counts={uint32_t(instances.size()),representative.indexBuffer ? representative.indexCount : representative.vertexCount,
                representative.indexBuffer ? 1u:0u,1u}; cull.selection.x=screenColor ? -1:representative.gpuLod;
            context->SetComputeReadBuffer(buffers.input.get()); context->SetComputeWriteBuffer(buffers.output.get());
            context->SetComputeConstants(&cull,sizeof(cull)); context->DispatchCompute(1); context->StorageBarrier(buffers.output.get());
            cull.counts.w=0;
            context->SetComputePipeline(m_instanceCullPipeline.get());
            context->SetComputeWriteBuffer(buffers.output.get()); context->SetComputeReadBuffer(buffers.input.get());
            context->SetComputeConstants(&cull,sizeof(cull)); context->DispatchCompute((uint32_t(instances.size())+63)/64);
            context->StorageBarrier(buffers.output.get()); context->EndComputePass(); ++m_lastRenderCpuTimings.gpuBatches;
            Engine::Rendering::AutomaticInstanceCache::ScheduleVisibilityReadback(
                context,buffers,static_cast<uint32_t>(std::clamp(representative.gpuLod,0,2)),
                shadowOnly,diagnosticViewIdentity);
        }
        ensureRecordsUploaded();
        context->SetPipeline(pipeline);
        context->SetConstantBuffer(0,buffers.draw.get());
        context->SetStructuredBuffer(45,buffers.output.get());
        context->SetStructuredBuffer(46,m_emptyMorphDeltaBuffer.get());
        context->SetStructuredBuffer(10,m_emptyMorphDeltaBuffer.get());
        context->SetStructuredBuffer(11,m_emptyMorphWeightBuffer.get());
        context->SetVertexBuffer(0,representative.vertexBuffer,representative.vertexStride,0);
        if(representative.indexBuffer) context->SetIndexBuffer(representative.indexBuffer,representative.indexCount,0);
        if(gpu) context->DrawInstancesIndirect(buffers.output.get(),0,representative.indexBuffer!=nullptr);
        else if(representative.indexBuffer) context->DrawIndexedInstanced(representative.indexCount,uint32_t(instances.size()));
        else context->DrawInstanced(representative.vertexCount,uint32_t(instances.size()));
        ++m_lastRenderCpuTimings.automaticBatches; m_lastRenderCpuTimings.automaticInstances+=instances.size();
        const uint32_t primitiveCount = representative.indexBuffer
            ? representative.indexCount : representative.vertexCount;
        return {1u, static_cast<uint64_t>(instances.size()),
            static_cast<uint64_t>(primitiveCount / 3u) * instances.size()};
    };

    // Render the selected realtime directional light into its depth map during
    // the explicit pre-view phase. This keeps Vulkan render passes unnested and
    // lets DX12 bind a depth-only target without reconstructing prior OM state.
    // Render phase 7: submit directional and portal shadow passes.
    const auto shadowPreparationEnd = std::chrono::steady_clock::now();
    if (shadowOnly)
        m_lastShadowCpuTiming.scenePreparationMs = shadowElapsedMs(
            shadowCpuStart, shadowPreparationEnd);
    if (shadowOnly && shadowSelection.valid && renderMainDirectionalShadow &&
        m_directionalShadowPipeline &&
        context->BeginDepthOnlyPass(m_directionalShadowMap.get(),
            preserveDirectionalShadowAtlas ? -1.f : 1.f))
    {
        m_shadowUploadCache.Prepare(size_t(m_shadowObjectDataBuffer->GetSize()/sizeof(ObjectGPUData)),sizeof(ObjectGPUData));
        context->BeginGpuTiming(Engine::Graphics::GpuTimingStage::Shadow);
        std::array<std::vector<uint8_t>,4> cascadeVisible;
        // Each cascade owns its visibility and receiver results. The spatial
        // tree is immutable for this render, and uncached queries leave the
        // single-owner view cache untouched.
        Engine::Rendering::RenderJobs::Get().For(shadowCascadeCount,1,[&](size_t cascade)
        {
            const auto frameVisible=hasPortalViews ? std::vector<uint8_t>{} :
                m_renderSpatialIndex.QueryUncached(shadowViewProjections[cascade]);
            auto& visible=cascadeVisible[cascade];
            visible.resize(preparedDraws.size(),0);
            for(size_t drawIndex=0;drawIndex<preparedDraws.size();++drawIndex)
            {
                const PreparedDraw& draw=preparedDraws[drawIndex];
                if (!draw.castsShadow || !draw.shadowVertexBuffer ||
                    isSpatialManipulatorCarrierDraw(draw)) continue;
                if(!frameVisible.empty() &&
                    !frameVisible[frameRenderItems[draw.frameItemIndex].objectSlot]) continue;
                visible[drawIndex]=!Renderer::SceneRendererShadows::IsOutsideCascade(draw, mainShadowCascades, static_cast<uint32_t>(cascade));
            }
            const uint32_t cascadeCasterBudget =
                m_realtimeShadowSettings.directionalCascadeCasterBudget;
            if (cascadeCasterBudget > 0u)
            {
                std::vector<std::pair<float, size_t>> candidates;
                candidates.reserve(preparedDraws.size());
                const glm::vec3 cascadeCenter =
                    mainShadowCascades.centers[cascade];
                for (size_t drawIndex = 0; drawIndex < preparedDraws.size();
                    ++drawIndex)
                {
                    if (!visible[drawIndex])
                        continue;
                    const PreparedDraw& draw = preparedDraws[drawIndex];
                    const glm::vec3 origin =
                        glm::vec3(draw.objectData.world[3]);
                    const float distance =
                        glm::length(origin - cascadeCenter);
                    const float lodPenalty = draw.gpuLod >= 0
                        ? static_cast<float>(draw.gpuLod) * 8.f : 0.f;
                    const float grassPenalty =
                        draw.mesh && draw.mesh->usesGrassShadowBudget ? 10.f : 0.f;
                    const float terrainPenalty = draw.terrainCell ? 4.f : 0.f;
                    candidates.emplace_back(distance + lodPenalty +
                        grassPenalty + terrainPenalty, drawIndex);
                }
                if (candidates.size() > cascadeCasterBudget)
                {
                    std::nth_element(candidates.begin(),
                        candidates.begin() + cascadeCasterBudget,
                        candidates.end(),
                        [](const auto& left, const auto& right)
                        {
                            return left.first == right.first
                                ? left.second < right.second
                                : left.first < right.first;
                        });
                    for (size_t index = cascadeCasterBudget;
                        index < candidates.size(); ++index)
                        visible[candidates[index].second] = 0;
                }
            }
        });
        std::vector<uint8_t> cascadeMasks(preparedDraws.size(), 0u);
        for(size_t drawIndex=0;drawIndex<preparedDraws.size();++drawIndex)
        {
            for (uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade)
                if (cascadeVisible[cascade][drawIndex])
                    cascadeMasks[drawIndex] |= static_cast<uint8_t>(1u << cascade);
        }
        const auto shadowCullingEnd = std::chrono::steady_clock::now();
        m_lastShadowCpuTiming.cascadeCullingMs = shadowElapsedMs(
            shadowPreparationEnd, shadowCullingEnd);
        std::array<uint32_t,4> firstDirtyByCascade, lastDirtyByCascade{};
        firstDirtyByCascade.fill(UINT32_MAX);
        Engine::Rendering::RenderJobs::Get().For(shadowCascadeCount,1,[&](size_t cascade)
        {
            for (size_t drawIndex = 0; drawIndex < preparedDraws.size(); ++drawIndex)
            {
                if (!(cascadeMasks[drawIndex] & (1u << cascade)))
                    continue;
                const PreparedDraw& draw = preparedDraws[drawIndex];
                const uint32_t recordIndex = static_cast<uint32_t>(cascade) * m_objectCapacity +
                    draw.drawData.objectIndex;
                ObjectGPUData shadowData = draw.objectData;
                shadowData.viewProjection = shadowViewProjections[cascade];
                shadowData.shadowCasterFade.z=1.f;
                shadowData.materialParams.w = static_cast<float>(
                    static_cast<uint32_t>(shadowData.materialParams.w) &
                    (1u | 32u));
                shadowData.ambientUnlit.w = 1.f;
                shadowData.environmentParams = glm::vec4(0.f);
                if(m_shadowUploadCache.Write(recordIndex,&shadowData,m_shadowObjectDataMapped))
                {
                    firstDirtyByCascade[cascade]=std::min(firstDirtyByCascade[cascade],recordIndex);
                    // Camera-distance sorting is independent of persistent
                    // slot order. A later draw may have a lower slot; every
                    // changed record must remain inside the upload range.
                    lastDirtyByCascade[cascade]=std::max(lastDirtyByCascade[cascade],recordIndex);
                }
                DrawCBData shadowDraw = draw.drawData;
                shadowDraw.objectIndex = recordIndex;
                shadowDraw.lightCount = 0;
                shadowDraw.flags &= ~kClusteredLightingFlag;
                std::memcpy(static_cast<uint8_t*>(m_shadowCBMapped) +
                    static_cast<uint64_t>(recordIndex) * kCBStride,
                    &shadowDraw, sizeof(shadowDraw));
            }
        });
        uint32_t firstDirtyShadow=UINT32_MAX,lastDirtyShadow=0;
        for (uint32_t cascade = 0; cascade < shadowCascadeCount; ++cascade)
        {
            firstDirtyShadow=std::min(firstDirtyShadow,firstDirtyByCascade[cascade]);
            lastDirtyShadow=std::max(lastDirtyShadow,lastDirtyByCascade[cascade]);
        }
        if(firstDirtyShadow!=UINT32_MAX)
        {
            const uint64_t bytes=uint64_t(lastDirtyShadow-firstDirtyShadow+1)*sizeof(ObjectGPUData);
            m_shadowObjectDataBuffer->FlushMappedWrites(uint64_t(firstDirtyShadow)*sizeof(ObjectGPUData),bytes);
            m_lastRenderCpuTimings.changedUploadBytes+=bytes;
            RecordFrameUploadBytes(bytes);
        }
        context->SetStructuredBuffer(6, m_lightDataBuffer.get());
        context->SetStructuredBuffer(7, m_shadowObjectDataBuffer.get());
        context->SetStructuredBuffer(8, m_boneDataBuffer.get());
        context->PrepareConstantBuffer(m_shadowConstantBuffer.get(),
            static_cast<uint64_t>(shadowCascadeCount) * m_objectCapacity * kCBStride);
        const auto shadowRecordsEnd = std::chrono::steady_clock::now();
        m_lastShadowCpuTiming.recordUploadMs = shadowElapsedMs(
            shadowCullingEnd, shadowRecordsEnd);

        Renderer::DirectionalShadowSubmissionContext shadowSubmission{
            *this, context, preparedDraws, cascadeVisible, cascadeMasks,
            directionalShadowDirtyMask, shadowCascadeCount,
            m_directionalShadowResolution, preserveDirectionalShadowAtlas,
            mainShadowCascades, quality, shadowSelection, cameraPosition,
            automaticGroups, lodSelection, geometrySubmission,
            submitAutomaticGroup, shadowRecordsEnd };
        Renderer::SceneRendererDirectionalShadowSubmission::Submit(
            shadowSubmission);
    }
    else if (shadowOnly && shadowSelection.valid &&
        !renderMainDirectionalShadow)
    {
        m_lastShadowCpuTiming.cascadeCullingMs = 0.0;
        m_lastShadowCpuTiming.recordUploadMs = 0.0;
        m_lastShadowCpuTiming.drawSubmissionMs = 0.0;
        m_lastShadowCpuTiming.passRestoreMs = 0.0;
    }

    // Render phase 8: schedule portal views and prepare their stencil/aperture work.
    const auto portalStart = std::chrono::steady_clock::now();
    if (shadowOnly && shadowSelection.valid &&
        m_realtimeShadowSettings.portalPolicy !=
            Engine::Model::PortalShadowPolicy::ReuseMain)
    {
        auto* bufferFactory = m_graphicsProvider->GetBufferFactory();
        auto* textureFactory = m_graphicsProvider->GetTextureFactory();
        const uint32_t portalResolution = std::clamp(static_cast<uint32_t>(
            static_cast<float>(m_realtimeShadowSettings.portalAtlasResolution) *
            shadowSelection.resolutionScale), 256u, 4096u);
        const uint32_t portalCascadeCount = std::clamp(
            m_realtimeShadowSettings.portalCascadeCount, 1u, 4u);
        for (PortalShadowCacheEntry& entry : m_portalShadowCache)
        {
            if (entry.ownerCamera != cam || !entry.targetChart ||
                !entry.selected)
                continue;
            if (!entry.atlas || entry.atlasResolution != portalResolution)
            {
                if (entry.atlas)
                    m_retiredShadowTextures.push_back(std::move(entry.atlas));
                entry.atlas = textureFactory
                    ? textureFactory->CreateDepthTexture2D(
                        portalResolution, portalResolution)
                    : nullptr;
                entry.atlasResolution = entry.atlas ? portalResolution : 0u;
                entry.rendered = false;
            }
            // A recorded native frame may still be reading these upload
            // buffers when the next frame prepares its portal shadows.
            if (entry.bufferCapacity != m_objectCapacity ||
                m_recordingNativeCommands)
            {
                if (entry.constantBuffer && entry.constantMapped)
                    entry.constantBuffer->Unmap();
                if (entry.objectBuffer && entry.objectMapped)
                    entry.objectBuffer->Unmap();
                if (entry.constantBuffer)
                    m_retiredRenderBuffers.push_back(
                        std::move(entry.constantBuffer));
                if (entry.objectBuffer)
                    m_retiredRenderBuffers.push_back(
                        std::move(entry.objectBuffer));
                entry.constantBuffer = bufferFactory->CreateBuffer(
                    Engine::Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
                    Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
                    static_cast<uint64_t>(m_objectCapacity) * 4u * kCBStride,
                    nullptr, sizeof(DrawCBData));
                entry.objectBuffer = bufferFactory->CreateBuffer(
                    Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
                    Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
                    static_cast<uint64_t>(m_objectCapacity) * 4u *
                        sizeof(ObjectGPUData), nullptr, sizeof(ObjectGPUData));
                entry.constantMapped = entry.constantBuffer
                    ? entry.constantBuffer->Map() : nullptr;
                entry.objectMapped = entry.objectBuffer
                    ? entry.objectBuffer->Map() : nullptr;
                entry.bufferCapacity = entry.constantMapped && entry.objectMapped
                    ? m_objectCapacity : 0u;
                entry.rendered = false;
            }
            if (!entry.atlas || !entry.constantMapped || !entry.objectMapped)
                continue;

            const auto portalCascades = Renderer::SceneRendererShadows::BuildCascades(
                *this, shadowSelection, quality, cam, entry.view, portalResolution,
                portalCascadeCount);
            entry.viewProjections = portalCascades.viewProjections;
            entry.cascadeSplits = portalCascades.splits;
            entry.cascadeData = portalCascades.data;
            entry.cameraData = portalCascades.cameraData;
            for (uint32_t cascade = 0; cascade < portalCascades.count; ++cascade)
            {
                for (const PreparedDraw& draw : preparedDraws)
                {
                    const uint32_t recordIndex = cascade * m_objectCapacity +
                        draw.drawData.objectIndex;
                    ObjectGPUData shadowData = draw.objectData;
                    shadowData.viewProjection =
                        portalCascades.viewProjections[cascade];
                    shadowData.shadowCasterFade.z=1.f;
                    shadowData.materialParams.w = static_cast<float>(
                        static_cast<uint32_t>(shadowData.materialParams.w) &
                        (1u | 32u));
                    shadowData.ambientUnlit.w = 1.f;
                    shadowData.environmentParams = glm::vec4(0.f);
                    std::memcpy(static_cast<uint8_t*>(entry.objectMapped) +
                        static_cast<size_t>(recordIndex) * sizeof(ObjectGPUData),
                        &shadowData, sizeof(shadowData));
                    DrawCBData shadowDraw = draw.drawData;
                    shadowDraw.objectIndex = recordIndex;
                    shadowDraw.lightCount = 0;
                    shadowDraw.flags &= ~kClusteredLightingFlag;
                    std::memcpy(static_cast<uint8_t*>(entry.constantMapped) +
                        static_cast<uint64_t>(recordIndex) * kCBStride,
                        &shadowDraw, sizeof(shadowDraw));
                }
            }
            entry.objectBuffer->FlushMappedWrites(0,
                static_cast<uint64_t>(portalCascades.count) *
                    m_objectCapacity * sizeof(ObjectGPUData));
            if (!context->BeginDepthOnlyPass(entry.atlas.get(), 1.f))
                continue;
            context->SetStructuredBuffer(6, m_lightDataBuffer.get());
            context->SetStructuredBuffer(7, entry.objectBuffer.get());
            context->SetStructuredBuffer(8, m_boneDataBuffer.get());
            const uint32_t tileResolution = portalCascades.count > 1u
                ? portalResolution / 2u : portalResolution;
            for (uint32_t cascade = 0; cascade < portalCascades.count; ++cascade)
            {
                const uint32_t tileX = portalCascades.count > 1u
                    ? cascade & 1u : 0u;
                const uint32_t tileY = portalCascades.count > 1u
                    ? cascade >> 1u : 0u;
                context->SetViewport({ static_cast<float>(tileX * tileResolution),
                    static_cast<float>(tileY * tileResolution),
                    static_cast<float>(tileResolution),
                    static_cast<float>(tileResolution), 0.f, 1.f });
                context->SetScissorRect({
                    static_cast<int32_t>(tileX * tileResolution),
                    static_cast<int32_t>(tileY * tileResolution),
                    static_cast<int32_t>((tileX + 1u) * tileResolution),
                    static_cast<int32_t>((tileY + 1u) * tileResolution) });
                for (const PreparedDraw& draw : preparedDraws)
                {
                    if (!draw.castsShadow || !draw.vertexBuffer ||
                        isSpatialManipulatorCarrierDraw(draw) ||
                        !IsObjectInSpatialRegion(draw.object,
                            entry.contentScopeRoot) ||
                        !Engine::Rendering::Portal::
                            IsTraversalInstanceVisibleInConnectedChart(
                                draw.traversalChartPortal, entry.targetChart))
                        continue;
                    Engine::Graphics::IPipelineState* shadowPipeline =
                        draw.shadowDoubleSided
                            ? m_directionalShadowDoubleSidedPipeline.get()
                            : m_directionalShadowPipeline.get();
                    context->SetPipeline(geometrySubmission.GeometryPipeline(draw, shadowPipeline));
                    const uint32_t recordIndex = cascade * m_objectCapacity +
                        draw.drawData.objectIndex;
                    context->SetConstantBuffer(0, entry.constantBuffer.get(),
                        static_cast<uint64_t>(recordIndex) * kCBStride);
                    for (uint32_t textureSlot = 0;
                        textureSlot < 12; ++textureSlot)
                        if (textureSlot != 7u)
                            context->SetTexture(
                                textureSlot, draw.textures[textureSlot]);
                    geometrySubmission.SubmitGeometry(draw);
                }
            }
            context->EndDepthOnlyPass();
            entry.rendered = true;
        }
    }

    const auto updatePortalShadowDebug = [&]()
    {
        m_shadowDebugSnapshot.portalAtlasCount = 0;
        m_shadowDebugSnapshot.renderedPortalAtlasCount = 0;
        for (const PortalShadowCacheEntry& entry : m_portalShadowCache)
        {
            if (entry.atlas)
                ++m_shadowDebugSnapshot.portalAtlasCount;
            if (entry.atlas && entry.rendered)
                ++m_shadowDebugSnapshot.renderedPortalAtlasCount;
        }
        m_shadowDebugSnapshot.portalAtlasResolution =
            m_realtimeShadowSettings.portalPolicy ==
                Engine::Model::PortalShadowPolicy::ReuseMain
                ? 0u : m_realtimeShadowSettings.portalAtlasResolution;
    };

    if (shadowOnly)
    {
        updatePortalShadowDebug();
        m_lastShadowCpuTiming.instances=m_lastRenderCpuTimings.instances;
        const auto shadowEnd = std::chrono::steady_clock::now();
        m_lastShadowCpuTiming.portalMs = shadowElapsedMs(portalStart, shadowEnd);
        m_lastShadowCpuTiming.totalMs = shadowElapsedMs(
            shadowCpuStart, shadowEnd);
        return;
    }

    std::optional<Engine::Graphics::GpuTimingStage> activeGpuStage;
    std::array<const Engine::Graphics::IGraphicsTexture*, 30> boundTextures{};
    bool texturesBound = false;
    uint32_t boundTextureSlotCount = 12;
    const auto switchGpuStage = [&](Engine::Graphics::GpuTimingStage stage)
    {
        if (activeGpuStage && *activeGpuStage == stage)
            return;
        if (activeGpuStage)
            context->EndGpuTiming(*activeGpuStage);
        context->BeginGpuTiming(stage);
        activeGpuStage = stage;
    };
    const auto colorGpuStage = [](const PreparedDraw& draw)
    {
        using Stage = Engine::Graphics::GpuTimingStage;
        if (draw.terrain || draw.terrainCell) return Stage::Terrain;
        if (draw.mesh && draw.mesh->usesGrassShadowBudget) return Stage::Grass;
        if (draw.blended) return Stage::Transparent;
        // Bundled oak source geometry, including shared prefab instances.
        if (draw.treeGeometry)
            return draw.material && draw.material->doubleSided ? Stage::TreeLeaves : Stage::TreeBark;
        return Stage::Opaque;
    };
    m_lastOrdinaryDrawCount=0;
    // Render phase 9: submit camera geometry, portal views, and debug overlays.
    const auto submissionStart=RenderClock::now();
    const double uploadBeforeSubmission=m_lastRenderCpuTimings.uploadMs;
    // Resolve coverage before expensive masked-surface lighting. Both passes
    // consume the SAME visibility/LOD output and constants, including editor
    // LOD overrides. Unsupported shader/geometry policies keep the normal path.
    std::vector<std::vector<Engine::Rendering::PreparedInstanceDraw>> foliageDepthDraws(automaticGroups.size());
    std::vector<uint8_t> foliageDepthPrepared(automaticGroups.size(), 0);
    const bool foliageDepthEnabled = !wireframeMode && !hasPortalViews &&
        !m_diagnosticOpaqueDepthEqual && ReadEnvFlag("ENGINE_FOLIAGE_DEPTH_PREPASS", true);
    const auto depthEligible = [&](const PreparedDraw& draw)
    {
        const auto flags = static_cast<uint32_t>(draw.objectData.materialParams.w);
        return draw.mesh && draw.mesh->IsInstanced() && draw.material &&
            draw.material->shader.empty() && !draw.blended && !draw.preview &&
            !draw.terrain && !draw.terrainCell && !draw.pmxOutline &&
            !draw.spatialManipulator && !draw.traversalChartPortal &&
            !draw.mesh->usesGrassShadowBudget && (flags & 32u) && !(flags & 64u) &&
            draw.objectData.skinParams.y <= 0.f && !draw.mesh->HasMorphTargets() &&
            !draw.mesh->HasSurfaceOffsets() &&
            draw.vertexBuffer == draw.mesh->GetGraphicsBuffer() &&
            draw.pipeline == (draw.doubleSided ? m_objectDoubleSidedPipeline.get() : m_objectPipeline.get());
    };
    if (foliageDepthEnabled)
        for (size_t groupIndex = 0; groupIndex < automaticGroups.size(); ++groupIndex)
        {
            const auto& group = automaticGroups[groupIndex];
            if (group.members.empty() || !std::all_of(group.members.begin(), group.members.end(),
                [&](auto index) {
                    // Singleton candidates can remain on the ordinary draw
                    // path and have no prepared instance tiles to replay.
                    return preparedDraws[index].automaticGroup == groupIndex &&
                        depthEligible(preparedDraws[index]);
                })) continue;
            const auto& draw = preparedDraws[group.members.front()];
            auto* depthPipeline = m_foliageDepthPipelines[draw.doubleSided ? 1 : 0].get();
            if (!depthPipeline || !m_foliageEqualPipelines[draw.doubleSided ? 1 : 0]) continue;
            switchGpuStage(Engine::Graphics::GpuTimingStage::FoliageDepth);
            for (uint32_t textureSlot = 0; textureSlot < std::max(draw.textureSlotCount, boundTextureSlotCount); ++textureSlot)
                if (!texturesBound || boundTextures[textureSlot] != draw.textures[textureSlot])
                    context->SetTexture(textureSlot, draw.textures[textureSlot]);
            boundTextures = draw.textures; boundTextureSlotCount = draw.textureSlotCount; texturesBound = true;
            instanceDrawCapture = &foliageDepthDraws[groupIndex];
            const auto submitted = submitAutomaticGroup(group, viewProjection, 0, depthPipeline, nullptr);
            instanceDrawCapture = nullptr;
            foliageDepthPrepared[groupIndex] = 1;
            m_lastOrdinaryDrawCount += static_cast<uint32_t>(submitted.draws);
            m_lastRenderCpuTimings.foliageDepthDraws += submitted.draws;
            m_lastRenderCpuTimings.submittedInstances += submitted.instances;
            m_lastRenderCpuTimings.submittedTriangles += submitted.triangles;
        }
    std::vector<uint8_t> submittedAutomaticGroups(automaticGroups.size(),0);
    for (const PreparedDraw& draw : preparedDraws)
    {
        if(draw.automaticGroup!=UINT32_MAX)
        {
            if(submittedAutomaticGroups[draw.automaticGroup]) continue;
            submittedAutomaticGroups[draw.automaticGroup]=1;
            switchGpuStage(colorGpuStage(draw));
            for(uint32_t textureSlot=0;textureSlot<std::max(draw.textureSlotCount,boundTextureSlotCount);++textureSlot)
                if(!texturesBound || boundTextures[textureSlot]!=draw.textures[textureSlot]) context->SetTexture(textureSlot,draw.textures[textureSlot]);
            boundTextures=draw.textures; boundTextureSlotCount=draw.textureSlotCount; texturesBound=true;
            if (foliageDepthPrepared[draw.automaticGroup])
            {
                for (const auto& instanceDraw : foliageDepthDraws[draw.automaticGroup])
                {
                    instanceDraw.Submit(*context, m_foliageEqualPipelines[draw.doubleSided ? 1 : 0].get(),
                        m_emptyMorphDeltaBuffer.get(), m_emptyMorphWeightBuffer.get());
                    ++m_lastOrdinaryDrawCount;
                    ++m_lastRenderCpuTimings.automaticBatches;
                    m_lastRenderCpuTimings.automaticInstances += instanceDraw.instanceCount;
                    m_lastRenderCpuTimings.submittedInstances += instanceDraw.instanceCount;
                    m_lastRenderCpuTimings.submittedTriangles +=
                        uint64_t(instanceDraw.elementCount / 3u) * instanceDraw.instanceCount;
                }
                continue;
            }
            const auto submitted = submitAutomaticGroup(automaticGroups[draw.automaticGroup],viewProjection,0,geometrySubmission.GeometryPipeline(draw,draw.pipeline),nullptr);
            m_lastOrdinaryDrawCount += static_cast<uint32_t>(submitted.draws);
            m_lastRenderCpuTimings.submittedInstances += submitted.instances;
            m_lastRenderCpuTimings.submittedTriangles += submitted.triangles;
            continue;
        }
        if(!lodSelection.IsSelected(draw)) continue;
        if (!draw.vertexBuffer)
            continue;
        // Spatial manipulators are logical scene components. Meshes or
        // materials accidentally attached to their carrier object never act
        // as runtime visualization for portals, matrix links, or warp volumes.
        if (isSpatialManipulatorCarrierDraw(draw))
            continue;
        if (draw.occlusionCandidate &&
            context->IsOccluded(draw.occlusionId))
        {
            ++m_lastOcclusionCulledCount;
            continue;
        }
        ensureRecordsUploaded();
        switchGpuStage(colorGpuStage(draw));
        if (draw.occlusionCandidate)
            context->BeginOcclusionQuery(draw.occlusionId);
        if (draw.pmxOutline && m_pmxOutlinePipeline)
        {
            context->SetPipeline(m_pmxOutlinePipeline.get());
            context->SetConstantBuffer(
                0, m_objectConstantBuffer.get(), draw.constantBufferOffset);
            geometrySubmission.SubmitGeometry(draw);
        }
        context->SetPipeline(geometrySubmission.GeometryPipeline(draw, draw.pipeline));
        context->SetConstantBuffer(
            0, m_objectConstantBuffer.get(), draw.constantBufferOffset);
        for (uint32_t textureSlot = 0;
            textureSlot < std::max(draw.textureSlotCount,
                boundTextureSlotCount); ++textureSlot)
            if (!texturesBound || boundTextures[textureSlot] != draw.textures[textureSlot])
                context->SetTexture(textureSlot, draw.textures[textureSlot]);
        boundTextures = draw.textures;
        texturesBound = true;
        boundTextureSlotCount = draw.textureSlotCount;
        if(draw.mesh && draw.mesh->IsInstanced())
        {
            for(const auto& range:draw.mesh->GetInstanceRanges())
            {
                if(!range.instanceCount || !range.vertexCount)
                    continue;
                ++m_lastOrdinaryDrawCount;
                m_lastRenderCpuTimings.submittedInstances += range.instanceCount;
                m_lastRenderCpuTimings.submittedTriangles +=
                    static_cast<uint64_t>(range.vertexCount / 3u) *
                    range.instanceCount;
            }
        }
        else
        {
            ++m_lastOrdinaryDrawCount;
            const uint32_t primitiveCount = draw.indexBuffer
                ? draw.indexCount : draw.vertexCount;
            m_lastRenderCpuTimings.submittedInstances += 1;
            m_lastRenderCpuTimings.submittedTriangles +=
                primitiveCount / 3u;
        }
        geometrySubmission.SubmitMaterialGeometry(draw, geometrySubmission.GeometryPipeline(draw, draw.pipeline));
        if (draw.occlusionCandidate)
            context->EndOcclusionQuery();

        // Draw selected object outline overlay. Structured buffers remain
        // bound across the pipeline change and do not need rebinding.
        const uint64_t selectedPlacementId = m_selectedPlacementId;
        const auto* instanceData = draw.mesh && draw.mesh->IsInstanced()
            ? draw.mesh->GetInstanceData().get() : nullptr;
        const auto* drawLayer = draw.object
            ? draw.object->GetComponentInParent<
                Engine::Components::TerrainObjectLayer>() : nullptr;
        const bool selectedPlacement = selectedPlacementId != 0 &&
            m_selectedPlacementLayer && drawLayer &&
            drawLayer->Owner == m_selectedPlacementLayer && instanceData &&
            std::find(instanceData->placementIds.begin(),
                instanceData->placementIds.end(), selectedPlacementId) !=
                    instanceData->placementIds.end();
        const bool selectedObject = selectedPlacementId == 0 &&
            (m_editorSelectedMesh ? draw.mesh == m_editorSelectedMesh
                : draw.object == m_selectedObject);
        if (includeEditorVisuals && !draw.preview &&
            draw.mesh != m_editorWeightPaintMesh &&
            (selectedPlacement || selectedObject) && m_objectOutlinePipeline)
        {
            context->SetPipeline(geometrySubmission.GeometryPipeline(
                draw, m_objectOutlinePipeline.get()));
            context->SetConstantBuffer(
                0, m_objectConstantBuffer.get(), draw.constantBufferOffset);
            geometrySubmission.SubmitGeometry(draw, false,
                selectedPlacement ? selectedPlacementId : 0);
        }
    }
    m_lastRenderCpuTimings.submissionMs=std::max(0.,std::chrono::duration<double,std::milli>(RenderClock::now()-submissionStart).count()-(m_lastRenderCpuTimings.uploadMs-uploadBeforeSubmission));
    if (activeGpuStage)
    {
        context->EndGpuTiming(*activeGpuStage);
        activeGpuStage.reset();
    }

    Renderer::ScenePortalRenderingContext portalRendering{
        *this, context, preparedDraws, automaticGroups, cam, view, proj,
        viewProjection, cameraPosition, viewportWidth, viewportHeight,
        portalRenderSlotsPerView, portalObjectSlotCount,
        cameraUsesSourceWarpChart, includeEditorVisuals, mainShadowCascades,
        geometrySubmission, lodSelection, submitAutomaticGroup,
        isSpatialManipulatorCarrierDraw, ensureRecordsUploaded,
        resolvePortalTarget };
    Renderer::ScenePortalRendering::Render(portalRendering);
    const auto retainedUiStart=RenderClock::now();
    // Screen-space retained UI is a game-view pass. The scene editor camera
    // should inspect UI objects in the scene without applying runtime
    // fullscreen composition that anchors to the active viewport.
    if (m_uiRenderer && (!includeEditorVisuals || settings.sceneViewUiOverlay))
        m_uiRenderer->Render(*this, context, aspect);
    updatePortalShadowDebug();
    m_lastRenderCpuTimings.retainedUiMs=std::chrono::duration<double,std::milli>(RenderClock::now()-retainedUiStart).count();
}

}
