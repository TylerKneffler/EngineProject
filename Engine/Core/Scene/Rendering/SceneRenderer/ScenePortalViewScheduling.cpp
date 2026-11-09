#include "Core/Scene/Rendering/SceneRenderer/ScenePortalViewScheduling.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Rendering/Portal/PortalRenderPolicy.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <glm/ext/matrix_transform.hpp>

namespace Engine::Scene::Renderer
{
std::optional<Engine::Graphics::IGraphicsContext::ScissorRect>
ScenePortalViewScheduling::ApertureScissor(const PortalStencilPass& pass,
    const glm::mat4& candidateView, const glm::mat4& proj,
    uint32_t viewportWidth, uint32_t viewportHeight, bool cameraUsesSourceWarpChart)
{
    const auto activePortalPoints = [&](const Engine::Components::SpatialManipulator& portal)
    {
        return cameraUsesSourceWarpChart ? portal.GetWorldPortalShapePoints()
            : portal.GetRenderWorldPortalShapePoints();
    };
        // Stencil is the correctness boundary. This rectangle only avoids
        // shading pixels which cannot belong to the aperture, so a partly
        // near-clipped polygon deliberately falls back to the full viewport.
        if (viewportWidth == 0u || viewportHeight == 0u)
            return std::nullopt;
        const std::vector<glm::vec3> points =
            activePortalPoints(*pass.source);
        if (points.size() < 3u)
            return std::nullopt;

        std::vector<glm::vec4> clipPolygon;
        clipPolygon.reserve(points.size());
        for (const glm::vec3& point : points)
            clipPolygon.push_back(proj * candidateView * glm::vec4(point, 1.f));
        clipPolygon = Engine::Rendering::Portal::ClipApertureToViewFrustum(
            std::move(clipPolygon));
        if (clipPolygon.size() < 3u)
            return Engine::Graphics::IGraphicsContext::ScissorRect { 0, 0, 0, 0 };

        glm::vec2 minimum(std::numeric_limits<float>::max());
        glm::vec2 maximum(std::numeric_limits<float>::lowest());
        for (const glm::vec4& clip : clipPolygon)
        {
            if (clip.w <= 1e-6f || !std::isfinite(clip.w))
                return std::nullopt;
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            minimum = glm::min(minimum, ndc);
            maximum = glm::max(maximum, ndc);
        }

        const float width = static_cast<float>(viewportWidth);
        const float height = static_cast<float>(viewportHeight);
        const int32_t left = std::max(0, static_cast<int32_t>(std::floor(
            (minimum.x * 0.5f + 0.5f) * width)) - 1);
        const int32_t right = std::min(static_cast<int32_t>(viewportWidth),
            static_cast<int32_t>(std::ceil((maximum.x * 0.5f + 0.5f) * width)) + 1);
        // The graphics context normalizes Vulkan's inverted native viewport,
        // so this top-left conversion is shared by DX11, DX12, and Vulkan.
        const int32_t top = std::max(0, static_cast<int32_t>(std::floor(
            (0.5f - maximum.y * 0.5f) * height)) - 1);
        const int32_t bottom = std::min(static_cast<int32_t>(viewportHeight),
            static_cast<int32_t>(std::ceil((0.5f - minimum.y * 0.5f) * height)) + 1);
        if (left >= right || top >= bottom)
            return Engine::Graphics::IGraphicsContext::ScissorRect { 0, 0, 0, 0 };
        return Engine::Graphics::IGraphicsContext::ScissorRect {
            left, top, right, bottom };
}

std::vector<PortalViewJob> ScenePortalViewScheduling::Build(Scene& scene,
    const std::vector<PortalStencilPass>& portalPasses,
    const glm::mat4& view, const glm::mat4& proj,
    const Engine::Components::Camera* cam, bool cameraUsesSourceWarpChart)
{
    const auto& settings = scene.settings;
    const auto activePortalPoints = [&](const Engine::Components::SpatialManipulator& portal)
    {
        return cameraUsesSourceWarpChart ? portal.GetWorldPortalShapePoints()
            : portal.GetRenderWorldPortalShapePoints();
    };
    const auto activePortalFrame = [&](const Engine::Components::SpatialManipulator& portal)
    {
        return cameraUsesSourceWarpChart ? portal.GetPortalWorldFrame()
            : portal.GetRenderPortalWorldFrame();
    };
    const auto mapActivePortalPoint = [&](const Engine::Components::SpatialManipulator& source,
        const glm::vec3& point, const Engine::Components::SpatialManipulator& target)
    {
        return cameraUsesSourceWarpChart ? source.MapWorldPointThroughPortalShape(point, target)
            : source.MapRenderWorldPointThroughPortalShape(point, target);
    };
    std::vector<PortalViewJob> portalViewJobs;
    const uint32_t portalDepthLimit = static_cast<uint32_t>(
        Engine::Rendering::Portal::ClampRecursionDepth(settings.portalRecursionDepth));
    const size_t portalViewBudget = static_cast<size_t>(
        Engine::Rendering::Portal::ClampViewBudget(settings.portalMaxViewsPerFrame));
    const auto apertureRegionInView = [&](const PortalStencilPass& pass,
        const glm::mat4& candidateView)
    {
        const std::vector<glm::vec3> points =
            activePortalPoints(*pass.source);
        if (points.size() < 3)
            return std::vector<glm::vec2>{};
        const glm::mat4 candidateCameraWorld = glm::inverse(candidateView);
        const glm::mat4 sourceFrame = activePortalFrame(*pass.source);
        const float cameraPlaneDistance = glm::dot(
            glm::vec3(candidateCameraWorld[3]) - glm::vec3(sourceFrame[3]),
            glm::vec3(sourceFrame[2]));
        // A camera exactly on an aperture plane has no well-defined source
        // side, and every aperture vertex projects with w=0. Do not feed that
        // singular polygon into recursive scheduling; physical traversal will
        // select the connected chart as soon as the camera body departs.
        if (std::abs(cameraPlaneDistance) <= 1e-5f)
            return std::vector<glm::vec2>{};
        std::vector<glm::vec4> clipPolygon;
        clipPolygon.reserve(points.size());
        for (const glm::vec3& point : points)
            clipPolygon.push_back(proj * candidateView * glm::vec4(point, 1.f));
        clipPolygon = Engine::Rendering::Portal::ClipApertureToViewFrustum(
            std::move(clipPolygon));
        std::vector<glm::vec2> screenPolygon;
        screenPolygon.reserve(clipPolygon.size());
        for (const glm::vec4& point : clipPolygon)
        {
            if (point.w <= 1e-6f || !std::isfinite(point.w))
                return std::vector<glm::vec2>{};
            screenPolygon.push_back(glm::vec2(point) / point.w);
        }
        return screenPolygon;
    };

    using PortalEdge = std::pair<
        const Engine::Components::SpatialManipulator*,
        const Engine::Components::SpatialManipulator*>;
    struct PortalScheduleNode
    {
        glm::mat4 apertureView { 1.f };
        uint32_t depth = 0;
        uint32_t inheritedRootStencilBase = 0;
        const Engine::Components::SpatialManipulator* previousExit = nullptr;
        const Engine::Core::Object* contentScopeRoot = nullptr;
        size_t parentJobIndex = std::numeric_limits<size_t>::max();
        std::vector<glm::vec2> visibleScreenRegion;
        std::vector<PortalEdge> path;
    };
    uint32_t nextRootStencilBase = 1u;
    std::vector<PortalScheduleNode> scheduleQueue;
    if (!portalPasses.empty())
    {
        scheduleQueue.push_back({ view, 0u, 0u, nullptr, nullptr,
            std::numeric_limits<size_t>::max(),
            { {-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f} }, {} });
    }
    size_t scheduleCursor = 0u;
    // Keep every visible root aperture before spending the remaining budget
    // on recursion. A depth-first walk allowed one early portal chain to
    // starve later roots, making them vanish as traversal order changed.
    while (scheduleCursor < scheduleQueue.size() &&
        portalViewJobs.size() < portalViewBudget)
    {
        PortalScheduleNode node = std::move(scheduleQueue[scheduleCursor++]);
        const glm::mat4& apertureView = node.apertureView;
        const uint32_t depth = node.depth;
        if (depth >= portalDepthLimit || portalViewJobs.size() >= portalViewBudget)
            continue;
        const glm::mat4 cameraWorldForView = glm::inverse(apertureView);
        const glm::vec3 viewCameraPosition(cameraWorldForView[3]);
        const glm::vec3 viewForward = glm::normalize(
            glm::vec3(cameraWorldForView[2]));
        const glm::vec3 viewUp = glm::normalize(glm::vec3(cameraWorldForView[1]));
        for (const PortalStencilPass& pass : portalPasses)
        {
            if (portalViewJobs.size() >= portalViewBudget)
                break;
            // The virtual camera has just emerged behind previousExit. That
            // aperture is the boundary of the parent connected view, not a
            // new entrance visible in the remote room. Recursing through it
            // immediately makes equal, aligned portals overwrite their whole
            // parent view. Other apertures—including the original source now
            // visible across the room—remain valid recursive entrances.
            if (Engine::Rendering::Portal::IsImmediateExitAperture(
                pass.source, node.previousExit))
                continue;
            if (!scene.IsObjectInSpatialRegion(pass.source->Owner,
                node.contentScopeRoot))
                continue;
            if (pass.apertureVertexCount < 3)
                continue;

            glm::mat4 effectiveApertureView = apertureView;
            glm::vec3 effectiveCameraPosition = viewCameraPosition;
            const glm::mat4 sourceFrame = activePortalFrame(*pass.source);
            const glm::vec3 sourcePlanePoint(sourceFrame[3]);
            const glm::vec3 sourcePlaneNormal = glm::normalize(
                glm::vec3(sourceFrame[2]));
            const float cameraPlaneDistance = glm::dot(
                viewCameraPosition - sourcePlanePoint, sourcePlaneNormal);
            const float cameraFacing = glm::dot(viewForward, sourcePlaneNormal);
            // At the aperture plane its vertices all project with w=0, so a
            // finite stencil polygon does not exist. If the eye is inside the
            // opening and looking through it, choose that connected chart and
            // offset only the render view onto the source side. Keeping the
            // aperture beyond the near plane yields a stable full-screen mask
            // without moving the physical camera or weakening target clipping.
            if (std::abs(cameraPlaneDistance) <= 1e-5f &&
                std::abs(cameraFacing) > 0.1f &&
                pass.source->IsWorldPointInsidePortalAperture(
                    viewCameraPosition, 0.f))
            {
                const float renderSeparation = std::max(
                    0.002f, cam->nearPlane * 1.05f /
                        std::abs(cameraFacing));
                effectiveCameraPosition -= sourcePlaneNormal *
                    (cameraFacing > 0.f ? renderSeparation : -renderSeparation);
                glm::mat4 effectiveCameraWorld = cameraWorldForView;
                effectiveCameraWorld[3] = glm::vec4(
                    effectiveCameraPosition, 1.f);
                effectiveApertureView = glm::inverse(effectiveCameraWorld);
            }
            std::vector<glm::vec2> visibleScreenRegion =
                Engine::Rendering::Portal::IntersectConvexScreenPolygons(
                    apertureRegionInView(pass, effectiveApertureView),
                    node.visibleScreenRegion);
            if (visibleScreenRegion.size() < 3u)
                continue;
            const PortalEdge edge { pass.source, pass.target };
            const size_t priorConnectionVisits = static_cast<size_t>(
                std::count_if(node.path.begin(), node.path.end(),
                    [&](const PortalEdge& previous)
                    {
                        return (previous.first == edge.first &&
                                previous.second == edge.second) ||
                               (previous.first == edge.second &&
                                previous.second == edge.first);
                    }));
            // A linked pair is allowed to repeat deliberately: with the
            // default limit of two, looking through A shows B and then A once
            // more. The limit prevents an unbounded A -> B -> A loop while
            // preserving recursion through unrelated connections.
            if (!Engine::Rendering::Portal::CanRepeatConnection(
                priorConnectionVisits, settings.portalConnectionRepeatLimit))
                continue;

            uint32_t rootStencilBase = node.inheritedRootStencilBase;
            if (depth == 0u)
            {
                if (nextRootStencilBase + portalDepthLimit - 1u > 0xFFu)
                    break;
                rootStencilBase = nextRootStencilBase;
                nextRootStencilBase += portalDepthLimit;
            }

            const glm::vec3 mappedCamera = mapActivePortalPoint(
                *pass.source, effectiveCameraPosition, *pass.target);
            const glm::vec3 mappedLookAt = mapActivePortalPoint(
                *pass.source, effectiveCameraPosition + viewForward, *pass.target);
            const glm::vec3 mappedUpPoint = mapActivePortalPoint(
                *pass.source, effectiveCameraPosition + viewUp, *pass.target);
            const glm::vec3 mappedForward = glm::normalize(
                mappedLookAt - mappedCamera);
            glm::vec3 mappedUp = mappedUpPoint - mappedCamera;
            mappedUp = glm::dot(mappedUp, mappedUp) <= 1e-6f
                ? glm::vec3(0.f, 1.f, 0.f)
                : glm::normalize(mappedUp);

            PortalViewJob job{};
            job.portal = &pass;
            job.contentScopeRoot =
                pass.target->ResolvePortalContentScopeRoot();
            job.apertureView = effectiveApertureView;
            job.mappedCameraPosition = mappedCamera;
            job.mappedView = glm::lookAtLH(mappedCamera,
                mappedCamera + mappedForward, mappedUp);
            job.depth = depth;
            job.rootStencilBase = rootStencilBase;
            job.parentJobIndex = node.parentJobIndex;
            job.shadowKey = 1469598103934665603ull;
            HashRevision(job.shadowKey, static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(cam)));
            for (const auto& pathEdge : node.path)
            {
                HashRevision(job.shadowKey, static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(pathEdge.first)));
                HashRevision(job.shadowKey, static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(pathEdge.second)));
            }
            HashRevision(job.shadowKey, static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(pass.source)));
            HashRevision(job.shadowKey, static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(pass.target)));
            for (size_t pointIndex = 0; pointIndex < visibleScreenRegion.size();
                ++pointIndex)
            {
                const glm::vec2& first = visibleScreenRegion[pointIndex];
                const glm::vec2& second = visibleScreenRegion[
                    (pointIndex + 1u) % visibleScreenRegion.size()];
                job.screenArea += first.x * second.y - first.y * second.x;
            }
            job.screenArea = std::abs(job.screenArea) * 0.5f;
            const size_t jobIndex = portalViewJobs.size();
            portalViewJobs.push_back(job);

            PortalScheduleNode child{};
            child.apertureView = job.mappedView;
            child.depth = depth + 1u;
            child.inheritedRootStencilBase = rootStencilBase;
            child.previousExit = pass.target;
            child.contentScopeRoot = job.contentScopeRoot;
            child.parentJobIndex = jobIndex;
            child.visibleScreenRegion = std::move(visibleScreenRegion);
            child.path = node.path;
            child.path.push_back(edge);
            scheduleQueue.push_back(std::move(child));
        }
    }

    // Breadth-first discovery protects visible root portals from exhausting
    // the frame budget, but stencil recursion itself must execute depth-first.
    // Sibling apertures use the same depth value; rendering a sibling before
    // an earlier branch's descendants leaves that value in unrelated pixels
    // and lets the descendant mask leak into (or be rejected by) the sibling.
    // Reorder only after the fair set of jobs has been selected.
    if (portalViewJobs.size() > 1u)
    {
        const size_t noParent = std::numeric_limits<size_t>::max();
        std::vector<std::vector<size_t>> childJobs(portalViewJobs.size());
        std::vector<size_t> rootJobs;
        rootJobs.reserve(portalViewJobs.size());
        for (size_t jobIndex = 0; jobIndex < portalViewJobs.size(); ++jobIndex)
        {
            const size_t parentIndex = portalViewJobs[jobIndex].parentJobIndex;
            if (parentIndex == noParent)
                rootJobs.push_back(jobIndex);
            else if (parentIndex < childJobs.size())
                childJobs[parentIndex].push_back(jobIndex);
        }

        std::vector<size_t> depthFirstOrder;
        depthFirstOrder.reserve(portalViewJobs.size());
        std::function<void(size_t)> appendSubtree = [&](size_t jobIndex)
        {
            depthFirstOrder.push_back(jobIndex);
            for (const size_t childIndex : childJobs[jobIndex])
                appendSubtree(childIndex);
        };
        for (const size_t rootIndex : rootJobs)
            appendSubtree(rootIndex);

        if (depthFirstOrder.size() == portalViewJobs.size())
        {
            std::vector<PortalViewJob> orderedJobs;
            orderedJobs.reserve(portalViewJobs.size());
            for (const size_t jobIndex : depthFirstOrder)
                orderedJobs.push_back(std::move(portalViewJobs[jobIndex]));
            portalViewJobs = std::move(orderedJobs);
        }
    }

    return portalViewJobs;
}
}
