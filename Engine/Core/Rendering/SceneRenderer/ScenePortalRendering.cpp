#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/SceneRenderer/ScenePortalRendering.h"
#include "Core/Rendering/SceneRenderer/ScenePortalViewScheduling.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Components/Physics/SpatialManipulator.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Rendering/Portal/PortalRenderPolicy.h"
#include "Core/Rendering/TextureFootprint.h"
#include "Core/Rendering/SceneRenderer/SceneRendererLighting.h"
#include "Core/Rendering/RenderUtilities.h"
#include "Core/Renderers/UIRenderer.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <optional>
#if defined(_WIN32)
#include "Core/Renderers/DX11/DX11GraphicsProvider.h"
#include "Core/Renderers/DX12/DX12GraphicsProvider.h"
#include <d3d11.h>
#include <wrl/client.h>
#endif
#if defined(ENGINE_VULKAN_ENABLED)
#include "Core/Renderers/Vulkan/VulkanGraphicsProvider.h"
#endif

namespace Engine::Rendering
{
using Scene = Engine::Scene::Scene;
namespace
{
struct PortalGridCBData
{
    glm::mat4 invVP;
    glm::vec3 cameraPos;
    float cellSize;
    glm::vec4 gridColor;
    glm::vec4 axisColor;
    float fadeDistance;
    float nearPlane;
    float farPlane;
    float mode2D;
};
static_assert(sizeof(PortalGridCBData) == 128);
}

void ScenePortalRendering::Render(ScenePortalRenderingContext& input)
{
    SceneRenderer& renderer = input.renderer;
    auto& scene = renderer.GetScene();
    auto* context = input.graphics;
    auto& preparedDraws = input.preparedDraws;
    const auto& automaticGroups = input.automaticGroups;
    auto* cam = input.camera;
    const glm::mat4& view = input.view;
    const glm::mat4& proj = input.projection;
    const glm::mat4& viewProjection = input.viewProjection;
    const glm::vec3& cameraPosition = input.cameraPosition;
    const uint32_t viewportWidth = input.viewportWidth;
    const uint32_t viewportHeight = input.viewportHeight;
    const uint32_t portalRenderSlotsPerView = input.portalRenderSlotsPerView;
    const uint32_t portalObjectSlotCount = input.portalObjectSlotCount;
    const bool cameraUsesSourceWarpChart = input.cameraUsesSourceWarpChart;
    const bool includeEditorVisuals = input.includeEditorVisuals;
    const auto& mainShadowCascades = input.mainShadowCascades;
    auto& geometrySubmission = input.geometrySubmission;
    auto& lodSelection = input.lodSelection;
    const auto& submitAutomaticGroup = input.submitAutomaticGroup;
    const auto& isSpatialManipulatorCarrierDraw = input.isSpatialManipulatorCarrierDraw;
    const auto& ensureRecordsUploaded = input.ensureRecordsUploaded;
    const auto& resolvePortalTarget = input.resolvePortalTarget;
    auto& settings = scene.settings;
    const auto* skybox = renderer.ResolveSkyboxTexture();
    using RenderClock = std::chrono::steady_clock;
    using PortalShadowCacheEntry = SceneRenderer::PortalShadowCacheEntry;
    ensureRecordsUploaded();
    const auto portalWorkStart=RenderClock::now();
    auto* portalVertices = static_cast<Engine::Model::AnimationVertex*>(renderer.m_portalApertureMapped);
    const uint32_t portalVertexCapacity = SceneRenderer::kMaxSpatialObjects * SceneRenderer::kMaxSpatialVerticesPerObject;
    auto portalPreparation = PortalPassPreparation::Build(
        renderer.m_frameSpatialManipulatorScratch, portalVertices, portalVertexCapacity,
        view, proj, cameraPosition, cameraUsesSourceWarpChart, resolvePortalTarget);
    auto& portalPasses = portalPreparation.passes;
    const auto activePortalFrame = [&](const Engine::Components::SpatialManipulator& portal)
    {
        return cameraUsesSourceWarpChart ? portal.GetPortalWorldFrame()
            : portal.GetRenderPortalWorldFrame();
    };
    uint32_t portalVertexCursor = portalPreparation.vertexCursor;
    std::vector<SpatialDebugPass> spatialDebugPasses;
    if (includeEditorVisuals && settings.portalDebugVisuals)
        spatialDebugPasses = SpatialDebugGeometry::Build(
            portalPasses, renderer.m_frameSpatialManipulatorScratch, portalVertices,
            portalVertexCursor, portalVertexCapacity, cameraUsesSourceWarpChart);
    if (portalVertexCursor > 0)
        renderer.m_portalApertureBuffer->FlushMappedWrites();

    auto portalViewJobs = ScenePortalViewScheduling::Build(renderer, portalPasses, view, proj, cam, cameraUsesSourceWarpChart);
    auto& viewVisibility = renderer.m_lastRenderCpuTimings.cameraVisibility;
    for (size_t jobIndex=0; jobIndex<portalViewJobs.size(); ++jobIndex)
    {
        const auto* source=portalViewJobs[jobIndex].portal
            ? portalViewJobs[jobIndex].portal->source : nullptr;
        const uint64_t sourceId=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(source));
        const uint64_t identity=(sourceId>>4u) ^ (uint64_t(jobIndex+1u)<<48u) ^
            (uint64_t(portalViewJobs[jobIndex].depth)<<40u);
        viewVisibility.viewIdentities.push_back({identity,true,false});
    }
    const auto apertureScissorInView = [&](const PortalStencilPass& pass,
        const glm::mat4& candidateView)
    {
        return ScenePortalViewScheduling::ApertureScissor(pass,
            candidateView, proj, viewportWidth, viewportHeight, cameraUsesSourceWarpChart);
    };

    for (PortalShadowCacheEntry& entry : renderer.m_portalShadowCache)
        if (entry.ownerCamera == cam)
            entry.selected = false;
    ++renderer.m_portalShadowGeneration;
    for (auto iterator = renderer.m_portalShadowCache.begin();
        iterator != renderer.m_portalShadowCache.end();)
    {
        const bool stale = renderer.m_portalShadowGeneration >
            iterator->touchedGeneration + 120u;
        if (!stale)
        {
            ++iterator;
            continue;
        }
        if (iterator->constantBuffer && iterator->constantMapped)
            iterator->constantBuffer->Unmap();
        if (iterator->objectBuffer && iterator->objectMapped)
            iterator->objectBuffer->Unmap();
        if (iterator->constantBuffer)
            renderer.RetiredSceneBuffers().push_back(
                std::move(iterator->constantBuffer));
        if (iterator->objectBuffer)
            renderer.RetiredSceneBuffers().push_back(
                std::move(iterator->objectBuffer));
        if (iterator->atlas)
            renderer.m_retiredShadowTextures.push_back(std::move(iterator->atlas));
        iterator = renderer.m_portalShadowCache.erase(iterator);
    }
    if (renderer.m_realtimeShadowSettings.portalPolicy !=
        Engine::Model::PortalShadowPolicy::ReuseMain)
    {
        std::vector<size_t> selectedJobs(portalViewJobs.size());
        std::iota(selectedJobs.begin(), selectedJobs.end(), 0u);
        std::stable_sort(selectedJobs.begin(), selectedJobs.end(),
            [&](size_t leftIndex, size_t rightIndex)
            {
                const PortalViewJob& left = portalViewJobs[leftIndex];
                const PortalViewJob& right = portalViewJobs[rightIndex];
                if (left.depth != right.depth)
                    return left.depth < right.depth;
                if (std::abs(left.screenArea - right.screenArea) > 1e-6f)
                    return left.screenArea > right.screenArea;
                return left.shadowKey < right.shadowKey;
            });
        if (renderer.m_realtimeShadowSettings.portalPolicy ==
            Engine::Model::PortalShadowPolicy::Budgeted)
        {
            selectedJobs.resize(std::min(selectedJobs.size(),
                static_cast<size_t>(std::clamp(
                    renderer.m_realtimeShadowSettings.portalViewBudget, 1u, 64u))));
        }
        for (const size_t jobIndex : selectedJobs)
        {
            PortalViewJob& job = portalViewJobs[jobIndex];
            auto found = std::find_if(renderer.m_portalShadowCache.begin(),
                renderer.m_portalShadowCache.end(), [&](const PortalShadowCacheEntry& entry)
                {
                    return entry.ownerCamera == cam && entry.key == job.shadowKey;
                });
            if (found == renderer.m_portalShadowCache.end())
            {
                renderer.m_portalShadowCache.emplace_back();
                found = std::prev(renderer.m_portalShadowCache.end());
                found->key = job.shadowKey;
                found->ownerCamera = cam;
            }
            found->targetChart = job.portal->target;
            found->contentScopeRoot = job.contentScopeRoot;
            found->view = job.mappedView;
            found->cameraPosition = job.mappedCameraPosition;
            found->depth = job.depth;
            found->screenArea = job.screenArea;
            found->touchedGeneration = renderer.m_portalShadowGeneration;
            found->selected = true;
            job.shadowCacheIndex = static_cast<size_t>(
                std::distance(renderer.m_portalShadowCache.begin(), found));
        }
    }

    const auto connectedSpaceClipPlane = [&activePortalFrame](
        const PortalStencilPass& portalPass,
        const glm::vec3& mappedCameraPosition)
    {
        const glm::mat4 targetFrame = activePortalFrame(*portalPass.target);
        const glm::vec3 clipPoint(targetFrame[3]);
        glm::vec3 clipNormal = glm::normalize(glm::vec3(targetFrame[2]));
        // Keep the half-space beyond the exit aperture. The earlier version
        // pointed this normal toward the virtual camera, retaining the space
        // behind the portal and clipping the floor/scene the opening was
        // supposed to reveal.
        if (glm::dot(mappedCameraPosition - clipPoint, clipNormal) > 0.f)
            clipNormal = -clipNormal;
        return glm::vec4(clipNormal,
            -glm::dot(clipNormal, clipPoint) - 0.0005f);
    };

    // Populate every recorded portal draw before issuing any of them. DX11
    // copies these values as draws are submitted, while DX12/Vulkan retain GPU
    // addresses until command execution; unique ranges satisfy both models.
    for (size_t jobIndex = 0; jobIndex < portalViewJobs.size(); ++jobIndex)
    {
        PortalViewJob& portalJob = portalViewJobs[jobIndex];
        const PortalStencilPass& portalPass = *portalJob.portal;
        portalJob.dataSlotBase = renderer.m_objectCapacity +
            static_cast<uint32_t>(jobIndex) * portalRenderSlotsPerView;
        portalJob.objectDataSlotBase =
            static_cast<uint32_t>(jobIndex) * portalRenderSlotsPerView;
        portalJob.skyboxConstantBufferOffset =
            static_cast<uint64_t>(jobIndex + 1u) * SceneRenderer::kCBStride;

        ObjectGPUData apertureData = portalPass.apertureObjectData;
        apertureData.viewProjection = proj * portalJob.apertureView;
        const uint32_t maskSlot = portalJob.dataSlotBase;
        const uint32_t resetSlot = maskSlot + 1u;
        const uint32_t maskObjectSlot = portalJob.objectDataSlotBase;
        const uint32_t resetObjectSlot = maskObjectSlot + 1u;
        memcpy(static_cast<uint8_t*>(renderer.m_portalObjectDataMapped) +
            static_cast<size_t>(maskObjectSlot) * sizeof(ObjectGPUData),
            &apertureData, sizeof(apertureData));
        memcpy(static_cast<uint8_t*>(renderer.m_portalObjectDataMapped) +
            static_cast<size_t>(resetObjectSlot) * sizeof(ObjectGPUData),
            &apertureData, sizeof(apertureData));

        DrawCBData maskDraw = portalPass.apertureDrawData;
        maskDraw.objectIndex = maskObjectSlot;
        DrawCBData resetDraw = portalPass.apertureDrawData;
        resetDraw.objectIndex = resetObjectSlot;
        resetDraw.flags |= kPortalDepthResetFlag;
        memcpy(static_cast<uint8_t*>(renderer.m_objectCBMapped) +
            static_cast<size_t>(maskSlot) * SceneRenderer::kCBStride,
            &maskDraw, sizeof(maskDraw));
        memcpy(static_cast<uint8_t*>(renderer.m_objectCBMapped) +
            static_cast<size_t>(resetSlot) * SceneRenderer::kCBStride,
            &resetDraw, sizeof(resetDraw));

        const glm::vec4 portalClipPlane = connectedSpaceClipPlane(
            portalPass, portalJob.mappedCameraPosition);
        for (const PreparedDraw& draw : preparedDraws)
        {
            if (!Scene::IsObjectInSpatialRegion(draw.object,
                portalJob.contentScopeRoot))
                continue;
            if (!Engine::Rendering::Portal::
                IsTraversalInstanceVisibleInConnectedChart(
                    draw.traversalChartPortal, portalPass.target))
                continue;
            ObjectGPUData mappedData = draw.objectData;
            mappedData.viewProjection = proj * portalJob.mappedView;
            mappedData.sphereView = portalJob.mappedView;
            mappedData.portalClipPlane = portalClipPlane;
            mappedData.viewPositionAlphaCutoff.x =
                portalJob.mappedCameraPosition.x;
            mappedData.viewPositionAlphaCutoff.y =
                portalJob.mappedCameraPosition.y;
            mappedData.viewPositionAlphaCutoff.z =
                portalJob.mappedCameraPosition.z;
            if (portalJob.shadowCacheIndex < renderer.m_portalShadowCache.size())
            {
                const PortalShadowCacheEntry& portalShadow =
                    renderer.m_portalShadowCache[portalJob.shadowCacheIndex];
                if (portalShadow.rendered && portalShadow.atlas)
                {
                    const float shadowDistanceScale = mainShadowCascades.data.w > 0.f
                        ? std::clamp(mappedData.shadowCascadeData.w /
                            mainShadowCascades.data.w, 0.f, 1.f)
                        : 1.f;
                    std::copy(portalShadow.viewProjections.begin(),
                        portalShadow.viewProjections.end(),
                        mappedData.shadowViewProjections);
                    mappedData.shadowCascadeSplits = portalShadow.cascadeSplits;
                    mappedData.shadowCascadeData = portalShadow.cascadeData;
                    mappedData.shadowCascadeData.w *= shadowDistanceScale;
                    mappedData.shadowCameraData = portalShadow.cameraData;
                }
            }
            if (includeEditorVisuals && settings.portalDebugVisuals &&
                settings.portalDebugTintRemoteView)
            {
                mappedData.baseColor = glm::vec4(
                    mappedData.baseColor.r * 0.35f,
                    mappedData.baseColor.g * 0.70f,
                    mappedData.baseColor.b * 1.15f,
                    mappedData.baseColor.a);
                mappedData.emissiveOcclusion.x += 0.05f;
                mappedData.emissiveOcclusion.y += 0.15f;
                mappedData.emissiveOcclusion.z += 0.2f;
            }

            const uint32_t drawSlot = portalJob.dataSlotBase + 2u +
                draw.drawData.objectIndex;
            const uint32_t objectDrawSlot = portalJob.objectDataSlotBase + 2u +
                draw.drawData.objectIndex;
            DrawCBData mappedDraw = draw.drawData;
            mappedDraw.objectIndex = objectDrawSlot;
            mappedDraw.flags &= ~kClusteredLightingFlag;
            memcpy(static_cast<uint8_t*>(renderer.m_objectCBMapped) +
                static_cast<size_t>(drawSlot) * SceneRenderer::kCBStride,
                &mappedDraw, sizeof(mappedDraw));
            memcpy(static_cast<uint8_t*>(renderer.m_portalObjectDataMapped) +
                static_cast<size_t>(objectDrawSlot) * sizeof(ObjectGPUData),
                &mappedData, sizeof(mappedData));
        }

        ViewLighting::SkyboxConstants skyboxData{};
        ViewLighting::SetSkyboxRays(skyboxData, proj,
            glm::mat4(glm::mat3(portalJob.mappedView)));
        const bool physicalSky = skybox && skybox == renderer.m_physicalSky.Panorama().get();
        skyboxData.displayParams = {
            scene.IsMode2D() ? 1.f : 0.f,
            physicalSky ? settings.physicalSky.skyIntensity : std::exp2(settings.hdriExposure),
            physicalSky ? 0.f : glm::radians(settings.hdriRotation),
            // The physical panorama already holds the Mie aureole.
            physicalSky ? 0.f : std::max(settings.sunGlowIntensity, 0.f) };
        skyboxData.sunDirection = renderer.m_skySunDirection;
        skyboxData.sunRadiance = renderer.m_skySunRadiance;
        // Fogged like the main sky, from the connected view's eye: the
        // portal's objects already fog with the same view terms.
        if (!scene.IsMode2D())
            ViewLighting::SetSkyboxFog(skyboxData, renderer.m_viewAtmosphereFog,
                settings.atmosphereFog.skyDistance,
                float(glm::inverse(portalJob.mappedView)[3].y));
        memcpy(static_cast<uint8_t*>(renderer.m_skyboxCBMapped) +
            portalJob.skyboxConstantBufferOffset,
            &skyboxData, sizeof(skyboxData));
    }
    const uint32_t debugSlotBase = renderer.m_objectCapacity;
    const uint32_t debugConstantBufferSlotBase = renderer.m_objectCapacity +
        portalObjectSlotCount;
    const float debugAlpha = std::clamp(
        settings.portalDebugOverlayAlpha, 0.f, 1.f);
    for (size_t debugIndex = 0; debugIndex < spatialDebugPasses.size() &&
        debugIndex < SceneRenderer::kMaxSpatialDebugDraws; ++debugIndex)
    {
        SpatialDebugPass& pass = spatialDebugPasses[debugIndex];
        pass.dataSlot = debugSlotBase + static_cast<uint32_t>(debugIndex);
        pass.constantBufferSlot = debugConstantBufferSlotBase +
            static_cast<uint32_t>(debugIndex);
        const DrawCBData debugDraw { pass.dataSlot, 0u, 0u, 0u };
        ObjectGPUData debugData{};
        debugData.world = glm::mat4(1.f);
        debugData.viewProjection = proj * view;
        debugData.baseColor = glm::vec4(glm::vec3(pass.color), debugAlpha);
        debugData.ambientUnlit = { 0.f, 0.f, 0.f, 1.f };
        debugData.emissiveOcclusion = glm::vec4(
            glm::vec3(pass.color) * 0.2f, 1.f);
        debugData.materialParams = { 0.f, 1.f, 1.f, 0.f };
        debugData.viewPositionAlphaCutoff =
            glm::vec4(cameraPosition, 0.001f);
        memcpy(static_cast<uint8_t*>(renderer.m_objectCBMapped) +
            static_cast<size_t>(pass.constantBufferSlot) * SceneRenderer::kCBStride,
            &debugDraw, sizeof(debugDraw));
        memcpy(static_cast<uint8_t*>(renderer.m_objectDataMapped) +
            static_cast<size_t>(pass.dataSlot) * sizeof(ObjectGPUData),
            &debugData, sizeof(debugData));
        renderer.m_objectUploadCache.Invalidate(pass.dataSlot);
    }
    if (!portalViewJobs.empty())
    {
        const uint64_t portalRecords =
            static_cast<uint64_t>(portalViewJobs.size() - 1u) *
                portalRenderSlotsPerView + 2u + preparedDraws.size();
        const uint64_t portalBytes = portalRecords * sizeof(ObjectGPUData);
        renderer.m_portalObjectDataBuffer->FlushMappedWrites(0, portalBytes);
        renderer.m_lastObjectDataUploadBytes += portalBytes;
        renderer.RecordFrameUploadBytes(portalBytes);
        // Aperture-mask draws are the first portal commands and also index
        // portal object data, so switch before issuing any portal work.
        context->SetStructuredBuffer(7, renderer.m_portalObjectDataBuffer.get());
    }
    if (!spatialDebugPasses.empty())
    {
        // D3D11's discard upload replaces the buffer contents, so preserve
        // the ordinary prefix when editor diagnostics occupy the tail.
        const uint64_t debugRecords = static_cast<uint64_t>(renderer.m_objectCapacity) +
            std::min(spatialDebugPasses.size(),
                static_cast<size_t>(SceneRenderer::kMaxSpatialDebugDraws));
        const uint64_t debugBytes = debugRecords * sizeof(ObjectGPUData);
        renderer.m_objectDataBuffer->FlushMappedWrites(0, debugBytes);
        renderer.m_lastObjectDataUploadBytes += debugBytes;
        renderer.RecordFrameUploadBytes(debugBytes);
    }

    const auto drawConnectedSkybox = [&](const PortalViewJob& portalJob)
    {
        // A scoped spatial region owns only its explicit hierarchy. Falling
        // back to the scene-global skybox would leak content from outside the
        // connected chart and hide missing region content.
        if (portalJob.contentScopeRoot)
            return;
        if (!skybox || !skybox->GetGraphicsTexture() ||
            !renderer.m_portalSkyboxStencilReadPipeline)
            return;
        context->SetPipeline(renderer.m_portalSkyboxStencilReadPipeline.get());
        context->SetConstantBuffer(0, renderer.m_skyboxConstantBuffer.get(),
            portalJob.skyboxConstantBufferOffset);
        context->SetTexture(0, skybox->GetGraphicsTexture());
        context->DrawInstanced(3, 1, 0, 0);
    };
    const auto portalTexture = [&](const PortalViewJob& portalJob,
        const PreparedDraw& draw, uint32_t textureSlot)
        -> const Engine::Graphics::IGraphicsTexture*
    {
        if (textureSlot == 7u &&
            portalJob.shadowCacheIndex < renderer.m_portalShadowCache.size())
        {
            const PortalShadowCacheEntry& shadow =
                renderer.m_portalShadowCache[portalJob.shadowCacheIndex];
            if (shadow.rendered && shadow.atlas)
                return shadow.atlas.get();
        }
        return draw.textures[textureSlot];
    };

// Retained only as a diagnostic fallback for older drivers. Normal builds use
// the unified graphics-pipeline path below for DX11, DX12, and Vulkan.
#if defined(_WIN32) && defined(ENGINE_USE_LEGACY_DX11_PORTAL_PATH)
    // Render linked-space view through portal aperture with a stencil mask.
    if (dynamic_cast<Engine::Renderers::D3D11GraphicsProvider*>(renderer.m_graphicsProvider))
    {
        ID3D11DeviceContext* dx11Context =
            static_cast<ID3D11DeviceContext*>(context->GetNativeHandle());
        if (dx11Context)
        {
            if (!portalViewJobs.empty())
            {
                ID3D11Device* dx11Device = nullptr;
                dx11Context->GetDevice(&dx11Device);
                if (dx11Device)
                {
                    static Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
                        stencilWriteState;
                    static Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
                        stencilIncrementState;
                    static Microsoft::WRL::ComPtr<ID3D11DepthStencilState>
                        stencilReadState;
                    static Microsoft::WRL::ComPtr<ID3D11BlendState>
                        colorMaskOffState;

                    if (!stencilWriteState)
                    {
                        D3D11_DEPTH_STENCIL_DESC descriptor{};
                        descriptor.DepthEnable = TRUE;
                        descriptor.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
                        descriptor.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; // reverse-Z
                        descriptor.StencilEnable = TRUE;
                        descriptor.StencilReadMask = 0xFF;
                        descriptor.StencilWriteMask = 0xFF;
                        descriptor.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
                        descriptor.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
                        descriptor.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.BackFace = descriptor.FrontFace;
                        dx11Device->CreateDepthStencilState(
                            &descriptor, &stencilWriteState);
                    }
                    if (!stencilReadState)
                    {
                        D3D11_DEPTH_STENCIL_DESC descriptor{};
                        descriptor.DepthEnable = FALSE;
                        descriptor.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
                        descriptor.DepthFunc = D3D11_COMPARISON_ALWAYS;
                        descriptor.StencilEnable = TRUE;
                        descriptor.StencilReadMask = 0xFF;
                        descriptor.StencilWriteMask = 0x00;
                        descriptor.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
                        descriptor.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.BackFace = descriptor.FrontFace;
                        dx11Device->CreateDepthStencilState(
                            &descriptor, &stencilReadState);
                    }
                    if (!stencilIncrementState)
                    {
                        D3D11_DEPTH_STENCIL_DESC descriptor{};
                        descriptor.DepthEnable = TRUE;
                        descriptor.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
                        descriptor.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; // reverse-Z
                        descriptor.StencilEnable = TRUE;
                        descriptor.StencilReadMask = 0xFF;
                        descriptor.StencilWriteMask = 0xFF;
                        descriptor.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
                        descriptor.FrontFace.StencilPassOp = D3D11_STENCIL_OP_INCR_SAT;
                        descriptor.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
                        descriptor.BackFace = descriptor.FrontFace;
                        dx11Device->CreateDepthStencilState(
                            &descriptor, &stencilIncrementState);
                    }
                    if (!colorMaskOffState)
                    {
                        D3D11_BLEND_DESC descriptor{};
                        descriptor.RenderTarget[0].BlendEnable = FALSE;
                        descriptor.RenderTarget[0].RenderTargetWriteMask = 0;
                        dx11Device->CreateBlendState(&descriptor,
                            &colorMaskOffState);
                    }
                    dx11Device->Release();

                    if (stencilWriteState && stencilIncrementState &&
                        stencilReadState && colorMaskOffState)
                    {
                        const float blendFactor[4]{};
                        for (const PortalViewJob& portalJob : portalViewJobs)
                        {
                        const PortalStencilPass& portalPass = *portalJob.portal;
                        if (portalPass.apertureVertexCount < 3)
                            continue;
                        const auto stencilStep = Engine::Rendering::Portal::
                            StencilForDepth(
                                Engine::Rendering::Portal::Backend::DirectX11,
                                portalJob.depth, portalJob.rootStencilBase);
                        const UINT portalStencilRef = stencilStep.sceneReadReference;

                        context->SetPipeline(renderer.m_objectDoubleSidedPipeline.get());
                        context->SetConstantBuffer(
                            0, renderer.m_objectConstantBuffer.get(),
                            static_cast<uint64_t>(portalJob.dataSlotBase) *
                                SceneRenderer::kCBStride);
                        context->SetVertexBuffer(
                            0, renderer.m_portalApertureBuffer.get(),
                            sizeof(Engine::Model::AnimationVertex),
                            portalPass.apertureVertexOffset);
                        dx11Context->OMSetBlendState(colorMaskOffState.Get(),
                            blendFactor, UINT_MAX);
                        dx11Context->OMSetDepthStencilState(
                            stencilStep.writeOperation == Engine::Rendering::
                                Portal::ApertureWriteOperation::Replace
                                ? stencilWriteState.Get()
                                : stencilIncrementState.Get(),
                            stencilStep.writeReference);
                        context->DrawInstanced(
                            portalPass.apertureVertexCount, 1, 0, 0);

                        context->SetStructuredBuffer(6, renderer.m_lightDataBuffer.get());
                        context->SetStructuredBuffer(7,
                            renderer.m_portalObjectDataBuffer.get());
                        context->SetStructuredBuffer(8, renderer.m_boneDataBuffer.get());

                        for (const PreparedDraw& draw : preparedDraws)
                        {
                            if (!draw.object || !draw.vertexBuffer)
                                continue;
                            if (isSpatialManipulatorCarrierDraw(draw))
                                continue;
                            if (!Scene::IsObjectInSpatialRegion(draw.object,
                                portalJob.contentScopeRoot))
                                continue;
                            if (!Engine::Rendering::Portal::
                                IsTraversalInstanceVisibleInConnectedChart(
                                    draw.traversalChartPortal,
                                    portalPass.target))
                                continue;

                            context->SetPipeline(geometrySubmission.GeometryPipeline(
                                draw, draw.pipeline));
                            const uint32_t drawSlot = portalJob.dataSlotBase +
                                2u + draw.drawData.objectIndex;
                            context->SetConstantBuffer(
                                0, renderer.m_objectConstantBuffer.get(),
                                static_cast<uint64_t>(drawSlot) * SceneRenderer::kCBStride);
                            for (uint32_t textureSlot = 0;
                                textureSlot < draw.textureSlotCount; ++textureSlot)
                            {
                                context->SetTexture(textureSlot,
                                    portalTexture(portalJob, draw, textureSlot));
                            }
                            geometrySubmission.SubmitMaterialGeometry(draw, geometrySubmission.GeometryPipeline(draw, draw.pipeline), [&]
                            {
                                dx11Context->OMSetDepthStencilState(
                                    stencilReadState.Get(), portalStencilRef);
                            });
                        }
                        }

                        dx11Context->OMSetDepthStencilState(nullptr, 0);
                        dx11Context->OMSetBlendState(nullptr, blendFactor,
                            UINT_MAX);
                    }
                }
            }
        }
    }
#endif

#if defined(_WIN32) || defined(ENGINE_VULKAN_ENABLED)
    if (!portalViewJobs.empty())
        context->BeginGpuTiming(Engine::Graphics::GpuTimingStage::Portal);
#if defined(_WIN32)
    const bool isDx11Provider =
        dynamic_cast<Engine::Renderers::D3D11GraphicsProvider*>(renderer.m_graphicsProvider) != nullptr;
#else
    const bool isDx11Provider = false;
#endif
    const bool isDx12Provider =
        dynamic_cast<Engine::Renderers::D3D12GraphicsProvider*>(renderer.m_graphicsProvider) != nullptr;
#if defined(ENGINE_VULKAN_ENABLED)
    const bool isVulkanProvider =
        dynamic_cast<Engine::Renderers::VulkanGraphicsProvider*>(renderer.m_graphicsProvider) != nullptr;
#else
    const bool isVulkanProvider = false;
#endif

    if ((isDx11Provider || isDx12Provider
#if defined(ENGINE_VULKAN_ENABLED)
            || isVulkanProvider
#endif
        ) && renderer.m_objectPortalStencilWritePipeline &&
            renderer.m_objectPortalStencilIncrementPipeline &&
            renderer.m_objectPortalDepthResetPipeline)
    {
        auto resolveStencilReadPipeline =
            [&](Engine::Graphics::IPipelineState* base)
        {
            // Depth-offset parallax falls back to flat depth inside portals.
            if (base == renderer.m_objectPipeline.get() ||
                base == renderer.m_objectParallaxDepthPipelines[0].get())
                return renderer.m_objectPortalStencilReadPipeline.get();
            if (base == renderer.m_objectParallaxDepthPipelines[1].get())
                return renderer.m_objectPortalStencilReadDoubleSidedPipeline.get();
            if (base == renderer.m_objectDoubleSidedPipeline.get())
                return renderer.m_objectPortalStencilReadDoubleSidedPipeline.get();
            if (base == renderer.m_objectBlendPipeline.get())
                return renderer.m_objectPortalStencilReadBlendPipeline.get();
            if (base == renderer.m_objectBlendDoubleSidedPipeline.get())
                return renderer.m_objectPortalStencilReadBlendDoubleSidedPipeline.get();
            if (base == renderer.m_objectWirePipeline.get())
                return renderer.m_objectPortalStencilReadWirePipeline.get();
            if (base == renderer.m_objectWireDoubleSidedPipeline.get())
                return renderer.m_objectPortalStencilReadWireDoubleSidedPipeline.get();
            if (base == renderer.m_objectBlendWirePipeline.get())
                return renderer.m_objectPortalStencilReadBlendWirePipeline.get();
            if (base == renderer.m_objectBlendWireDoubleSidedPipeline.get())
                return renderer.m_objectPortalStencilReadBlendWireDoubleSidedPipeline.get();
            if (base == renderer.m_objectPreviewPipeline.get())
                return renderer.m_objectPortalStencilReadPreviewPipeline.get();
            if (base == renderer.m_objectPreviewDoubleSidedPipeline.get())
                return renderer.m_objectPortalStencilReadPreviewDoubleSidedPipeline.get();
            if (base == renderer.m_objectPreviewWirePipeline.get())
                return renderer.m_objectPortalStencilReadPreviewWirePipeline.get();
            if (base == renderer.m_objectPreviewWireDoubleSidedPipeline.get())
                return renderer.m_objectPortalStencilReadPreviewWireDoubleSidedPipeline.get();
            return base;
        };

        if (!portalViewJobs.empty())
        {
            // Establish every top-level aperture while the depth buffer still
            // contains only the local scene.  Interleaving root-mask writes
            // with connected-scene draws lets geometry rendered through an
            // earlier portal replace local depth.  In folded layouts (such as
            // the three-room matrix ring), a portal that was originally
            // occluded can then pass the depth test as a second root and
            // overwrite the legitimate recursive view.
            //
            // portalPasses are ordered far-to-near, so overlapping roots keep
            // the nearest local aperture's disjoint stencil range.
            bool usedRootScissor = false;
            for (const PortalViewJob& portalJob : portalViewJobs)
            {
                if (portalJob.depth != 0u)
                    continue;
                const PortalStencilPass& portalPass = *portalJob.portal;
                if (portalPass.apertureVertexCount < 3)
                    continue;

                const auto rootScissor = apertureScissorInView(portalPass,
                    portalJob.apertureView);
                if (rootScissor)
                {
                    context->SetScissorRect(*rootScissor);
                    usedRootScissor = true;
                }
                const auto portalBackend = isDx11Provider
                    ? Engine::Rendering::Portal::Backend::DirectX11
                    : (isDx12Provider
                        ? Engine::Rendering::Portal::Backend::DirectX12
                        : Engine::Rendering::Portal::Backend::Vulkan);
                const auto stencilStep = Engine::Rendering::Portal::
                    StencilForDepth(portalBackend, portalJob.depth,
                        portalJob.rootStencilBase);
                context->SetPipeline(renderer.m_objectPortalStencilWritePipeline.get());
                context->SetStencilReference(stencilStep.writeReference);
                context->SetConstantBuffer(
                    0, renderer.m_objectConstantBuffer.get(),
                    static_cast<uint64_t>(portalJob.dataSlotBase) * SceneRenderer::kCBStride);
                context->SetVertexBuffer(
                    0, renderer.m_portalApertureBuffer.get(),
                    sizeof(Engine::Model::AnimationVertex),
                    portalPass.apertureVertexOffset);
                context->DrawInstanced(
                    portalPass.apertureVertexCount, 1, 0, 0);
            }
            if (usedRootScissor)
            {
                context->SetScissorRect({ 0, 0,
                    static_cast<int32_t>(viewportWidth),
                    static_cast<int32_t>(viewportHeight) });
            }

            for (const PortalViewJob& portalJob : portalViewJobs)
            {
            const PortalStencilPass& portalPass = *portalJob.portal;
            if (portalPass.apertureVertexCount < 3)
                continue;
            const auto portalScissor = apertureScissorInView(portalPass,
                portalJob.apertureView);
            if (portalScissor)
                context->SetScissorRect(*portalScissor);
            const auto portalBackend = isDx11Provider
                ? Engine::Rendering::Portal::Backend::DirectX11
                : (isDx12Provider
                    ? Engine::Rendering::Portal::Backend::DirectX12
                    : Engine::Rendering::Portal::Backend::Vulkan);
            const auto stencilStep = Engine::Rendering::Portal::
                StencilForDepth(portalBackend, portalJob.depth,
                    portalJob.rootStencilBase);
            // Root apertures were written together against untouched local
            // depth above. Descendants alone modify stencil here, constrained
            // by their inherited root range and the parent connected view.
            if (portalJob.depth > 0u)
            {
                context->SetPipeline(renderer.m_objectPortalStencilIncrementPipeline.get());
                context->SetStencilReference(stencilStep.writeReference);
                context->SetConstantBuffer(
                    0, renderer.m_objectConstantBuffer.get(),
                    static_cast<uint64_t>(portalJob.dataSlotBase) * SceneRenderer::kCBStride);
                context->SetVertexBuffer(
                    0, renderer.m_portalApertureBuffer.get(),
                    sizeof(Engine::Model::AnimationVertex),
                    portalPass.apertureVertexOffset);
                context->DrawInstanced(portalPass.apertureVertexCount, 1, 0, 0);
            }
            context->SetStencilReference(stencilStep.sceneReadReference);

            context->SetPipeline(renderer.m_objectPortalDepthResetPipeline.get());
            context->SetConstantBuffer(
                0, renderer.m_objectConstantBuffer.get(),
                static_cast<uint64_t>(portalJob.dataSlotBase + 1u) *
                    SceneRenderer::kCBStride);
            // Root masks are established in a separate pre-pass, so the
            // currently bound aperture can belong to another root. Always
            // rebind the matching geometry before resetting depth; otherwise
            // a different portal's polygon punches skybox-coloured wedges
            // through local and connected geometry near the aperture edge.
            context->SetVertexBuffer(
                0, renderer.m_portalApertureBuffer.get(),
                sizeof(Engine::Model::AnimationVertex),
                portalPass.apertureVertexOffset);
            context->DrawInstanced(portalPass.apertureVertexCount, 1, 0, 0);

            drawConnectedSkybox(portalJob);

            context->SetStructuredBuffer(6, renderer.m_lightDataBuffer.get());
            context->SetStructuredBuffer(7, renderer.m_portalObjectDataBuffer.get());
            context->SetStructuredBuffer(8, renderer.m_boneDataBuffer.get());

            for (const PreparedDraw& draw : preparedDraws)
            {
                if (!draw.object || !draw.vertexBuffer)
                    continue;
                if (isSpatialManipulatorCarrierDraw(draw))
                    continue;
                if (!Scene::IsObjectInSpatialRegion(draw.object,
                    portalJob.contentScopeRoot))
                    continue;
                if (!Engine::Rendering::Portal::
                    IsTraversalInstanceVisibleInConnectedChart(
                        draw.traversalChartPortal, portalPass.target))
                    continue;

                context->SetPipeline(geometrySubmission.GeometryPipeline(draw,
                    resolveStencilReadPipeline(draw.pipeline)));
                const uint32_t drawSlot = portalJob.dataSlotBase + 2u +
                    draw.drawData.objectIndex;
                context->SetConstantBuffer(
                    0, renderer.m_objectConstantBuffer.get(),
                    static_cast<uint64_t>(drawSlot) * SceneRenderer::kCBStride);
                for (uint32_t textureSlot = 0;
                    textureSlot < draw.textureSlotCount; ++textureSlot)
                {
                    context->SetTexture(textureSlot,
                        portalTexture(portalJob, draw, textureSlot));
                }
                geometrySubmission.SubmitMaterialGeometry(draw, geometrySubmission.GeometryPipeline(draw,
                    resolveStencilReadPipeline(draw.pipeline)));
            }

            // The connected chart uses the same particle records with its
            // own camera, stencil aperture, content scope, and clip plane.
            context->SetStencilReference(stencilStep.sceneReadReference);
            renderer.RenderParticles(context, proj * portalJob.mappedView,
                portalJob.mappedView, portalJob.contentScopeRoot,
                connectedSpaceClipPlane(portalPass,
                    portalJob.mappedCameraPosition), true);

            if (portalScissor)
            {
                context->SetScissorRect({ 0, 0,
                    static_cast<int32_t>(viewportWidth),
                    static_cast<int32_t>(viewportHeight) });
            }
        }
        }
    }
#endif

    if (!portalViewJobs.empty())
        context->EndGpuTiming(Engine::Graphics::GpuTimingStage::Portal);

    if (!portalPasses.empty())
    {
        for (const PreparedDraw& draw : preparedDraws)
        {
            memcpy(static_cast<uint8_t*>(renderer.m_objectCBMapped) +
                draw.constantBufferOffset,
                &draw.drawData, sizeof(draw.drawData));
            memcpy(static_cast<uint8_t*>(renderer.m_objectDataMapped) +
                static_cast<size_t>(draw.drawData.objectIndex) *
                sizeof(ObjectGPUData),
                &draw.objectData, sizeof(draw.objectData));
            renderer.m_objectUploadCache.Invalidate(draw.drawData.objectIndex);
        }
        const uint64_t restoredRecords = !spatialDebugPasses.empty()
            ? static_cast<uint64_t>(renderer.m_objectCapacity) + std::min(
                spatialDebugPasses.size(),
                static_cast<size_t>(SceneRenderer::kMaxSpatialDebugDraws))
            : static_cast<uint64_t>(renderer.m_renderObjectSlotCount);
        const uint64_t restoredBytes = restoredRecords * sizeof(ObjectGPUData);
        renderer.m_objectDataBuffer->FlushMappedWrites(0, restoredBytes);
        renderer.m_lastObjectDataUploadBytes += restoredBytes;
        renderer.RecordFrameUploadBytes(restoredBytes);
        context->SetStructuredBuffer(6, renderer.m_lightDataBuffer.get());
        context->SetStructuredBuffer(7, renderer.m_objectDataBuffer.get());
        context->SetStructuredBuffer(8, renderer.m_boneDataBuffer.get());
    }

    if (includeEditorVisuals && settings.portalDebugVisuals)
    {
        Engine::Graphics::IPipelineState* portalDebugPipeline =
            settings.portalDebugWireframe
                ? renderer.m_objectSpatialDebugWirePipeline.get()
                : renderer.m_objectSpatialDebugPipeline.get();

        if (portalDebugPipeline)
        {
            // Apertures are deliberately not redrawn here. They already own
            // the stencil-masked portal image; an editor debug fill would
            // compete with that image in depth/blend order. SpatialDebugPass
            // contains only the requested point and line diagnostics.
            for (const SpatialDebugPass& pass : spatialDebugPasses)
            {
                if (pass.vertexCount < 3 || pass.dataSlot ==
                    std::numeric_limits<uint32_t>::max())
                    continue;
                context->SetStructuredBuffer(6, renderer.m_lightDataBuffer.get());
                context->SetStructuredBuffer(7, renderer.m_objectDataBuffer.get());
                context->SetStructuredBuffer(8, renderer.m_boneDataBuffer.get());
                context->SetPipeline(portalDebugPipeline);
                context->SetConstantBuffer(0, renderer.m_objectConstantBuffer.get(),
                    static_cast<uint64_t>(pass.constantBufferSlot) * SceneRenderer::kCBStride);
                context->SetVertexBuffer(0, renderer.m_portalApertureBuffer.get(),
                    sizeof(Engine::Model::AnimationVertex), pass.vertexOffset);
                context->DrawInstanced(pass.vertexCount, 1, 0, 0);
            }
        }
    }

    renderer.RenderParticles(context, proj * view, view);

    // Draw scene helpers (grid) after opaque objects so blending works correctly
    if (includeEditorVisuals && settings.showGrid && renderer.m_gridPipeline)
    {
        // Prepare grid constant buffer data
        PortalGridCBData gridData{};
        // Reconstruct grid rays relative to the camera. Inverting a matrix
        // with a large camera translation loses precision near the horizon.
        gridData.invVP = glm::inverse(proj * glm::mat4(glm::mat3(view)));
        gridData.cameraPos = cameraPosition;
        gridData.cellSize = settings.gridCellSize;
        gridData.gridColor = { settings.gridColor.x, settings.gridColor.y, settings.gridColor.z, settings.gridOpacity };
        // One zoom level for the whole plane keeps the grid's hierarchy
        // stable as the camera moves. The shader uses axisColor.a for it.
        const float gridBaseSpacing = std::max(std::abs(settings.gridCellSize), 0.000001f);
        const float gridZoomLevel = scene.IsMode2D() ? 0.f : std::clamp(
            std::log10(std::max(std::abs(cameraPosition.y) / gridBaseSpacing,
                0.000001f)), -6.f, 8.f);
        gridData.axisColor = { settings.gridOriginColor.x, settings.gridOriginColor.y,
            settings.gridOriginColor.z, gridZoomLevel };
        gridData.fadeDistance = settings.gridFadeDistance;
        gridData.nearPlane = cam->nearPlane;
        gridData.farPlane = cam->farPlane;
        gridData.mode2D = scene.IsMode2D() ? 1.f : 0.f;

        // Write to constant buffer
        memcpy(renderer.m_gridCBMapped, &gridData, sizeof(PortalGridCBData));

        // Set pipeline and constant buffer
        context->SetPipeline(renderer.m_gridPipeline.get());
        context->SetConstantBuffer(0, renderer.m_gridConstantBuffer.get(), 0);

        // Draw fullscreen triangle (3 vertices, no vertex buffer)
        context->DrawInstanced(3, 1, 0, 0);
    }

    renderer.m_lastRenderCpuTimings.portalMs=std::chrono::duration<double,std::milli>(RenderClock::now()-portalWorkStart).count();
    // Render phase 10: composite retained UI and close per-view timing.

}
}
