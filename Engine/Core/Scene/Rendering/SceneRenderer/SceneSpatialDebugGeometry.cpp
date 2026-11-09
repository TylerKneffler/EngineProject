#include "Core/Scene/Rendering/SceneRenderer/SceneSpatialDebugGeometry.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <glm/ext/matrix_transform.hpp>

namespace Engine::Scene::Renderer
{
std::vector<SpatialDebugPass> SceneSpatialDebugGeometry::Build(
    const std::vector<PortalStencilPass>& portalPasses,
    const std::vector<Engine::Components::SpatialManipulator*>& manipulators,
    Engine::Model::AnimationVertex* portalVertices, uint32_t& portalVertexCursor,
    uint32_t portalVertexCapacity, bool cameraUsesSourceWarpChart)
{
    const auto activePortalPoints = [&](const Engine::Components::SpatialManipulator& portal)
    {
        return cameraUsesSourceWarpChart ? portal.GetWorldPortalShapePoints()
            : portal.GetRenderWorldPortalShapePoints();
    };
    const auto mapActivePortalPoint = [&](const Engine::Components::SpatialManipulator& source,
        const glm::vec3& point, const Engine::Components::SpatialManipulator& target)
    {
        return cameraUsesSourceWarpChart ? source.MapWorldPointThroughPortalShape(point, target)
            : source.MapRenderWorldPointThroughPortalShape(point, target);
    };
    std::vector<SpatialDebugPass> spatialDebugPasses;
        const auto appendDebugVertex = [&](const glm::vec3& worldPoint)
        {
            Engine::Model::AnimationVertex& vertex = portalVertices[portalVertexCursor++];
            vertex = Engine::Model::AnimationVertex {};
            vertex.pos[0] = worldPoint.x;
            vertex.pos[1] = worldPoint.y;
            vertex.pos[2] = worldPoint.z;
        };
        const auto appendDebugBox = [&, appendDebugVertex](const glm::mat4& world,
            const glm::vec3& halfSize, const glm::vec4& color)
        {
            constexpr uint32_t kBoxVertexCount = 36;
            if (portalVertexCursor + kBoxVertexCount > portalVertexCapacity)
                return;
            static constexpr uint8_t indices[kBoxVertexCount] = {
                0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
                0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7,
                0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2 };
            const glm::vec3 corners[8] = {
                {-halfSize.x, -halfSize.y, -halfSize.z},
                { halfSize.x, -halfSize.y, -halfSize.z},
                { halfSize.x,  halfSize.y, -halfSize.z},
                {-halfSize.x,  halfSize.y, -halfSize.z},
                {-halfSize.x, -halfSize.y,  halfSize.z},
                { halfSize.x, -halfSize.y,  halfSize.z},
                { halfSize.x,  halfSize.y,  halfSize.z},
                {-halfSize.x,  halfSize.y,  halfSize.z} };
            SpatialDebugPass pass{};
            pass.vertexOffset = static_cast<uint64_t>(portalVertexCursor) *
                sizeof(Engine::Model::AnimationVertex);
            pass.vertexCount = kBoxVertexCount;
            pass.color = color;
            for (uint8_t index : indices)
                appendDebugVertex(glm::vec3(world * glm::vec4(corners[index], 1.f)));
            spatialDebugPasses.push_back(pass);
        };
        const auto appendDebugSphere = [&, appendDebugVertex](const glm::mat4& world,
            float radius, const glm::vec4& color)
        {
            constexpr uint32_t kSphereVertexCount = 24;
            if (portalVertexCursor + kSphereVertexCount > portalVertexCapacity)
                return;
            const glm::vec3 points[6] = {
                { radius, 0.f, 0.f }, { -radius, 0.f, 0.f },
                { 0.f, radius, 0.f }, { 0.f, -radius, 0.f },
                { 0.f, 0.f, radius }, { 0.f, 0.f, -radius } };
            static constexpr uint8_t indices[kSphereVertexCount] = {
                2, 0, 4, 2, 4, 1, 2, 1, 5, 2, 5, 0,
                3, 4, 0, 3, 1, 4, 3, 5, 1, 3, 0, 5 };
            SpatialDebugPass pass{};
            pass.vertexOffset = static_cast<uint64_t>(portalVertexCursor) *
                sizeof(Engine::Model::AnimationVertex);
            pass.vertexCount = kSphereVertexCount;
            pass.color = color;
            for (uint8_t index : indices)
                appendDebugVertex(glm::vec3(world * glm::vec4(points[index], 1.f)));
            spatialDebugPasses.push_back(pass);
        };
        const auto appendDebugSegment = [&, appendDebugBox](
            const glm::vec3& start, const glm::vec3& end, float thickness,
            const glm::vec4& color)
        {
            const glm::vec3 delta = end - start;
            const float length = glm::length(delta);
            if (length <= 1e-5f)
                return;
            const glm::vec3 xAxis = delta / length;
            const glm::vec3 helper = std::abs(glm::dot(xAxis,
                glm::vec3(0.f, 1.f, 0.f))) > 0.95f
                ? glm::vec3(0.f, 0.f, 1.f)
                : glm::vec3(0.f, 1.f, 0.f);
            const glm::vec3 zAxis = glm::normalize(glm::cross(xAxis, helper));
            const glm::vec3 yAxis = glm::normalize(glm::cross(zAxis, xAxis));
            glm::mat4 world(1.f);
            world[0] = glm::vec4(xAxis, 0.f);
            world[1] = glm::vec4(yAxis, 0.f);
            world[2] = glm::vec4(zAxis, 0.f);
            world[3] = glm::vec4((start + end) * 0.5f, 1.f);
            appendDebugBox(world,
                glm::vec3(length * 0.5f, thickness, thickness), color);
        };

        // Portal connectivity is an editor-only diagnostic. The highlighted
        // geometry is generated transiently from the logical aperture points:
        // yellow markers identify paired points and yellow/gold bars trace
        // aperture edges, point mappings, and normals. None of it is runtime
        // portal geometry or a portal surface.
        using DebugPortalPair = std::pair<
            const Engine::Components::SpatialManipulator*,
            const Engine::Components::SpatialManipulator*>;
        std::vector<DebugPortalPair> highlightedPortalPairs;
        for (const PortalStencilPass& portalPass : portalPasses)
        {
            const bool alreadyHighlighted = std::any_of(
                highlightedPortalPairs.begin(), highlightedPortalPairs.end(),
                [&](const DebugPortalPair& pair)
                {
                    return (pair.first == portalPass.source &&
                            pair.second == portalPass.target) ||
                           (pair.first == portalPass.target &&
                            pair.second == portalPass.source);
                });
            if (alreadyHighlighted)
                continue;
            highlightedPortalPairs.emplace_back(
                portalPass.source, portalPass.target);

            const std::vector<glm::vec3> sourcePoints =
                activePortalPoints(*portalPass.source);
            const std::vector<glm::vec3> targetPoints =
                activePortalPoints(*portalPass.target);
            const size_t pointCount = std::min(
                sourcePoints.size(), targetPoints.size());
            if (pointCount < 3)
                continue;

            float averageEdgeLength = 0.f;
            for (size_t pointIndex = 0; pointIndex < pointCount; ++pointIndex)
            {
                const size_t next = (pointIndex + 1u) % pointCount;
                averageEdgeLength += glm::length(
                    sourcePoints[next] - sourcePoints[pointIndex]);
                averageEdgeLength += glm::length(
                    targetPoints[next] - targetPoints[pointIndex]);
            }
            averageEdgeLength /= static_cast<float>(pointCount * 2u);
            const float markerRadius = std::clamp(
                averageEdgeLength * 0.045f, 0.015f, 0.2f);
            const float edgeThickness = markerRadius * 0.32f;
            const glm::vec4 pointColor { 1.f, 0.92f, 0.10f, 1.f };
            const glm::vec4 edgeColor { 1.f, 0.76f, 0.05f, 1.f };
            const glm::vec4 connectionColor { 1.f, 0.62f, 0.02f, 1.f };
            const glm::vec4 normalColor { 1.f, 0.84f, 0.18f, 1.f };

            glm::vec3 sourceCenter(0.f);
            glm::vec3 targetCenter(0.f);
            for (size_t pointIndex = 0; pointIndex < pointCount; ++pointIndex)
            {
                const size_t next = (pointIndex + 1u) % pointCount;
                sourceCenter += sourcePoints[pointIndex];
                targetCenter += targetPoints[pointIndex];
                appendDebugSphere(glm::translate(glm::mat4(1.f),
                    sourcePoints[pointIndex]), markerRadius, pointColor);
                appendDebugSphere(glm::translate(glm::mat4(1.f),
                    targetPoints[pointIndex]), markerRadius, pointColor);
                appendDebugSegment(sourcePoints[pointIndex], sourcePoints[next],
                    edgeThickness, edgeColor);
                appendDebugSegment(targetPoints[pointIndex], targetPoints[next],
                    edgeThickness, edgeColor);
                const glm::vec3 mappedSourcePoint = mapActivePortalPoint(
                    *portalPass.source, sourcePoints[pointIndex],
                    *portalPass.target);
                // Point indices describe each aperture's authored winding;
                // they are not necessarily the connected correspondence. The
                // portal half-turn reverses local X, so index-to-index bars
                // falsely appeared crossed even while the view transform used
                // the opposite target corner. Draw the transform itself.
                appendDebugSegment(sourcePoints[pointIndex],
                    mappedSourcePoint, edgeThickness * 0.55f,
                    connectionColor);
            }
            sourceCenter /= static_cast<float>(pointCount);
            targetCenter /= static_cast<float>(pointCount);
            glm::vec3 sourceNormal = glm::cross(
                sourcePoints[1] - sourcePoints[0],
                sourcePoints[2] - sourcePoints[0]);
            glm::vec3 targetNormal = glm::cross(
                targetPoints[1] - targetPoints[0],
                targetPoints[2] - targetPoints[0]);
            sourceNormal = glm::dot(sourceNormal, sourceNormal) > 1e-8f
                ? glm::normalize(sourceNormal) : glm::vec3(0.f, 0.f, 1.f);
            targetNormal = glm::dot(targetNormal, targetNormal) > 1e-8f
                ? glm::normalize(targetNormal) : glm::vec3(0.f, 0.f, 1.f);
            const float normalLength = std::max(
                averageEdgeLength * 0.35f, markerRadius * 3.f);
            appendDebugSegment(sourceCenter,
                sourceCenter + sourceNormal * normalLength,
                edgeThickness, normalColor);
            appendDebugSegment(targetCenter,
                targetCenter + targetNormal * normalLength,
                edgeThickness, normalColor);
        }

        for (Engine::Components::SpatialManipulator* manipulator :
            manipulators)
        {
            Engine::Core::Object* sceneObject = manipulator
                ? manipulator->Owner : nullptr;
            if (!sceneObject || !sceneObject->IsEnabledInHierarchy())
                continue;
            if (!manipulator || !manipulator->enabled)
                continue;
            const auto mode = static_cast<
                Engine::Components::SpatialManipulator::ConnectionMode>(
                    manipulator->connectionMode);
            if (mode == Engine::Components::SpatialManipulator::ConnectionMode::Portal ||
                mode == Engine::Components::SpatialManipulator::ConnectionMode::LinkedPortal)
                continue;

            const glm::mat4 ownerWorld =
                sceneObject->transform.GetWorldMatrix();
            const glm::vec3 center(ownerWorld[3]);
            const glm::vec4 warpDebugColor { 1.f, 0.78f, 0.08f, 1.f };
            const float markerRadius = 0.08f;
            if (manipulator->definesWarpVolume)
            {
                const auto shape = static_cast<
                    Engine::Components::SpatialManipulator::WarpVolumeShape>(
                        manipulator->warpVolumeShape);
                if (shape == Engine::Components::SpatialManipulator::WarpVolumeShape::Sphere)
                {
                    appendDebugSphere(ownerWorld,
                        std::max(0.001f, std::abs(manipulator->warpVolumeRadius)),
                        warpDebugColor);
                }
                else
                {
                    const glm::vec3 halfSize = shape ==
                        Engine::Components::SpatialManipulator::WarpVolumeShape::Box
                        ? glm::max(glm::abs(manipulator->warpVolumeSize) * 0.5f,
                            glm::vec3(0.001f))
                        : glm::vec3(0.5f);
                    appendDebugBox(ownerWorld, halfSize,
                        warpDebugColor);
                }
                // A volume boundary alone is difficult to associate with its
                // mapping.  Give every warped space a yellow origin point and
                // short local-frame lines so its placement/orientation is
                // unambiguous in the Scene editor.
                appendDebugSphere(glm::translate(glm::mat4(1.f), center),
                    markerRadius, { 1.f, 0.94f, 0.18f, 1.f });
                const float axisLength = std::clamp(
                    manipulator->warpVolumeRadius * 0.25f, 0.35f, 1.25f);
                appendDebugSegment(center, center + glm::vec3(ownerWorld[0]) *
                    axisLength, markerRadius * 0.3f, warpDebugColor);
                appendDebugSegment(center, center + glm::vec3(ownerWorld[1]) *
                    axisLength, markerRadius * 0.3f, warpDebugColor);
                appendDebugSegment(center, center + glm::vec3(ownerWorld[2]) *
                    axisLength, markerRadius * 0.3f, warpDebugColor);
            }
            else if (mode == Engine::Components::SpatialManipulator::ConnectionMode::MatrixOverlay)
            {
                appendDebugBox(ownerWorld * manipulator->GetOverlayMatrix(),
                    glm::vec3(0.5f), { 1.f, 0.72f, 0.08f, 1.f });
            }
        }
    return spatialDebugPasses;
}
}
