#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
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
    // A portal crossing is a half-turn in portal-local coordinates.
    assert(glm::length(mapped - glm::vec3(-0.25f, 0.25f, 0.f)) < 0.05f);

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
    assert(glm::length(mappedNormal + targetNormal) < 0.0002f);
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
    assert(glm::length(mappedTangent - glm::vec3(0.f, 0.f, 1.f)) < 0.0002f);
    assert(glm::length(mappedBitangent - glm::vec3(0.f, 1.f, 0.f)) < 0.0002f);
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
        glm::vec3(6.f, 6.f, 0.f), renderQuery);
    const glm::vec3 opticalExit = shrinkingTunnel.MapSpatialPoint(
        glm::vec3(1.5f, 6.f, 30.f), renderQuery);
    const glm::vec3 opticalExitFloor = shrinkingTunnel.MapSpatialPoint(
        glm::vec3(0.f, 4.5f, 30.f), renderQuery);
    assert(glm::length(opticalEntrance - glm::vec3(6.f, 6.f, 0.f)) < 0.002f);
    assert(glm::length(opticalExit - glm::vec3(6.f, 6.f, 30.f)) < 0.002f);
    assert(glm::length(opticalExitFloor - glm::vec3(0.f, 0.f, 30.f)) < 0.002f);

    // Trace three physical geodesic samples through the tunnel. In physical
    // space their lateral position contracts exponentially; mapping every
    // point through the render chart produces straight, parallel optical
    // rays. This is the same condition the per-vertex tunnel mesh satisfies.
    constexpr float tunnelExponent = 0.046209812f;
    for (const float opticalX : { -3.5f, 0.f, 3.5f })
    {
        for (int sample = 0; sample <= 30; ++sample)
        {
            const float z = static_cast<float>(sample);
            const glm::vec3 physicalRayPoint(opticalX *
                std::exp(-tunnelExponent * z), 6.f, z);
            const glm::vec3 opticalRayPoint = shrinkingTunnel.MapSpatialPoint(
                physicalRayPoint, renderQuery);
            assert(glm::length(opticalRayPoint -
                glm::vec3(opticalX, 6.f, z)) < 0.003f);
        }
    }

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
    scaleProbe->transform.position = { 0.f, 6.f, 10.f };
    tunnelWarp->Update();
    assert(glm::length(scaleProbe->transform.scale - glm::vec3(
        std::exp(-tunnelExponent * 10.f))) < 0.003f);

    scaleProbe->transform.position = { 0.f, 6.f, 30.1f };
    tunnelWarp->Update();
    assert(glm::length(scaleProbe->transform.scale - glm::vec3(0.25f)) < 0.003f);
    assert(probeMesh->GetVertexCount() == sourceVertexCount);
    assert(glm::length(glm::vec3(probeMesh->GetVertices()[0].pos[0],
        probeMesh->GetVertices()[0].pos[1], probeMesh->GetVertices()[0].pos[2]) -
        sourceVertex) < 0.0001f);
    assert(probeMesh->GetMorphWeights().size() == 1u &&
        std::abs(probeMesh->GetMorphWeights()[0] - 0.65f) < 0.0001f);

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
