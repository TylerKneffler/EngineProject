#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Rendering/Lighting/Pipelines/Realtime/RealtimeLightingPipeline.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Object.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <array>
#include <cmath>
#include <iostream>
#include <utility>

int main()
{
    using namespace Engine::Components;

    Transform transform;
    transform.position = { 0.f, 0.f, 0.f };
    transform.matrixLayer.enabled = true;
    transform.matrixLayer.localToLayer = glm::translate(glm::mat4(1.f), glm::vec3(5.f, 0.f, 0.f));
    transform.matrixLayer.layerToLocal = glm::inverse(transform.matrixLayer.localToLayer);

    const glm::vec3 warped = transform.ApplyLocalMatrixLayer(glm::vec3(1.f, 2.f, 3.f));
    assert(glm::length(warped - glm::vec3(6.f, 2.f, 3.f)) < 0.0001f);

    const glm::vec3 restored = transform.InverseApplyLocalMatrixLayer(warped);
    assert(glm::length(restored - glm::vec3(1.f, 2.f, 3.f)) < 0.0001f);

    // World-layer point mapping must use the whole affine matrix. The old
    // position-offset path happened to work for translation only but lost
    // both rotation and scale.
    transform.matrixLayer.localToLayer = glm::translate(glm::mat4(1.f),
        glm::vec3(5.f, -3.f, 2.f)) * glm::rotate(glm::mat4(1.f),
        glm::radians(90.f), glm::vec3(0.f, 0.f, 1.f)) * glm::scale(
        glm::mat4(1.f), glm::vec3(2.f, 0.5f, 1.5f));
    transform.matrixLayer.layerToLocal = glm::inverse(transform.matrixLayer.localToLayer);
    const glm::vec3 worldPoint(3.f, -2.f, 4.f);
    const glm::vec3 expectedWorldLayerPoint = glm::vec3(
        transform.matrixLayer.localToLayer * glm::vec4(worldPoint, 1.f));
    const glm::vec3 mappedWorldLayerPoint = transform.ApplyWorldMatrixLayer(worldPoint);
    assert(glm::length(mappedWorldLayerPoint - expectedWorldLayerPoint) < 0.0001f);
    assert(glm::length(transform.InverseApplyWorldMatrixLayer(mappedWorldLayerPoint) -
        worldPoint) < 0.0001f);

    std::vector<Mesh::Vertex> vertices(6);
    for (size_t i = 0; i < 6; ++i)
    {
        vertices[i].pos[0] = i < 3 ? -1.f : 1.f;
        vertices[i].pos[1] = (i % 3 == 0) ? -1.f : (i % 3 == 1 ? 0.f : 1.f);
        vertices[i].pos[2] = 0.f;
        vertices[i].normal[0] = 0.f;
        vertices[i].normal[1] = 0.f;
        vertices[i].normal[2] = 1.f;
    }

    const auto split = Mesh::SliceByPlane(vertices, glm::vec3(0.f), glm::vec3(1.f, 0.f, 0.f));
    assert(!split.first.empty());
    assert(!split.second.empty());

    // A portal cut through a closed mesh must be closed by a weldable cap on
    // each side, not left as the open clipped triangle stream.
    std::vector<Mesh::Vertex> cube;
    const auto appendFace = [&](const glm::vec3& a, const glm::vec3& b,
        const glm::vec3& c, const glm::vec3& d, const glm::vec3& normal)
    {
        const glm::vec3 positions[6] = { a, b, c, a, c, d };
        for (const glm::vec3& position : positions)
        {
            Mesh::Vertex vertex{};
            vertex.pos[0] = position.x;
            vertex.pos[1] = position.y;
            vertex.pos[2] = position.z;
            vertex.normal[0] = normal.x;
            vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;
            cube.push_back(vertex);
        }
    };
    appendFace({ 1.f, -1.f, -1.f }, { 1.f, 1.f, -1.f },
        { 1.f, 1.f, 1.f }, { 1.f, -1.f, 1.f }, { 1.f, 0.f, 0.f });
    appendFace({ -1.f, -1.f, 1.f }, { -1.f, 1.f, 1.f },
        { -1.f, 1.f, -1.f }, { -1.f, -1.f, -1.f }, { -1.f, 0.f, 0.f });
    appendFace({ -1.f, 1.f, -1.f }, { -1.f, 1.f, 1.f },
        { 1.f, 1.f, 1.f }, { 1.f, 1.f, -1.f }, { 0.f, 1.f, 0.f });
    appendFace({ -1.f, -1.f, 1.f }, { -1.f, -1.f, -1.f },
        { 1.f, -1.f, -1.f }, { 1.f, -1.f, 1.f }, { 0.f, -1.f, 0.f });
    appendFace({ -1.f, -1.f, 1.f }, { 1.f, -1.f, 1.f },
        { 1.f, 1.f, 1.f }, { -1.f, 1.f, 1.f }, { 0.f, 0.f, 1.f });
    appendFace({ 1.f, -1.f, -1.f }, { -1.f, -1.f, -1.f },
        { -1.f, 1.f, -1.f }, { 1.f, 1.f, -1.f }, { 0.f, 0.f, -1.f });

    const auto cappedSplit = Mesh::SliceByPlane(cube, glm::vec3(0.f),
        glm::vec3(1.f, 0.f, 0.f));
    const auto countCapVertices = [](const std::vector<Mesh::Vertex>& half,
        const glm::vec3& expectedNormal)
    {
        size_t count = 0;
        for (const Mesh::Vertex& vertex : half)
        {
            const glm::vec3 position(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
            const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            const glm::vec3 tangent(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
            if (std::abs(position.x) < 0.0001f &&
                glm::dot(normal, expectedNormal) > 0.999f)
            {
                assert(std::abs(glm::length(tangent) - 1.f) < 0.0001f);
                assert(std::isfinite(vertex.uv[0]) && std::isfinite(vertex.uv[1]));
                ++count;
            }
        }
        return count;
    };
    assert(countCapVertices(cappedSplit.first, glm::vec3(-1.f, 0.f, 0.f)) == 6u);
    assert(countCapVertices(cappedSplit.second, glm::vec3(1.f, 0.f, 0.f)) == 6u);
    const auto assertCapWinding = [](const std::vector<Mesh::Vertex>& half,
        const glm::vec3& expectedNormal)
    {
        size_t capTriangleCount = 0;
        for (size_t index = 0; index + 2u < half.size(); index += 3u)
        {
            const glm::vec3 a(half[index].pos[0], half[index].pos[1], half[index].pos[2]);
            const glm::vec3 b(half[index + 1u].pos[0], half[index + 1u].pos[1], half[index + 1u].pos[2]);
            const glm::vec3 c(half[index + 2u].pos[0], half[index + 2u].pos[1], half[index + 2u].pos[2]);
            if (std::abs(a.x) < 0.0001f && std::abs(b.x) < 0.0001f &&
                std::abs(c.x) < 0.0001f)
            {
                assert(glm::dot(glm::cross(b - a, c - a), expectedNormal) > 0.001f);
                ++capTriangleCount;
            }
        }
        assert(capTriangleCount == 2u);
    };
    assertCapWinding(cappedSplit.first, glm::vec3(-1.f, 0.f, 0.f));
    assertCapWinding(cappedSplit.second, glm::vec3(1.f, 0.f, 0.f));

    SpatialManipulator sourcePortal;
    SpatialManipulator targetPortal;
    sourcePortal.portalPoint = glm::vec3(0.f, 0.f, 0.f);
    sourcePortal.portalNormal = glm::vec3(0.f, 0.f, 1.f);
    sourcePortal.portalPointCount = 4;

    targetPortal.portalPoint = glm::vec3(0.f, 0.f, 0.f);
    targetPortal.portalNormal = glm::vec3(0.f, 0.f, 1.f);
    targetPortal.portalPointCount = 4;
    targetPortal.portalShapePoint0 = glm::vec3(-1.f, -1.f, 0.f);
    targetPortal.portalShapePoint1 = glm::vec3(1.f, -1.f, 0.f);
    targetPortal.portalShapePoint2 = glm::vec3(1.f, 1.f, 0.f);
    targetPortal.portalShapePoint3 = glm::vec3(-1.f, 1.f, 0.f);

    assert(sourcePortal.HasCompatiblePortalShapeWith(targetPortal));
    assert(sourcePortal.IsValidPortalAperture());
    assert(sourcePortal.IsWorldPointInsidePortalAperture(
        glm::vec3(0.f, 0.f, 0.f)));
    assert(!sourcePortal.IsWorldPointInsidePortalAperture(
        glm::vec3(0.75f, 0.f, 0.f)));
    const glm::vec3 mapped = sourcePortal.MapWorldPointThroughPortalShape(
        glm::vec3(0.25f, 0.25f, 0.f), targetPortal);
    // A portal crossing is a half-turn in portal-local coordinates and the
    // point offset carries the 2x aperture ratio into the target chart.
    assert(glm::length(mapped - glm::vec3(-0.5f, 0.5f, 0.f)) < 0.05f);

    // Moving one target corner activates the adaptive piecewise path. Under
    // the portal half-turn, the source top-left corresponds to target
    // top-right and must land on the authored raised corner exactly.
    targetPortal.portalShapePoint2 = glm::vec3(1.f, 2.f, 0.f);
    assert(sourcePortal.UsesPiecewisePortalWarpTo(targetPortal));
    const glm::vec3 mappedRaisedCorner =
        sourcePortal.MapWorldPointThroughPortalShape(
            sourcePortal.portalShapePoint3, targetPortal);
    assert(glm::length(mappedRaisedCorner - targetPortal.portalShapePoint2) <
        0.002f);
    // Other corresponding corners remain pinned rather than being displaced
    // by a single global area-derived scale.
    const glm::vec3 mappedFixedCorner =
        sourcePortal.MapWorldPointThroughPortalShape(
            sourcePortal.portalShapePoint0, targetPortal);
    assert(glm::length(mappedFixedCorner - targetPortal.portalShapePoint1) <
        0.002f);
    targetPortal.portalShapePoint2 = glm::vec3(1.f, 1.f, 0.f);
    assert(!sourcePortal.UsesPiecewisePortalWarpTo(targetPortal));

    sourcePortal.portalShapePoint2 = glm::vec3(-0.25f, 0.f, 0.f);
    assert(!sourcePortal.IsValidPortalAperture());
    sourcePortal.portalShapePoint2 = glm::vec3(0.5f, 0.5f, 0.f);

    targetPortal.portalPointCount = 3;
    assert(!sourcePortal.HasCompatiblePortalShapeWith(targetPortal));

    Engine::Scene::Scene scene;
    Engine::Core::Object* sourceObject = scene.AddObject("WarpSource");
    Engine::Core::Object* targetObject = scene.AddObject("WarpTarget");
    auto* source = sourceObject->AddComponent<SpatialManipulator>();
    auto* target = targetObject->AddComponent<SpatialManipulator>();
    Engine::Core::Object* sourceContent = scene.AddObject("SourceOverlayContent");
    Engine::Core::Object* targetContent = scene.AddObject("TargetOverlayContent");
    Engine::Core::Object* unrelatedContent = scene.AddObject("UnrelatedContent");
    assert(scene.MoveObject(sourceContent, sourceObject,
        Engine::Scene::Scene::ObjectPlacement::AsChild));
    assert(scene.MoveObject(targetContent, targetObject,
        Engine::Scene::Scene::ObjectPlacement::AsChild));

    source->connectionMode = static_cast<int>(SpatialManipulator::ConnectionMode::MatrixOverlay);
    source->position = glm::vec3(2.f, 0.f, 0.f);
    target->position = glm::vec3(4.f, 0.f, 0.f);
    source->ConnectToTarget(target);
    assert(source->ResolveTarget() == target);
    assert(target->ResolveTarget() == source);
    source->Update();
    assert(sourceObject->transform.matrixLayer.enabled);
    assert(targetObject->transform.matrixLayer.enabled);
    // A matrix link owns both declared hierarchies, not just the two carrier
    // transforms. The unrelated object remains in the ordinary scene layer.
    assert(sourceContent->transform.matrixLayer.enabled);
    assert(targetContent->transform.matrixLayer.enabled);
    assert(!unrelatedContent->transform.matrixLayer.enabled);
    const glm::mat4 sourceScopeMatrix = target->GetOverlayMatrix() *
        glm::inverse(source->GetOverlayMatrix());
    assert(glm::length(glm::vec3(sourceContent->transform.matrixLayer.localToLayer[3]) -
        glm::vec3(sourceScopeMatrix[3])) < 0.0002f);

    source->enabled = false;
    source->Update();
    assert(!sourceObject->transform.matrixLayer.enabled);
    assert(!sourceObject->transform.matrixLayer.connection.enabled);
    assert(!targetObject->transform.matrixLayer.enabled);
    assert(!targetObject->transform.matrixLayer.connection.enabled);
    assert(!sourceContent->transform.matrixLayer.enabled);
    assert(!targetContent->transform.matrixLayer.enabled);
    assert(sourceObject->transform.matrixLayer.localToLayer == glm::mat4(1.f));
    assert(targetObject->transform.matrixLayer.localToLayer == glm::mat4(1.f));

    source->enabled = true;
    target->enabled = true;
    source->connectionMode = static_cast<int>(SpatialManipulator::ConnectionMode::Portal);
    source->Update();
    assert(sourceObject->transform.matrixLayer.connection.enabled);
    assert(targetObject->transform.matrixLayer.connection.enabled);

    target->enabled = false;
    source->Update();
    assert(!sourceObject->transform.matrixLayer.enabled);
    assert(!sourceObject->transform.matrixLayer.connection.enabled);
    assert(!targetObject->transform.matrixLayer.enabled);
    assert(!targetObject->transform.matrixLayer.connection.enabled);

    source->enabled = true;
    target->enabled = true;
    sourceObject->transform.position = glm::vec3(2.f, 3.f, 4.f);
    sourceObject->transform.rotation = glm::vec3(0.f);
    targetObject->transform.position = glm::vec3(-5.f, 1.f, 7.f);
    targetObject->transform.rotation = glm::vec3(0.f, glm::radians(90.f), 0.f);
    source->connectionMode = static_cast<int>(SpatialManipulator::ConnectionMode::Portal);
    source->portalPointCount = 4;
    target->portalPointCount = 4;
    source->portalPoint = glm::vec3(1.f, -0.5f, 0.25f);
    target->portalPoint = glm::vec3(-0.75f, 0.4f, -0.2f);
    target->portalShapePoint0 = glm::vec3(-1.f, -1.f, 0.f);
    target->portalShapePoint1 = glm::vec3(1.f, -1.f, 0.f);
    target->portalShapePoint2 = glm::vec3(1.f, 1.f, 0.f);
    target->portalShapePoint3 = glm::vec3(-1.f, 1.f, 0.f);

    const glm::mat4 sourceToTarget = source->GetPortalWorldTransformTo(*target);
    const glm::mat4 targetToSource = target->GetPortalWorldTransformTo(*source);
    const glm::mat4 roundTrip = targetToSource * sourceToTarget;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            assert(std::abs(roundTrip[column][row] - (column == row ? 1.f : 0.f)) < 0.0002f);

    const glm::vec3 sourceAnchor = glm::vec3(sourceObject->transform.GetWorldMatrix() *
        glm::vec4(source->portalPoint, 1.f));
    const glm::vec3 targetAnchor = glm::vec3(targetObject->transform.GetWorldMatrix() *
        glm::vec4(target->portalPoint, 1.f));
    const glm::vec3 mappedAnchor = glm::vec3(sourceToTarget *
        glm::vec4(sourceAnchor, 1.f));
    assert(glm::length(mappedAnchor - targetAnchor) < 0.0002f);

    const glm::vec3 sourceNormal(0.f, 0.f, 1.f);
    const glm::vec3 targetNormal(1.f, 0.f, 0.f);
    const glm::vec3 mappedNormal = glm::vec3(sourceToTarget *
        glm::vec4(sourceNormal, 0.f));
    assert(glm::length(mappedNormal + targetNormal *
        source->GetPortalScaleRatioTo(*target)) < 0.0002f);
    // A virtual viewer on the source normal side maps behind the target and
    // continues through its aperture into the connected space.
    const glm::vec3 sourceViewer = sourceAnchor + sourceNormal * 2.f;
    const glm::vec3 mappedViewer = glm::vec3(sourceToTarget *
        glm::vec4(sourceViewer, 1.f));
    const glm::vec3 mappedLookPoint = glm::vec3(sourceToTarget *
        glm::vec4(sourceViewer - sourceNormal, 1.f));
    assert(glm::dot(mappedViewer - targetAnchor, targetNormal) < -1.9f);
    assert(glm::dot(mappedLookPoint - mappedViewer, targetNormal) > 0.9f);
    const glm::vec3 mappedTangent = glm::vec3(sourceToTarget *
        glm::vec4(1.f, 0.f, 0.f, 0.f));
    const glm::vec3 mappedBitangent = glm::vec3(sourceToTarget *
        glm::vec4(0.f, 1.f, 0.f, 0.f));
    const float portalScaleRatio = source->GetPortalScaleRatioTo(*target);
    assert(glm::length(mappedTangent -
        glm::vec3(0.f, 0.f, portalScaleRatio)) < 0.0002f);
    assert(glm::length(mappedBitangent -
        glm::vec3(0.f, portalScaleRatio, 0.f)) < 0.0002f);
    assert(glm::determinant(glm::mat3(sourceToTarget)) > 0.f);

    // A query ray must stop at the source aperture, continue from the mapped
    // target anchor, and carry its direction through the same relative frame
    // used by rigid-body traversal and virtual cameras.
    const auto portalRaySegments = scene.TracePortalRay(
        { sourceAnchor - sourceNormal * 2.f, sourceNormal }, 12.f, 4u);
    assert(portalRaySegments.size() >= 2u);
    assert(std::abs(portalRaySegments[0].maxDistance - 2.f) < 0.002f);
    assert(portalRaySegments[0].enteredPortal == sourceObject);
    assert(glm::length(portalRaySegments[1].ray.direction -
        glm::normalize(mappedNormal)) < 0.0002f);
    assert(glm::length(portalRaySegments[1].ray.origin - targetAnchor) <
        0.01f);
    const auto nonTraversingRaySegments = scene.TracePortalRay(
        { sourceAnchor - sourceNormal * 2.f, sourceNormal }, 12.f, 0u);
    assert(nonTraversingRaySegments.size() == 1u);
    assert(std::abs(nonTraversingRaySegments[0].maxDistance - 12.f) <
        0.0002f);

    source->Update();
    const auto& sourceConnection = sourceObject->transform.matrixLayer.connection;
    const auto& targetConnection = targetObject->transform.matrixLayer.connection;
    assert(glm::length(sourceConnection.boundaryPoint - sourceAnchor) < 0.0002f);
    assert(glm::length(targetConnection.boundaryPoint - targetAnchor) < 0.0002f);
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
        {
            assert(std::abs(sourceConnection.localToRemote[column][row] -
                sourceToTarget[column][row]) < 0.0002f);
            assert(std::abs(targetConnection.localToRemote[column][row] -
                targetToSource[column][row]) < 0.0002f);
        }

    source->Disconnect();
    assert(!source->HasTarget());
    assert(!target->HasTarget());
    assert(!sourceObject->transform.matrixLayer.connection.enabled);
    assert(!targetObject->transform.matrixLayer.connection.enabled);

    source->ConnectToTarget(target);
    source->Update();
    target->enabled = false;
    target->Update();
    assert(!sourceObject->transform.matrixLayer.connection.enabled);
    assert(!targetObject->transform.matrixLayer.connection.enabled);
    target->enabled = true;

    Engine::Core::Object* volumeObject = scene.AddObject("SpiralWarpVolume");
    auto* volume = volumeObject->AddComponent<SpatialManipulator>();
    volume->definesWarpVolume = true;
    volume->warpVolumeShape = static_cast<int>(
        SpatialManipulator::WarpVolumeShape::Box);
    volume->warpVolumeSize = glm::vec3(10.f);
    volume->spaceWarpType = static_cast<int>(
        SpatialManipulator::SpaceWarpType::Formula);
    volume->formulaA = 1.57079632679f;
    volume->formulaX = "cos(a*y)*x - sin(a*y)*z";
    volume->formulaY = "y";
    volume->formulaZ = "sin(a*y)*x + cos(a*y)*z";
    volume->scale = glm::vec3(0.5f, 1.f, 0.5f);

    Engine::Core::Object* insideVolume = scene.AddObject("InsideWarpVolume");
    insideVolume->transform.position = glm::vec3(1.f, 2.f, 0.f);
    const glm::vec3 warpedInside = glm::vec3(
        insideVolume->transform.GetWorldMatrixWithLayer()[3]);
    assert(glm::length(warpedInside - glm::vec3(-0.5f, 2.f, 0.f)) < 0.002f);

    Engine::Core::Object* outsideVolume = scene.AddObject("OutsideWarpVolume");
    outsideVolume->transform.position = glm::vec3(1.f, 6.f, 0.f);
    const glm::vec3 warpedOutside = glm::vec3(
        outsideVolume->transform.GetWorldMatrixWithLayer()[3]);
    assert(glm::length(warpedOutside - outsideVolume->transform.position) < 0.0002f);

    const glm::vec3 mappedByScene = scene.MapSpatialPoint(glm::vec3(1.f, 2.f, 0.f),
        { Engine::Scene::Scene::SpatialQueryDomain::Gameplay });
    assert(glm::length(mappedByScene - warpedInside) < 0.0002f);

    // Portal aperture geometry and its virtual camera ray must occupy the
    // same rendering chart as the warped scene objects. The source endpoint
    // is inside this nonlinear volume while the target remains outside it.
    const glm::mat4 rawSourceFrame = source->GetPortalWorldFrame();
    const glm::mat4 renderSourceFrame = source->GetRenderPortalWorldFrame();
    const glm::mat4 renderTargetFrame = target->GetRenderPortalWorldFrame();
    const glm::vec3 rawSourceAnchor(rawSourceFrame[3]);
    const glm::vec3 expectedRenderSourceAnchor = scene.MapSpatialPoint(
        rawSourceAnchor, { Engine::Scene::Scene::SpatialQueryDomain::Rendering,
            sourceObject });
    assert(glm::length(glm::vec3(renderSourceFrame[3]) -
        expectedRenderSourceAnchor) < 0.002f);
    assert(glm::length(glm::vec3(renderSourceFrame[3]) - rawSourceAnchor) > 0.01f);

    const glm::mat4 renderSourceToTarget =
        source->GetRenderPortalWorldTransformTo(*target);
    const glm::vec3 mappedRenderAnchor = glm::vec3(renderSourceToTarget *
        glm::vec4(renderSourceFrame[3]));
    assert(glm::length(mappedRenderAnchor - glm::vec3(renderTargetFrame[3])) <
        0.002f);
    const glm::vec3 renderViewer = glm::vec3(renderSourceFrame[3]) +
        glm::vec3(renderSourceFrame[2]) * 2.f;
    const glm::vec3 mappedRenderViewer =
        source->MapRenderWorldPointThroughPortalShape(renderViewer, *target);
    const glm::vec3 mappedRenderForward = glm::normalize(
        source->MapRenderWorldPointThroughPortalShape(renderViewer -
            glm::vec3(renderSourceFrame[2]), *target) - mappedRenderViewer);
    // A viewer in front of the source maps behind the target plane and looks
    // through its opening. Rendering and physical traversal use the same
    // portal-local half-turn.
    assert(glm::dot(mappedRenderViewer - glm::vec3(renderTargetFrame[3]),
        glm::vec3(renderTargetFrame[2])) < -1.9f);
    assert(glm::dot(mappedRenderForward, glm::vec3(renderTargetFrame[2])) > 0.9f);

    // Rendering, camera, audio, physics, raycast, and gameplay now consume
    // one nonlinear spatial-query contract rather than separate affine paths.
    const auto spatialSample = scene.SampleSpatialPoint(glm::vec3(1.f, 2.f, 0.f),
        { Engine::Scene::Scene::SpatialQueryDomain::Physics });
    assert(spatialSample.affectedByWarpVolume);
    assert(glm::length(spatialSample.point - mappedByScene) < 0.0002f);
    assert(glm::length(spatialSample.jacobian[0]) > 0.5f);
    const Engine::Scene::Scene::SpatialRay mappedRay = scene.MapSpatialRay(
        { glm::vec3(1.f, 2.f, 0.f), glm::vec3(1.f, 0.f, 0.f) },
        { Engine::Scene::Scene::SpatialQueryDomain::Raycast });
    assert(glm::length(mappedRay.origin - mappedByScene) < 0.0002f);
    assert(glm::length(mappedRay.direction) > 0.999f);

    volume->formulaX = "sqrt(-1)";
    assert(glm::length(scene.WarpWorldPoint(glm::vec3(1.f, 2.f, 0.f)) -
        glm::vec3(1.f, 2.f, 0.f)) < 0.0002f);
    volume->formulaX = "cos(a*y)*x - sin(a*y)*z";

    // Overlapping volume composition is priority ordered, rather than relying
    // on object insertion/update order. Scale and translation do not commute.
    Engine::Scene::Scene priorityScene;
    Engine::Core::Object* lowVolumeObject = priorityScene.AddObject("LowPriorityVolume");
    Engine::Core::Object* highVolumeObject = priorityScene.AddObject("HighPriorityVolume");
    auto* lowVolume = lowVolumeObject->AddComponent<SpatialManipulator>();
    auto* highVolume = highVolumeObject->AddComponent<SpatialManipulator>();
    lowVolume->definesWarpVolume = true;
    highVolume->definesWarpVolume = true;
    lowVolume->spaceWarpType = static_cast<int>(SpatialManipulator::SpaceWarpType::Affine);
    highVolume->spaceWarpType = static_cast<int>(SpatialManipulator::SpaceWarpType::Affine);
    lowVolume->scale = glm::vec3(2.f);
    highVolume->position = glm::vec3(1.f, 0.f, 0.f);
    lowVolume->warpPriority = 0;
    highVolume->warpPriority = 10;
    assert(glm::length(priorityScene.MapSpatialPoint(glm::vec3(1.f, 0.f, 0.f)) -
        glm::vec3(3.f, 0.f, 0.f)) < 0.0002f);
    lowVolume->warpPriority = 10;
    highVolume->warpPriority = 0;
    assert(glm::length(priorityScene.MapSpatialPoint(glm::vec3(1.f, 0.f, 0.f)) -
        glm::vec3(4.f, 0.f, 0.f)) < 0.0002f);

    Engine::Scene::Scene showcase;
    assert(Engine::Serialization::SceneSerializer::Load(showcase,
        "Engine/Core/Assets/Scenes/spiral_warp_column.scene", nullptr));
    Engine::Core::Object* loadedVolume = nullptr;
    Engine::Core::Object* loadedFallingBody = nullptr;
    for (const auto& object : showcase.GetObjects())
    {
        if (object->name == "Spiral Warp Volume") loadedVolume = object.get();
        if (object->name == "Falling Warp Sphere") loadedFallingBody = object.get();
    }
    assert(loadedVolume && loadedFallingBody);
    const auto* loadedManipulator = loadedVolume->GetComponent<SpatialManipulator>();
    assert(loadedManipulator && loadedManipulator->definesWarpVolume);
    assert(static_cast<SpatialManipulator::SpaceWarpType>(
        loadedManipulator->spaceWarpType) == SpatialManipulator::SpaceWarpType::Formula);
    assert(glm::length(glm::vec3(loadedFallingBody->transform.GetWorldMatrixWithLayer()[3]) -
        loadedFallingBody->transform.GetWorldPosition()) > 0.1f);

    // The shrinking-tunnel showcase is one continuous mesh. Its inverse
    // exponential chart must flatten both physical ends into the same optical
    // square, which is the mapping the renderer now evaluates per vertex.
    Engine::Scene::Scene shrinkingTunnel;
    assert(Engine::Serialization::SceneSerializer::Load(shrinkingTunnel,
        "Engine/Core/Assets/Scenes/smooth_shrinking_tunnel.scene", nullptr));
    const auto renderQuery = Engine::Scene::Scene::SpatialQuery {
        Engine::Scene::Scene::SpatialQueryDomain::Rendering };
    const glm::vec3 opticalEntrance = shrinkingTunnel.MapSpatialPoint(
        glm::vec3(4.5f, 4.5f, 0.f), renderQuery);
    const glm::vec3 opticalExit = shrinkingTunnel.MapSpatialPoint(
        glm::vec3(1.125f, 4.5f, 10.f), renderQuery);
    const glm::vec3 opticalExitFloor = shrinkingTunnel.MapSpatialPoint(
        glm::vec3(0.f, 3.375f, 10.f), renderQuery);
    assert(glm::length(opticalEntrance - glm::vec3(4.5f, 4.5f, 0.f)) < 0.002f);
    assert(glm::length(opticalExit - glm::vec3(4.5f, 4.5f, 10.f)) < 0.002f);
    assert(glm::length(opticalExitFloor - glm::vec3(0.f, 0.f, 10.f)) < 0.002f);

    // Trace three physical geodesic samples through the tunnel. In physical
    // space their lateral position contracts exponentially; mapping every
    // point through the render chart produces straight, parallel optical
    // rays. This is the same condition the per-vertex tunnel mesh satisfies.
    constexpr float tunnelExponent = 0.138629436f;
    for (const float opticalX : { -3.f, 0.f, 3.f })
    {
        for (int sample = 0; sample <= 40; ++sample)
        {
            const float z = static_cast<float>(sample) * 0.25f;
            const glm::vec3 physicalRayPoint(opticalX *
                std::exp(-tunnelExponent * z), 4.5f, z);
            const glm::vec3 opticalRayPoint = shrinkingTunnel.MapSpatialPoint(
                physicalRayPoint, renderQuery);
            assert(glm::length(opticalRayPoint -
                glm::vec3(opticalX, 4.5f, z)) < 0.003f);
        }
    }

    // Camera/render sampling must preserve the straight optical chart from
    // either end. Under perspective projection, every mapped longitudinal
    // corner of the physically tapered mesh must remain on one straight
    // screen-space line, and the mapped center ray must remain centered.
    Engine::Core::Object* tunnelCameraObject = shrinkingTunnel.FindObjectByName(
        "Shrinking Tunnel Camera");
    Camera* tunnelCamera = tunnelCameraObject
        ? tunnelCameraObject->GetComponent<Camera>() : nullptr;
    assert(tunnelCamera);
    Engine::Core::Object* reverseCameraObject = shrinkingTunnel.AddObject(
        "Shrinking Tunnel Reverse Test Camera");
    reverseCameraObject->transform.position = { 0.f, 4.5f, 16.f };
    Camera* reverseTunnelCamera = reverseCameraObject->AddComponent<Camera>();
    reverseTunnelCamera->active = false;
    reverseTunnelCamera->useTransformRotation = false;
    reverseTunnelCamera->target = { 0.f, 4.5f, 5.f };
    reverseTunnelCamera->up = { 0.f, 1.f, 0.f };
    reverseTunnelCamera->fov = tunnelCamera->fov;
    reverseTunnelCamera->nearPlane = tunnelCamera->nearPlane;
    reverseTunnelCamera->farPlane = tunnelCamera->farPlane;

    const auto projectTunnelSample = [&](const Camera& camera,
        const glm::vec3& physicalPoint)
    {
        const glm::vec3 opticalPoint = shrinkingTunnel.MapSpatialPoint(
            physicalPoint, renderQuery);
        const glm::vec4 clip = camera.GetProjectionMatrix(16.f / 9.f) *
            camera.GetViewMatrix() * glm::vec4(opticalPoint, 1.f);
        assert(std::abs(clip.w) > 1e-6f);
        return glm::vec2(clip) / clip.w;
    };
    const auto validateStraightTunnelView = [&](const Camera& camera)
    {
        float maximumCenterRayError = 0.f;
        float maximumProjectedEdgeError = 0.f;
        for (int step = 0; step <= 40; ++step)
        {
            const float z = static_cast<float>(step) * 0.25f;
            maximumCenterRayError = std::max(maximumCenterRayError,
                glm::length(projectTunnelSample(camera,
                    glm::vec3(0.f, 4.5f, z))));
        }
        for (const float xSign : { -1.f, 1.f })
        {
            for (const float ySign : { -1.f, 1.f })
            {
                const auto physicalCorner = [&](float z)
                {
                    const float halfExtent = 4.5f *
                        std::exp(-tunnelExponent * z);
                    return glm::vec3(xSign * halfExtent,
                        4.5f + ySign * halfExtent, z);
                };
                const glm::vec2 projectedStart = projectTunnelSample(camera,
                    physicalCorner(0.f));
                const glm::vec2 projectedEnd = projectTunnelSample(camera,
                    physicalCorner(10.f));
                const glm::vec2 projectedLine = projectedEnd - projectedStart;
                const float projectedLength = glm::length(projectedLine);
                assert(projectedLength > 1e-6f);
                for (int step = 1; step < 40; ++step)
                {
                    const float z = static_cast<float>(step) * 0.25f;
                    const glm::vec2 projected = projectTunnelSample(camera,
                        physicalCorner(z));
                    const glm::vec2 delta = projected - projectedStart;
                    const float distance = std::abs(projectedLine.x * delta.y -
                        projectedLine.y * delta.x) / projectedLength;
                    maximumProjectedEdgeError = std::max(
                        maximumProjectedEdgeError, distance);
                }
            }
        }
        assert(maximumCenterRayError < 0.0002f);
        assert(maximumProjectedEdgeError < 0.0003f);
        return std::array<float, 2> {
            maximumCenterRayError, maximumProjectedEdgeError };
    };
    const auto entranceViewErrors = validateStraightTunnelView(*tunnelCamera);
    const auto exitViewErrors = validateStraightTunnelView(*reverseTunnelCamera);
    std::cout << "shrinking_tunnel_camera_rays entrance_center_error="
        << entranceViewErrors[0] << " entrance_edge_error="
        << entranceViewErrors[1] << " exit_center_error="
        << exitViewErrors[0] << " exit_edge_error=" << exitViewErrors[1]
        << '\n';

    // Traversal scale is a warp-volume capability, not tunnel-scene script
    // behavior. It carries the transverse inverse metric onto a dynamic body,
    // samples the exact positive boundary on exit, and never rewrites mesh or
    // morph data.
    Engine::Core::Object* tunnelVolume = shrinkingTunnel.FindObjectByName(
        "Smooth Shrinking Tunnel Warp");
    Engine::Core::Object* scaleProbe = shrinkingTunnel.FindObjectByName(
        "Persistent Scale Probe");
    assert(tunnelVolume && scaleProbe);
    SpatialManipulator* tunnelWarp = tunnelVolume->GetComponent<SpatialManipulator>();
    Mesh* probeMesh = scaleProbe->GetComponent<Mesh>();
    assert(tunnelWarp && tunnelWarp->applyTraversalScale &&
        tunnelWarp->persistTraversalScaleOnExit && probeMesh);
    const uint32_t sourceVertexCount = probeMesh->GetVertexCount();
    const glm::vec3 sourceVertex(probeMesh->GetVertices()[0].pos[0],
        probeMesh->GetVertices()[0].pos[1], probeMesh->GetVertices()[0].pos[2]);
    Mesh::MorphTarget probeMorph;
    probeMorph.positions.resize(sourceVertexCount, glm::vec3(0.f));
    probeMesh->SetMorphData(0u, { std::move(probeMorph) }, { 0.65f });

    tunnelWarp->Update(); // Records the probe before it enters at local -Z.
    scaleProbe->transform.position = { 0.f, 4.5f, 4.f };
    tunnelWarp->Update();
    assert(glm::length(scaleProbe->transform.scale - glm::vec3(
        std::exp(-tunnelExponent * 4.f))) < 0.003f);

    scaleProbe->transform.position = { 0.f, 4.5f, 10.1f };
    tunnelWarp->Update();
    assert(glm::length(scaleProbe->transform.scale - glm::vec3(0.25f)) < 0.003f);
    assert(probeMesh->GetVertexCount() == sourceVertexCount);
    assert(glm::length(glm::vec3(probeMesh->GetVertices()[0].pos[0],
        probeMesh->GetVertices()[0].pos[1], probeMesh->GetVertices()[0].pos[2]) -
        sourceVertex) < 0.0001f);
    assert(probeMesh->GetMorphWeights().size() == 1u &&
        std::abs(probeMesh->GetMorphWeights()[0] - 0.65f) < 0.0001f);

    // Move a fresh probe continuously through the authored scene instead of
    // checking only two teleported samples. Its physical scale contracts with
    // the tunnel, while the nonlinear render chart expands both the probe and
    // wall back to constant optical sizes. This catches double-deformation and
    // object-vs-environment chart mismatches.
    Engine::Scene::Scene movingTunnel;
    assert(Engine::Serialization::SceneSerializer::Load(movingTunnel,
        "Engine/Core/Assets/Scenes/smooth_shrinking_tunnel.scene", nullptr));
    Engine::Core::Object* movingVolume = movingTunnel.FindObjectByName(
        "Smooth Shrinking Tunnel Warp");
    Engine::Core::Object* movingProbe = movingTunnel.FindObjectByName(
        "Persistent Scale Probe");
    assert(movingVolume && movingProbe);
    SpatialManipulator* movingWarp =
        movingVolume->GetComponent<SpatialManipulator>();
    assert(movingWarp);

    const auto movingRenderQuery = Engine::Scene::Scene::SpatialQuery {
        Engine::Scene::Scene::SpatialQueryDomain::Rendering, movingProbe };
    float maximumScaleError = 0.f;
    float maximumOpticalSizeError = 0.f;
    float maximumRelativeSizeError = 0.f;
    movingProbe->transform.position = { 0.f, 4.5f, -1.5f };
    movingWarp->Update();
    for (int step = 0; step <= 40; ++step)
    {
        const float z = static_cast<float>(step) * 0.25f;
        movingProbe->transform.position = { 0.f, 4.5f, z };
        movingWarp->Update();

        const float expectedScale = std::exp(-tunnelExponent * z);
        maximumScaleError = std::max(maximumScaleError,
            glm::length(movingProbe->transform.scale -
                glm::vec3(expectedScale)));

        const glm::vec3 physicalCenter = movingProbe->transform.GetWorldPosition();
        const glm::vec3 physicalEdge = glm::vec3(
            movingProbe->transform.GetWorldMatrix() *
            glm::vec4(0.5f, 0.f, 0.f, 1.f));
        const glm::vec3 opticalCenter = movingTunnel.MapSpatialPoint(
            physicalCenter, movingRenderQuery);
        const glm::vec3 opticalEdge = movingTunnel.MapSpatialPoint(
            physicalEdge, movingRenderQuery);
        const float opticalHalfWidth = glm::length(opticalEdge - opticalCenter);
        maximumOpticalSizeError = std::max(maximumOpticalSizeError,
            std::abs(opticalHalfWidth - 0.5f));

        const glm::vec3 physicalWall(4.5f * expectedScale, 4.5f, z);
        const glm::vec3 opticalWall = movingTunnel.MapSpatialPoint(
            physicalWall, movingRenderQuery);
        const float opticalTunnelHalfWidth = glm::length(
            opticalWall - opticalCenter);
        const float relativeHalfWidth = opticalHalfWidth /
            opticalTunnelHalfWidth;
        maximumRelativeSizeError = std::max(maximumRelativeSizeError,
            std::abs(relativeHalfWidth - (0.5f / 4.5f)));
    }
    assert(maximumScaleError < 0.003f);
    assert(maximumOpticalSizeError < 0.003f);
    assert(maximumRelativeSizeError < 0.0005f);

    movingProbe->transform.position = { 0.f, 4.5f, 10.5f };
    movingWarp->Update();
    const float persistedScale = movingProbe->transform.scale.x;
    const float exitRelativeHalfWidth = (0.5f * persistedScale) / 1.125f;
    assert(std::abs(persistedScale - 0.25f) < 0.003f);
    assert(std::abs(exitRelativeHalfWidth - (0.5f / 4.5f)) < 0.0005f);
    std::cout << "shrinking_tunnel_traversal samples=41 max_scale_error="
        << maximumScaleError << " max_optical_size_error="
        << maximumOpticalSizeError << " max_relative_size_error="
        << maximumRelativeSizeError << " exit_scale=" << persistedScale
        << '\n';

    // Finally run the scene lifecycle and Bullet simulation exactly as the
    // game does. The authored +Z velocity must carry the probe across both
    // volume boundaries and leave the exit deformation persisted.
    Engine::Scene::Scene simulatedTunnel;
    assert(Engine::Serialization::SceneSerializer::Load(simulatedTunnel,
        "Engine/Core/Assets/Scenes/smooth_shrinking_tunnel.scene", nullptr));
    Engine::Core::Object* simulatedProbe = simulatedTunnel.FindObjectByName(
        "Persistent Scale Probe");
    assert(simulatedProbe && simulatedProbe->GetComponent<RigidBody>());
    simulatedTunnel.Start();
    bool crossedEntrance = false;
    bool reachedMidpoint = false;
    bool crossedExit = false;
    int entranceFrame = -1;
    int exitFrame = -1;
    for (int frame = 0; frame < 220; ++frame)
    {
        simulatedTunnel.Update(1.f / 60.f);
        const float z = simulatedProbe->transform.GetWorldPosition().z;
        if (entranceFrame < 0 && z >= 0.f)
            entranceFrame = frame;
        if (exitFrame < 0 && z > 10.f)
            exitFrame = frame;
        crossedEntrance = crossedEntrance || z >= 0.f;
        reachedMidpoint = reachedMidpoint || z >= 5.f;
        crossedExit = crossedExit || z > 10.f;
    }
    const float simulatedExitZ =
        simulatedProbe->transform.GetWorldPosition().z;
    const float simulatedExitScale = simulatedProbe->transform.scale.x;
    assert(crossedEntrance && reachedMidpoint && crossedExit);
    assert(simulatedExitZ > 10.f);
    assert(std::abs(simulatedExitScale - 0.25f) < 0.003f);
    assert(entranceFrame >= 0 && exitFrame > entranceFrame);
    const float traversalSeconds = static_cast<float>(
        exitFrame - entranceFrame) / 60.f;
    assert(traversalSeconds >= 2.f && traversalSeconds <= 3.f);
    std::cout << "shrinking_tunnel_runtime frames=220 entrance="
        << crossedEntrance << " midpoint=" << reachedMidpoint << " exit="
        << crossedExit << " final_z=" << simulatedExitZ << " exit_scale="
        << simulatedExitScale << " traversal_seconds=" << traversalSeconds
        << '\n';

    // Three-room scene fixtures share the same directed navigation loop:
    // north door 1 -> west door 2, north 2 -> west 3, north 3 -> west 1.
    // Each room's west/north openings are on adjacent (orthogonal) walls.
    const auto validateThreeRoomPortalLoop = [](Engine::Scene::Scene& roomScene,
        const std::string& prefix)
    {
        const std::array<std::pair<std::string, std::string>, 3> links {{
            { prefix + "Room 1 North Exit", prefix + "Room 2 West Entrance" },
            { prefix + "Room 2 North Exit", prefix + "Room 3 West Entrance" },
            { prefix + "Room 3 North Exit", prefix + "Room 1 West Entrance" }
        }};
        for (const auto& [sourceName, targetName] : links)
        {
            Engine::Core::Object* sourceObject =
                roomScene.FindObjectByName(sourceName);
            Engine::Core::Object* targetObject =
                roomScene.FindObjectByName(targetName);
            assert(sourceObject && targetObject);
            SpatialManipulator* source =
                sourceObject->GetComponent<SpatialManipulator>();
            SpatialManipulator* target =
                targetObject->GetComponent<SpatialManipulator>();
            assert(source && target && source->ResolveTarget() == target &&
                target->ResolveTarget() == source);
        }
        for (int room = 1; room <= 3; ++room)
        {
            Engine::Core::Object* westObject = roomScene.FindObjectByName(
                prefix + "Room " + std::to_string(room) + " West Entrance");
            Engine::Core::Object* northObject = roomScene.FindObjectByName(
                prefix + "Room " + std::to_string(room) + " North Exit");
            assert(westObject && northObject);
            const glm::vec3 westNormal = glm::vec3(westObject->GetComponent<
                SpatialManipulator>()->GetRenderPortalWorldFrame()[2]);
            const glm::vec3 northNormal = glm::vec3(northObject->GetComponent<
                SpatialManipulator>()->GetRenderPortalWorldFrame()[2]);
            assert(std::abs(glm::dot(westNormal, northNormal)) < 0.001f);
        }
    };

    Engine::Scene::Scene stackedRooms;
    assert(Engine::Serialization::SceneSerializer::Load(stackedRooms,
        "Engine/Core/Assets/Scenes/three_room_portal_stack_loop.scene", nullptr));
    validateThreeRoomPortalLoop(stackedRooms, "");
    const std::array<const char*, 3> stackedShellNames {
        "Room 1 Red Shell", "Room 2 Green Shell", "Room 3 Blue Shell" };
    std::array<glm::vec3, 3> roomColors {};
    for (size_t room = 0; room < stackedShellNames.size(); ++room)
    {
        Engine::Core::Object* shell = stackedRooms.FindObjectByName(
            stackedShellNames[room]);
        assert(shell && shell->GetComponent<Mesh>() &&
            shell->GetComponent<MeshObjectCollider>());
        Material* roomMaterial = shell->GetComponent<Material>();
        assert(roomMaterial);
        roomColors[room] = roomMaterial->diffuseColor;
    }
    assert(glm::length(roomColors[0] - roomColors[1]) > 0.5f &&
        glm::length(roomColors[1] - roomColors[2]) > 0.5f &&
        glm::length(roomColors[2] - roomColors[0]) > 0.5f);

    Engine::Scene::Scene matrixRingRooms;
    assert(Engine::Serialization::SceneSerializer::Load(matrixRingRooms,
        "Engine/Core/Assets/Scenes/three_room_matrix_ring.scene", nullptr));
    validateThreeRoomPortalLoop(matrixRingRooms, "Matrix ");
    const std::array<glm::vec3, 3> physicalRoomCenters {
        glm::vec3(-24.f, 2.5f, 0.f), glm::vec3(0.f, 2.5f, 0.f),
        glm::vec3(24.f, 2.5f, 0.f) };
    const std::array<glm::vec3, 3> expectedRingCenters {
        glm::vec3(-4.f, 2.5f, -2.309f), glm::vec3(4.f, 2.5f, -2.309f),
        glm::vec3(0.f, 2.5f, 4.619f) };
    const auto roomRenderQuery = Engine::Scene::Scene::SpatialQuery {
        Engine::Scene::Scene::SpatialQueryDomain::Rendering };
    std::array<glm::vec3, 3> mappedRoomCenters {};
    for (size_t room = 0; room < physicalRoomCenters.size(); ++room)
    {
        const auto sample = matrixRingRooms.SampleSpatialPoint(
            physicalRoomCenters[room], roomRenderQuery);
        mappedRoomCenters[room] = sample.point;
        assert(glm::length(sample.point - expectedRingCenters[room]) < 0.002f);
        // A rigid orthonormal Jacobian proves the interior metric remains
        // cubic even though the three exterior charts fold into a ring.
        for (int axis = 0; axis < 3; ++axis)
            assert(std::abs(glm::length(sample.jacobian[axis]) - 1.f) < 0.002f);
        assert(std::abs(glm::dot(sample.jacobian[0], sample.jacobian[1])) < 0.002f);
        assert(std::abs(glm::dot(sample.jacobian[1], sample.jacobian[2])) < 0.002f);
        assert(std::abs(glm::dot(sample.jacobian[2], sample.jacobian[0])) < 0.002f);
    }
    const float ringSide01 = glm::length(mappedRoomCenters[0] -
        mappedRoomCenters[1]);
    const float ringSide12 = glm::length(mappedRoomCenters[1] -
        mappedRoomCenters[2]);
    const float ringSide20 = glm::length(mappedRoomCenters[2] -
        mappedRoomCenters[0]);
    assert(std::abs(ringSide01 - 8.f) < 0.002f);
    assert(std::abs(ringSide12 - 8.f) < 0.002f);
    assert(std::abs(ringSide20 - 8.f) < 0.002f);

    // Move a probe around the authored route repeatedly. Every transition
    // starts at a room center, crosses that room's north portal, arrives at
    // the next room's west portal, and finishes at the next room center.
    // Besides the gameplay-space handoff, sample the same movement in the
    // rendering chart so the folded layout cannot silently disagree with
    // portal position or direction mapping.
    constexpr int roomProbeLoopCount = 12;
    constexpr int roomProbeSamplesPerLeg = 16;
    const auto exerciseRoomProbeLoop = [&](Engine::Scene::Scene& roomScene,
        const std::string& prefix, const std::array<glm::vec3, 3>& roomCenters,
        bool expectMatrixWarp)
    {
        Engine::Core::Object* probe = roomScene.AddObject(
            prefix + "Repeated Room Probe");
        probe->transform.position = roomCenters[0];
        glm::vec3 probePosition = roomCenters[0];
        float traveledDistance = 0.f;
        float renderedDistance = 0.f;
        float maximumPositionError = 0.f;
        float minimumDirectionDot = 1.f;
        int transitionCount = 0;

        const auto moveProbe = [&](const glm::vec3& destination)
        {
            const glm::vec3 start = probePosition;
            const glm::vec3 displacement = destination - start;
            const float distance = glm::length(displacement);
            assert(distance > 0.001f);
            const glm::vec3 direction = displacement / distance;
            glm::vec3 previous = start;
            glm::vec3 previousRendered = roomScene.MapSpatialPoint(previous,
                roomRenderQuery);
            for (int sample = 1; sample <= roomProbeSamplesPerLeg; ++sample)
            {
                const float alpha = static_cast<float>(sample) /
                    static_cast<float>(roomProbeSamplesPerLeg);
                const glm::vec3 current = glm::mix(start, destination, alpha);
                const glm::vec3 delta = current - previous;
                assert(glm::dot(glm::normalize(delta), direction) > 0.9999f);

                const glm::vec3 currentRendered = roomScene.MapSpatialPoint(
                    current, roomRenderQuery);
                const glm::vec3 renderedDelta = currentRendered - previousRendered;
                const auto renderedRay = roomScene.MapSpatialRay(
                    { previous, direction }, roomRenderQuery);
                const float directionDot = glm::dot(glm::normalize(renderedDelta),
                    renderedRay.direction);
                minimumDirectionDot = std::min(minimumDirectionDot, directionDot);
                assert(directionDot > 0.999f);
                // Both fixtures use only rigid portal/volume transforms, so
                // movement length must remain unchanged in render space.
                assert(std::abs(glm::length(renderedDelta) -
                    glm::length(delta)) < 0.002f);
                traveledDistance += glm::length(delta);
                renderedDistance += glm::length(renderedDelta);
                previous = current;
                previousRendered = currentRendered;
                probe->transform.position = current;
            }
            probePosition = destination;
        };

        for (int loop = 0; loop < roomProbeLoopCount; ++loop)
        {
            for (int room = 0; room < 3; ++room)
            {
                const int nextRoom = (room + 1) % 3;
                assert(glm::length(probePosition - roomCenters[room]) < 0.002f);
                Engine::Core::Object* sourceObject = roomScene.FindObjectByName(
                    prefix + "Room " + std::to_string(room + 1) + " North Exit");
                Engine::Core::Object* targetObject = roomScene.FindObjectByName(
                    prefix + "Room " + std::to_string(nextRoom + 1) +
                    " West Entrance");
                assert(sourceObject && targetObject);
                SpatialManipulator* source =
                    sourceObject->GetComponent<SpatialManipulator>();
                SpatialManipulator* target =
                    targetObject->GetComponent<SpatialManipulator>();
                assert(source && target && source->ResolveTarget() == target);

                const glm::vec3 sourceAnchor(
                    source->GetPortalWorldFrame()[3]);
                const glm::vec3 targetAnchor(
                    target->GetPortalWorldFrame()[3]);
                const glm::vec3 outgoingDirection = glm::normalize(
                    sourceAnchor - roomCenters[room]);
                assert(source->IsWorldPointInsidePortalAperture(sourceAnchor));
                assert(glm::dot(outgoingDirection, glm::vec3(0.f, 0.f, 1.f)) >
                    0.999f);

                // Continue slightly through the plane so the mapped probe is
                // already inside the destination room after the handoff.
                const glm::vec3 sourceCrossing = sourceAnchor +
                    outgoingDirection * 0.2f;
                moveProbe(sourceCrossing);
                const glm::mat4 portalTransform =
                    source->GetPortalWorldTransformTo(*target);
                const glm::vec3 mappedPosition = glm::vec3(portalTransform *
                    glm::vec4(probePosition, 1.f));
                const glm::vec3 mappedDirection = glm::normalize(glm::mat3(
                    portalTransform) * outgoingDirection);
                const glm::vec3 expectedMappedPosition = targetAnchor +
                    mappedDirection * 0.2f;
                const float positionError = glm::length(mappedPosition -
                    expectedMappedPosition);
                maximumPositionError = std::max(maximumPositionError,
                    positionError);
                assert(positionError < 0.002f);
                assert(glm::dot(mappedDirection, glm::normalize(
                    roomCenters[nextRoom] - targetAnchor)) > 0.999f);

                const glm::vec3 renderSourceCrossing = roomScene.MapSpatialPoint(
                    probePosition, roomRenderQuery);
                const glm::vec3 renderMappedPosition =
                    source->MapRenderWorldPointThroughPortalShape(
                        renderSourceCrossing, *target);
                const glm::vec3 expectedRenderMappedPosition =
                    roomScene.MapSpatialPoint(mappedPosition, roomRenderQuery);
                assert(glm::length(renderMappedPosition -
                    expectedRenderMappedPosition) < 0.004f);
                const glm::vec3 renderOutgoingDirection = roomScene.MapSpatialRay(
                    { probePosition, outgoingDirection }, roomRenderQuery).direction;
                const glm::vec3 renderMappedDirection = glm::normalize(glm::mat3(
                    source->GetRenderPortalWorldTransformTo(*target)) *
                    renderOutgoingDirection);
                const glm::vec3 expectedRenderMappedDirection =
                    roomScene.MapSpatialRay(
                        { mappedPosition, mappedDirection }, roomRenderQuery).direction;
                const float portalDirectionDot = glm::dot(renderMappedDirection,
                    expectedRenderMappedDirection);
                minimumDirectionDot = std::min(minimumDirectionDot,
                    portalDirectionDot);
                assert(portalDirectionDot > 0.999f);

                probePosition = mappedPosition;
                probe->transform.position = mappedPosition;
                moveProbe(roomCenters[nextRoom]);
                ++transitionCount;
            }
        }

        assert(transitionCount == roomProbeLoopCount * 3);
        assert(glm::length(probePosition - roomCenters[0]) < 0.002f);
        assert(std::abs(traveledDistance - roomProbeLoopCount * 3.f * 8.f) <
            0.05f);
        assert(std::abs(renderedDistance - traveledDistance) < 0.05f);
        const auto finalRenderSample = roomScene.SampleSpatialPoint(
            probePosition, roomRenderQuery);
        assert(finalRenderSample.affectedByWarpVolume == expectMatrixWarp);
        if (expectMatrixWarp)
            assert(glm::length(finalRenderSample.point - probePosition) > 1.f);

        std::cout << "three_room_probe fixture=" <<
            (expectMatrixWarp ? "matrix_ring" : "stacked") << " loops=" <<
            roomProbeLoopCount << " transitions=" << transitionCount <<
            " distance=" << traveledDistance << " render_distance=" <<
            renderedDistance << " max_position_error=" << maximumPositionError <<
            " min_direction_dot=" << minimumDirectionDot << '\n';
    };

    const std::array<glm::vec3, 3> stackedRoomCenters {
        glm::vec3(0.f, 1.7f, 0.f), glm::vec3(0.f, 8.7f, 0.f),
        glm::vec3(0.f, 15.7f, 0.f) };
    const std::array<glm::vec3, 3> matrixRoomCenters {
        glm::vec3(-24.f, 1.7f, 0.f), glm::vec3(0.f, 1.7f, 0.f),
        glm::vec3(24.f, 1.7f, 0.f) };
    exerciseRoomProbeLoop(stackedRooms, "", stackedRoomCenters, false);
    exerciseRoomProbeLoop(matrixRingRooms, "Matrix ", matrixRoomCenters, true);

    std::cout << "three_room_scenes stacked_loop=1 matrix_ring_loop=1"
        << " ring_sides=" << ringSide01 << ',' << ringSide12 << ','
        << ringSide20 << " cubic_jacobians=1\n";

    // A quarter-turn formula volume maps a straight source-chart ray to a
    // quarter circle around its center. The local ray tangent rotates from
    // +Z at the entry to +X at the exit, which is the shared contract for
    // rendering, camera orientation, and light placement.
    Engine::Scene::Scene quarterTurn;
    assert(Engine::Serialization::SceneSerializer::Load(quarterTurn,
        "Engine/Core/Assets/Scenes/quarter_turn_warp_optics.scene", nullptr));
    constexpr float bendRadius = 6.f;
    constexpr float quarterTurnLength = 9.42477796f;
    const auto warpRenderQuery = Engine::Scene::Scene::SpatialQuery {
        Engine::Scene::Scene::SpatialQueryDomain::Rendering };
    const glm::vec3 bendEntry = quarterTurn.MapSpatialPoint(
        { 0.f, 2.f, 0.f }, warpRenderQuery);
    const glm::vec3 bendMidpoint = quarterTurn.MapSpatialPoint(
        { 0.f, 2.f, quarterTurnLength * 0.5f }, warpRenderQuery);
    const glm::vec3 bendExit = quarterTurn.MapSpatialPoint(
        { 0.f, 2.f, quarterTurnLength }, warpRenderQuery);
    assert(glm::length(bendEntry - glm::vec3(0.f, 2.f, 0.f)) < 0.003f);
    assert(glm::length(bendMidpoint - glm::vec3(
        bendRadius * (1.f - std::sqrt(0.5f)), 2.f,
        bendRadius * std::sqrt(0.5f))) < 0.003f);
    assert(glm::length(bendExit - glm::vec3(bendRadius, 2.f, bendRadius)) < 0.003f);
    for (int step = 0; step <= 16; ++step)
    {
        const float distance = quarterTurnLength * static_cast<float>(step) / 16.f;
        const glm::vec3 curvePoint = quarterTurn.MapSpatialPoint(
            { 0.f, 2.f, distance }, warpRenderQuery);
        assert(std::abs(glm::length(glm::vec2(curvePoint.x - bendRadius,
            curvePoint.z)) - bendRadius) < 0.004f);
    }
    const Engine::Scene::Scene::SpatialRay entranceRay = quarterTurn.MapSpatialRay(
        { { 0.f, 2.f, 0.02f }, { 0.f, 0.f, 1.f } }, warpRenderQuery);
    const Engine::Scene::Scene::SpatialRay exitRay = quarterTurn.MapSpatialRay(
        { { 0.f, 2.f, quarterTurnLength - 0.02f }, { 0.f, 0.f, 1.f } },
        warpRenderQuery);
    assert(glm::dot(entranceRay.direction, glm::vec3(0.f, 0.f, 1.f)) > 0.999f);
    assert(glm::dot(exitRay.direction, glm::vec3(1.f, 0.f, 0.f)) > 0.999f);

    // Inside the volume, the camera stays in the source chart. Its forward
    // raster ray therefore continues along +Z through the whole curved path
    // rather than looking along the globally embedded chord at the first bend.
    Engine::Core::Object* quarterTurnCameraObject = quarterTurn.FindObjectByName(
        "Quarter Turn Warp Camera");
    Engine::Components::Camera* quarterTurnCamera = quarterTurnCameraObject
        ? quarterTurnCameraObject->GetComponent<Engine::Components::Camera>() : nullptr;
    assert(quarterTurnCamera);
    quarterTurnCameraObject->transform.position =
        { 0.f, 2.f, quarterTurnLength * 0.5f };
    const glm::mat4 sourceChartCameraWorld = glm::inverse(
        quarterTurnCamera->GetViewMatrix());
    assert(glm::length(glm::vec3(sourceChartCameraWorld[3]) - glm::vec3(
        0.f, 2.f, quarterTurnLength * 0.5f)) < 0.003f);
    assert(glm::dot(glm::normalize(glm::vec3(sourceChartCameraWorld[2])),
        glm::vec3(0.f, 0.f, 1.f)) > 0.999f);

    std::array<Engine::Model::LightData, 8> warpLights {};
    Engine::Rendering::RealtimeLightingPipeline realtimeLights;
    const uint32_t warpLightCount = realtimeLights.CollectLights(quarterTurn,
        warpLights.data(), static_cast<uint32_t>(warpLights.size()));
    assert(warpLightCount == 2u);
    bool foundMappedBendLight = false;
    for (uint32_t index = 0; index < warpLightCount; ++index)
    {
        const glm::vec3 lightPosition(warpLights[index].positionRange);
        if (glm::length(lightPosition - glm::vec3(
                bendRadius * (1.f - std::sqrt(0.5f)), 3.3f,
                bendRadius * std::sqrt(0.5f))) < 0.004f)
        {
            foundMappedBendLight = true;
            break;
        }
    }
    assert(foundMappedBendLight);

    Engine::Core::Object* removableObject = scene.AddObject("RemovableLink");
    Engine::Core::Object* survivingObject = scene.AddObject("SurvivingLink");
    auto* removable = removableObject->AddComponent<SpatialManipulator>();
    auto* surviving = survivingObject->AddComponent<SpatialManipulator>();
    removable->connectionMode = static_cast<int>(
        SpatialManipulator::ConnectionMode::Portal);
    surviving->connectionMode = removable->connectionMode;
    removable->ConnectToTarget(surviving);
    removable->Update();
    assert(surviving->HasTarget());
    assert(survivingObject->transform.matrixLayer.connection.enabled);
    scene.RemoveObject(removableObject);
    assert(!surviving->HasTarget());
    assert(!survivingObject->transform.matrixLayer.connection.enabled);

    return 0;
}
