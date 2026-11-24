#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/SceneRenderer/SceneRendererDirectionalShadowSubmission.h"

#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Rendering/RenderUtilities.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Engine::Rendering
{
void SceneRendererDirectionalShadowSubmission::Submit(
    DirectionalShadowSubmissionContext& input)
{
    SceneRenderer& renderer = input.renderer;
    auto& scene = renderer.GetScene();
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
    const auto submissionCyclesStart = scene.IsUpdateProfilingEnabled()
        ? threadCycles() : 0;
    const uint32_t tileResolution = input.cascadeCount > 1u
        ? input.atlasResolution / 2u : input.atlasResolution;
    std::array<const Engine::Graphics::IGraphicsTexture*, 30> shadowTextures{};
    bool shadowTexturesBound = false;
    for (uint32_t cascade = 0; cascade < input.cascadeCount; ++cascade)
    {
        if (!input.dirtyMask[cascade])
            continue;
        renderer.m_shadowDebugSnapshot.refreshedCascadeMask |= 1u << cascade;
        renderer.m_lastShadowCpuTiming.refreshedCascadeMask |= 1u << cascade;
        // The cascade's atlas tile (2 x 2, or 2 x 3 with the refresh blend's
        // spares; SceneRendererShadows assigns them).
        const uint32_t tile = renderer.m_directionalShadowCascadeTile[cascade];
        const uint32_t tileX = input.cascadeCount > 1u ? tile & 1u : 0u;
        const uint32_t tileY = input.cascadeCount > 1u ? tile >> 1u : 0u;
        // The rectangle redrawn: the whole tile, or a far region refresh's
        // texels (drawn through the matching cropped projection).
        const bool region = input.farRegionRefresh && input.cascades.hasFarCascade &&
            cascade + 1u == input.cascadeCount;
        const glm::uvec4 rect = region
            ? input.farRegionPixels
            : glm::uvec4(0u, 0u, tileResolution, tileResolution);
        const uint32_t left = tileX * tileResolution + rect.x;
        const uint32_t top = tileY * tileResolution + rect.y;
        const uint32_t right = tileX * tileResolution + rect.z;
        const uint32_t bottom = tileY * tileResolution + rect.w;
        context->SetViewport({
            static_cast<float>(left), static_cast<float>(top),
            static_cast<float>(right - left), static_cast<float>(bottom - top), 0.f, 1.f });
        context->SetScissorRect({
            static_cast<int32_t>(left), static_cast<int32_t>(top),
            static_cast<int32_t>(right), static_cast<int32_t>(bottom) });
        if (input.preserveAtlas)
            context->ClearDepthRectangle({
                int32_t(left), int32_t(top), int32_t(right), int32_t(bottom) },
                Engine::Graphics::Depth::ClearValue);

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
                ? renderer.m_directionalShadowDoubleSidedPipeline.get()
                : renderer.m_directionalShadowPipeline.get();
            const auto bindStarted = scene.IsUpdateProfilingEnabled()
                ? Clock::now() : Clock::time_point{};
            auto* resolvedPipeline = input.geometry.GeometryPipeline(
                draw, shadowPipeline);
            context->SetPipeline(resolvedPipeline);
            const uint32_t recordIndex = cascade * renderer.m_objectCapacity +
                draw.drawData.objectIndex;
            context->SetConstantBuffer(0, renderer.m_shadowConstantBuffer.get(),
                static_cast<uint64_t>(recordIndex) * renderer.kCBStride);
            const auto afterConstants = scene.IsUpdateProfilingEnabled()
                ? Clock::now() : Clock::time_point{};
            for (uint32_t textureSlot = 0; textureSlot < 12; ++textureSlot)
            {
                if (textureSlot != 7u && (!shadowTexturesBound ||
                    shadowTextures[textureSlot] != draw.textures[textureSlot]))
                    context->SetTexture(textureSlot, draw.textures[textureSlot]);
            }
            shadowTextures = draw.textures;
            shadowTexturesBound = true;
            const auto beforeGeometry = scene.IsUpdateProfilingEnabled()
                ? Clock::now() : Clock::time_point{};
            if (draw.automaticGroup != UINT32_MAX)
            {
                automaticSubmitted[draw.automaticGroup] = 1;
                const auto submitted = input.submitAutomaticGroup(
                    input.automaticGroups[draw.automaticGroup],
                    input.drawViewProjections[cascade],
                    cascade * renderer.m_objectCapacity, resolvedPipeline,
                    &input.cascadeVisible[cascade]);
                renderer.m_shadowDebugSnapshot.submittedCasterDrawCount +=
                    static_cast<uint32_t>(submitted.draws);
                renderer.m_lastShadowCpuTiming.automaticDraws += submitted.draws;
                if (scene.IsUpdateProfilingEnabled())
                {
                    renderer.m_lastShadowCpuTiming.constantBindingMs +=
                        elapsedMs(bindStarted, afterConstants);
                    renderer.m_lastShadowCpuTiming.textureBindingMs +=
                        elapsedMs(afterConstants, beforeGeometry);
                    const double batchMs = elapsedMs(beforeGeometry, Clock::now());
                    renderer.m_lastShadowCpuTiming.geometryDrawMs += batchMs;
                    renderer.m_lastShadowCpuTiming.maxBatchMs = std::max(
                        renderer.m_lastShadowCpuTiming.maxBatchMs, batchMs);
                }
                continue;
            }

            input.geometry.SubmitGeometry(draw, true);
            if (scene.IsUpdateProfilingEnabled())
            {
                renderer.m_lastShadowCpuTiming.constantBindingMs +=
                    elapsedMs(bindStarted, afterConstants);
                renderer.m_lastShadowCpuTiming.textureBindingMs +=
                    elapsedMs(afterConstants, beforeGeometry);
                const double batchMs = elapsedMs(beforeGeometry, Clock::now());
                renderer.m_lastShadowCpuTiming.geometryDrawMs += batchMs;
                renderer.m_lastShadowCpuTiming.maxBatchMs = std::max(
                    renderer.m_lastShadowCpuTiming.maxBatchMs, batchMs);
            }
            if (draw.mesh && draw.mesh->IsInstanced())
            {
                const auto draws = static_cast<uint32_t>(std::count_if(
                    draw.mesh->GetInstanceRanges().begin(),
                    draw.mesh->GetInstanceRanges().end(),
                    [](const auto& range)
                    { return range.instanceCount && range.vertexCount; }));
                renderer.m_shadowDebugSnapshot.submittedCasterDrawCount += draws;
                renderer.m_lastShadowCpuTiming.explicitInstanceDraws += draws;
            }
            else
            {
                ++renderer.m_shadowDebugSnapshot.submittedCasterDrawCount;
                ++renderer.m_lastShadowCpuTiming.ordinaryDraws;
            }
        }
    }

    if (scene.IsUpdateProfilingEnabled())
    {
        renderer.m_lastShadowCpuTiming.submissionThreadCycles =
            threadCycles() - submissionCyclesStart;
        renderer.m_lastShadowCpuTiming.submissionWallMs = elapsedMs(
            input.recordsUploadedAt, Clock::now());
    }
    const auto restoreStarted = Clock::now();
    context->EndDepthOnlyPass();
    context->EndGpuTiming(Engine::Graphics::GpuTimingStage::Shadow);
    renderer.m_lastShadowCpuTiming.passRestoreMs = elapsedMs(
        restoreStarted, Clock::now());
    renderer.m_lastShadowCpuTiming.drawSubmissionMs = elapsedMs(
        input.recordsUploadedAt, Clock::now());

    renderer.m_directionalShadowCacheValid = true;
    renderer.m_directionalShadowCachedQuality = input.quality;
    renderer.m_directionalShadowLastRenderedFrame =
        renderer.m_directionalShadowFrameCounter;
    renderer.m_directionalShadowCachedResolution = input.atlasResolution;
    renderer.m_directionalShadowCachedCascadeCount = input.cascades.count;
    renderer.m_directionalShadowCachedLightKey =
        input.shadowSelection.stableLightKey;
    renderer.m_directionalShadowCachedCameraPosition = input.cameraPosition;
    renderer.m_directionalShadowCachedDirection = glm::normalize(
        input.shadowSelection.directionToLight);
    for (uint32_t cascade = 0; cascade < input.cascadeCount; ++cascade)
    {
        if (!input.dirtyMask[cascade])
            continue;
        // A region refresh keeps the tile's square, age and revision: the
        // rest of the tile is still the earlier drawing.
        if (input.farRegionRefresh && input.cascades.hasFarCascade &&
            cascade + 1u == input.cascadeCount)
            continue;
        renderer.m_directionalShadowCachedViewProjections[cascade] =
            input.cascades.viewProjections[cascade];
        renderer.m_directionalShadowCachedCascadeCenters[cascade] =
            input.cascades.centers[cascade];
        renderer.m_directionalShadowCachedCascadeRadii[cascade] =
            input.cascades.radii[cascade];
        renderer.m_directionalShadowCascadeLastRenderedFrame[cascade] =
            renderer.m_directionalShadowFrameCounter;
        if (input.cascades.hasFarCascade && cascade + 1u == input.cascadeCount)
            renderer.m_directionalShadowFarStructureRevision =
                scene.GetStructureRevision();
    }
    renderer.m_directionalShadowCachedCascadeSplits = input.cascades.splits;
    renderer.m_directionalShadowCachedCascadeData = input.cascades.data;
    renderer.m_directionalShadowCachedCameraData = input.cascades.cameraData;
}
}
