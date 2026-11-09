#include "Core/Scene/Rendering/SceneRenderer/ScenePortalPassPreparation.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Rendering/Portal/PortalRenderPolicy.h"
#include <algorithm>
#include <cmath>

namespace Engine::Scene::Renderer
{
ScenePortalPassPreparation::Result ScenePortalPassPreparation::Build(
    const std::vector<Engine::Components::SpatialManipulator*>& manipulators,
    Engine::Model::AnimationVertex* portalVertices, uint32_t portalVertexCapacity,
    const glm::mat4& view, const glm::mat4& proj,
    const glm::vec3& cameraPosition, bool cameraUsesSourceWarpChart,
    TargetResolver resolveTarget)
{
    Result result;
    auto& portalPasses = result.passes;
    uint32_t portalVertexCursor = 0;
    // Portal geometry and virtual cameras must live in the same coordinate
    // chart as the camera and ordinary meshes for this view. Game cameras
    // inside a finite warp volume intentionally render its physical/source
    // chart; editor inspection normally renders the embedded chart instead.
    // Mixing render-chart apertures with source-chart depth makes the visible
    // doorway receive no stencil mask (notably in the three-room matrix ring).
    const auto activePortalPoints = [&](const Engine::Components::
        SpatialManipulator& portal)
    {
        return cameraUsesSourceWarpChart
            ? portal.GetWorldPortalShapePoints()
            : portal.GetRenderWorldPortalShapePoints();
    };
    const auto activePortalFrame = [&](const Engine::Components::
        SpatialManipulator& portal)
    {
        return cameraUsesSourceWarpChart
            ? portal.GetPortalWorldFrame()
            : portal.GetRenderPortalWorldFrame();
    };
    // Sort overlapping apertures in the same chart as the view.  Asking the
    // Transform for GetWorldMatrixWithLayer() always applies the render-space
    // warp, even when this camera deliberately uses the physical/source chart;
    // its forward axis could therefore choose the wrong portal as the nearer
    // root.  The inverse view is already the authoritative camera transform
    // for both chart modes.
    const glm::mat4 activeCameraWorld = glm::inverse(view);
    const glm::vec3 cameraForward = glm::normalize(
        glm::vec3(activeCameraWorld[2]));
    size_t portalObjectOrder = 0;
    for (Engine::Components::SpatialManipulator* manipulator :
        manipulators)
    {
        const size_t objectOrder = portalObjectOrder++;
        Engine::Core::Object* object = manipulator ? manipulator->Owner : nullptr;
        if (!object || !object->IsEnabledInHierarchy())
            continue;
        if (!manipulator || !manipulator->enabled)
            continue;

        const auto mode = static_cast<Engine::Components::SpatialManipulator::ConnectionMode>(
            manipulator->connectionMode);
        if (mode != Engine::Components::SpatialManipulator::ConnectionMode::Portal &&
            mode != Engine::Components::SpatialManipulator::ConnectionMode::LinkedPortal)
            continue;

        Engine::Components::SpatialManipulator* target =
            resolveTarget(manipulator);
        if (!target || !target->enabled || !target->Owner ||
            !manipulator->HasCompatiblePortalShapeWith(*target))
            continue;

        PortalStencilPass pass{};
        pass.source = manipulator;
        pass.target = target;
        const std::vector<glm::vec3> aperturePoints =
            activePortalPoints(*manipulator);
        glm::vec3 apertureCenter(0.f);
        for (const glm::vec3& point : aperturePoints)
            apertureCenter += point;
        if (!aperturePoints.empty())
            apertureCenter /= static_cast<float>(aperturePoints.size());
        else
            apertureCenter = object->transform.GetWorldPosition();
        pass.viewDepth = glm::dot(
            apertureCenter - cameraPosition, cameraForward);
        // A procedural aperture uses one transient object slot. It is restored
        // before ordinary scene rendering continues and does not require a
        // corresponding mesh/material draw.
        constexpr uint32_t kPortalObjectSlot = 0u;
        // Keep the stencil aperture rasterizable when it lies between the
        // camera and its near plane. The vertex shader clamps only this
        // procedural mask; connected scene geometry retains normal clipping.
        pass.apertureDrawData = {
            kPortalObjectSlot, 0u, kPortalApertureNearClampFlag, 0u };
        pass.apertureObjectData.world = glm::mat4(1.f);
        pass.apertureObjectData.viewProjection = proj * view;
        pass.apertureObjectData.baseColor = glm::vec4(1.f);
        pass.apertureObjectData.ambientUnlit = { 0.f, 0.f, 0.f, 1.f };
        pass.apertureObjectData.emissiveOcclusion = { 0.f, 0.f, 0.f, 1.f };
        pass.apertureObjectData.materialParams = { 0.f, 1.f, 1.f, 0.f };
        pass.apertureObjectData.viewPositionAlphaCutoff =
            glm::vec4(cameraPosition, 0.001f);
        pass.drawOrder = objectOrder;
        portalPasses.push_back(pass);
    }

    // Draw far apertures first so nearer portals deterministically win where
    // their screen-space masks overlap. Scene draw order is the final stable
    // tie-breaker, independent of pointer values or hash iteration order.
    std::stable_sort(portalPasses.begin(), portalPasses.end(),
        [](const PortalStencilPass& left, const PortalStencilPass& right)
        {
            if (std::abs(left.viewDepth - right.viewDepth) > 1e-5f)
                return left.viewDepth > right.viewDepth;
            const std::string& leftName = left.source->Owner->name;
            const std::string& rightName = right.source->Owner->name;
            if (leftName != rightName)
                return leftName < rightName;
            return left.drawOrder < right.drawOrder;
        });

    for (PortalStencilPass& pass : portalPasses)
    {
        const std::vector<glm::vec3> points =
            activePortalPoints(*pass.source);
        if (points.size() < 3)
            continue;
        const uint32_t required = Engine::Rendering::Portal::
            TriangulatedApertureVertexCount(points.size());
        if (portalVertexCursor + required > portalVertexCapacity)
            break;

        pass.apertureVertexOffset = static_cast<uint64_t>(portalVertexCursor) *
            sizeof(Engine::Model::AnimationVertex);
        pass.apertureVertexCount = required;
        for (size_t pointIndex = 1; pointIndex + 1 < points.size(); ++pointIndex)
        {
            const glm::vec3 triangle[3] = {
                points[0], points[pointIndex], points[pointIndex + 1] };
            for (const glm::vec3& point : triangle)
            {
                Engine::Model::AnimationVertex& vertex = portalVertices[portalVertexCursor++];
                vertex = Engine::Model::AnimationVertex {};
                vertex.pos[0] = point.x;
                vertex.pos[1] = point.y;
                vertex.pos[2] = point.z;
            }
        }
    }

    result.vertexCursor = portalVertexCursor;
    return result;
}
}
