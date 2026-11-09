#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDirectionalShadowSubmission.h"

#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Engine::Scene::Renderer
{
void SceneRendererDirectionalShadowSubmission::Submit(
    DirectionalShadowSubmissionContext& input)
{
    Scene& scene = input.scene;
    auto* context = input.graphics;
    using Clock = std::chrono::steady_clock;
    const auto elapsedMs = [](Clock::time_point start, Clock::time_point end)
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    };
    const auto threadCycles = []() -> uint64_t
    {
#if defined(_WIN32)
        ULONG64 cycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cycles);
        return cycles;
#else
        return 0;
#endif
    };
    const auto submissionCyclesStart = scene.m_updateProfilingEnabled
        ? threadCycles() : 0;
    const uint32_t tileResolution = input.cascadeCount > 1u
        ? input.atlasResolution / 2u : input.atlasResolution;
    std::array<const Engine::Graphics::IGraphicsTexture*, 30> shadowTextures{};
    bool shadowTexturesBound = false;
    for (uint32_t cascade = 0; cascade < input.cascadeCount; ++cascade)
    {
        if (!input.dirtyMask[cascade])
            continue;
        scene.m_shadowDebugSnapshot.refreshedCascadeMask |= 1u << cascade;
        const uint32_t tileX = input.cascadeCount > 1u ? cascade & 1u : 0u;
        const uint32_t tileY = input.cascadeCount > 1u ? cascade >> 1u : 0u;
        context->SetViewport({
            static_cast<float>(tileX * tileResolution),
            static_cast<float>(tileY * tileResolution),
            static_cast<float>(tileResolution),
            static_cast<float>(tileResolution), 0.f, 1.f });
        context->SetScissorRect({
            static_cast<int32_t>(tileX * tileResolution),
            static_cast<int32_t>(tileY * tileResolution),
            static_cast<int32_t>((tileX + 1u) * tileResolution),
            static_cast<int32_t>((tileY + 1u) * tileResolution) });
        if (input.preserveAtlas)
            context->ClearDepthRectangle({
                int32_t(tileX * tileResolution), int32_t(tileY * tileResolution),
                int32_t((tileX + 1u) * tileResolution),
                int32_t((tileY + 1u) * tileResolution) }, 1.f);

        std::vector<uint8_t> automaticSubmitted(input.automaticGroups.size(), 0);
        for (size_t drawIndex = 0; drawIndex < input.draws.size(); ++drawIndex)
        {
            if (!(input.cascadeMasks[drawIndex] & (1u << cascade)))
                continue;
            const PreparedDraw& draw = input.draws[drawIndex];
            if (draw.automaticGroup != UINT32_MAX &&
                automaticSubmitted[draw.automaticGroup])
                continue;
            if (draw.automaticGroup == UINT32_MAX &&
                !input.lodSelection.IsSelected(draw, true))
                continue;

            auto* shadowPipeline = draw.shadowDoubleSided
                ? scene.m_directionalShadowDoubleSidedPipeline.get()
                : scene.m_directionalShadowPipeline.get();
            const auto bindStarted = scene.m_updateProfilingEnabled
                ? Clock::now() : Clock::time_point{};
            auto* resolvedPipeline = input.geometry.GeometryPipeline(
                draw, shadowPipeline);
            context->SetPipeline(resolvedPipeline);
            const uint32_t recordIndex = cascade * scene.m_objectCapacity +
                draw.drawData.objectIndex;
            context->SetConstantBuffer(0, scene.m_shadowConstantBuffer.get(),
                static_cast<uint64_t>(recordIndex) * scene.kCBStride);
            const auto afterConstants = scene.m_updateProfilingEnabled
                ? Clock::now() : Clock::time_point{};
            for (uint32_t textureSlot = 0; textureSlot < 12; ++textureSlot)
            {
                if (textureSlot != 7u && (!shadowTexturesBound ||
                    shadowTextures[textureSlot] != draw.textures[textureSlot]))
                    context->SetTexture(textureSlot, draw.textures[textureSlot]);
            }
            shadowTextures = draw.textures;
            shadowTexturesBound = true;
            const auto beforeGeometry = scene.m_updateProfilingEnabled
                ? Clock::now() : Clock::time_point{};
            if (draw.automaticGroup != UINT32_MAX)
            {
                automaticSubmitted[draw.automaticGroup] = 1;
                const auto submitted = input.submitAutomaticGroup(
                    input.automaticGroups[draw.automaticGroup],
                    input.cascades.viewProjections[cascade],
                    cascade * scene.m_objectCapacity, resolvedPipeline,
                    &input.cascadeVisible[cascade]);
                scene.m_shadowDebugSnapshot.submittedCasterDrawCount +=
                    static_cast<uint32_t>(submitted.draws);
                scene.m_lastShadowCpuTiming.automaticDraws += submitted.draws;
                if (scene.m_updateProfilingEnabled)
                {
                    scene.m_lastShadowCpuTiming.constantBindingMs +=
                        elapsedMs(bindStarted, afterConstants);
                    scene.m_lastShadowCpuTiming.textureBindingMs +=
                        elapsedMs(afterConstants, beforeGeometry);
                    const double batchMs = elapsedMs(beforeGeometry, Clock::now());
                    scene.m_lastShadowCpuTiming.geometryDrawMs += batchMs;
                    scene.m_lastShadowCpuTiming.maxBatchMs = std::max(
                        scene.m_lastShadowCpuTiming.maxBatchMs, batchMs);
                }
                continue;
            }

            input.geometry.SubmitGeometry(draw, true);
            if (scene.m_updateProfilingEnabled)
            {
                scene.m_lastShadowCpuTiming.constantBindingMs +=
                    elapsedMs(bindStarted, afterConstants);
                scene.m_lastShadowCpuTiming.textureBindingMs +=
                    elapsedMs(afterConstants, beforeGeometry);
                const double batchMs = elapsedMs(beforeGeometry, Clock::now());
                scene.m_lastShadowCpuTiming.geometryDrawMs += batchMs;
                scene.m_lastShadowCpuTiming.maxBatchMs = std::max(
                    scene.m_lastShadowCpuTiming.maxBatchMs, batchMs);
            }
            if (draw.mesh && draw.mesh->IsInstanced())
            {
                const auto draws = static_cast<uint32_t>(std::count_if(
                    draw.mesh->GetInstanceRanges().begin(),
                    draw.mesh->GetInstanceRanges().end(),
                    [](const auto& range)
                    { return range.instanceCount && range.vertexCount; }));
                scene.m_shadowDebugSnapshot.submittedCasterDrawCount += draws;
                scene.m_lastShadowCpuTiming.explicitInstanceDraws += draws;
            }
            else
            {
                ++scene.m_shadowDebugSnapshot.submittedCasterDrawCount;
                ++scene.m_lastShadowCpuTiming.ordinaryDraws;
            }
        }
    }

    if (scene.m_updateProfilingEnabled)
    {
        scene.m_lastShadowCpuTiming.submissionThreadCycles =
            threadCycles() - submissionCyclesStart;
        scene.m_lastShadowCpuTiming.submissionWallMs = elapsedMs(
            input.recordsUploadedAt, Clock::now());
    }
    const auto restoreStarted = Clock::now();
    context->EndDepthOnlyPass();
    context->EndGpuTiming(Engine::Graphics::GpuTimingStage::Shadow);
    scene.m_lastShadowCpuTiming.passRestoreMs = elapsedMs(
        restoreStarted, Clock::now());
    scene.m_lastShadowCpuTiming.drawSubmissionMs = elapsedMs(
        input.recordsUploadedAt, Clock::now());

    scene.m_directionalShadowCacheValid = true;
    scene.m_directionalShadowCachedQuality = input.quality;
    scene.m_directionalShadowLastRenderedFrame =
        scene.m_directionalShadowFrameCounter;
    scene.m_directionalShadowCachedResolution = input.atlasResolution;
    scene.m_directionalShadowCachedCascadeCount = input.cascades.count;
    scene.m_directionalShadowCachedLightKey =
        input.shadowSelection.stableLightKey;
    scene.m_directionalShadowCachedCameraPosition = input.cameraPosition;
    scene.m_directionalShadowCachedDirection = glm::normalize(
        input.shadowSelection.directionToLight);
    for (uint32_t cascade = 0; cascade < input.cascadeCount; ++cascade)
    {
        if (!input.dirtyMask[cascade])
            continue;
        scene.m_directionalShadowCachedViewProjections[cascade] =
            input.cascades.viewProjections[cascade];
        scene.m_directionalShadowCachedCascadeCenters[cascade] =
            input.cascades.centers[cascade];
        scene.m_directionalShadowCachedCascadeRadii[cascade] =
            input.cascades.radii[cascade];
        scene.m_directionalShadowCascadeLastRenderedFrame[cascade] =
            scene.m_directionalShadowFrameCounter;
    }
    scene.m_directionalShadowCachedCascadeSplits = input.cascades.splits;
    scene.m_directionalShadowCachedCascadeData = input.cascades.data;
    scene.m_directionalShadowCachedCameraData = input.cascades.cameraData;
}
}
