#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/SpatialManipulator.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Object.h"
#include <glm/glm.hpp>
#include <cassert>

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
    const glm::vec3 mapped = sourcePortal.MapWorldPointThroughPortalShape(
        glm::vec3(0.25f, 0.25f, 0.f), targetPortal);
    assert(glm::length(mapped - glm::vec3(-0.25f, 0.25f, 0.f)) < 0.05f);

    targetPortal.portalPointCount = 3;
    assert(!sourcePortal.HasCompatiblePortalShapeWith(targetPortal));

    Engine::Scene::Scene scene;
    Engine::Core::Object* sourceObject = scene.AddObject("WarpSource");
    Engine::Core::Object* targetObject = scene.AddObject("WarpTarget");
    auto* source = sourceObject->AddComponent<SpatialManipulator>();
    auto* target = targetObject->AddComponent<SpatialManipulator>();

    source->connectionMode = static_cast<int>(SpatialManipulator::ConnectionMode::MatrixOverlay);
    source->position = glm::vec3(2.f, 0.f, 0.f);
    target->position = glm::vec3(4.f, 0.f, 0.f);
    source->ConnectToTarget(target);
    assert(source->ResolveTarget() == target);
    assert(target->ResolveTarget() == source);
    source->Update();
    assert(sourceObject->transform.matrixLayer.enabled);
    assert(targetObject->transform.matrixLayer.enabled);

    source->enabled = false;
    source->Update();
    assert(!sourceObject->transform.matrixLayer.enabled);
    assert(!sourceObject->transform.matrixLayer.connection.enabled);
    assert(!targetObject->transform.matrixLayer.enabled);
    assert(!targetObject->transform.matrixLayer.connection.enabled);
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
    assert(glm::length(mappedNormal - (-targetNormal)) < 0.0002f);
    const glm::vec3 mappedTangent = glm::vec3(sourceToTarget *
        glm::vec4(1.f, 0.f, 0.f, 0.f));
    const glm::vec3 mappedBitangent = glm::vec3(sourceToTarget *
        glm::vec4(0.f, 1.f, 0.f, 0.f));
    assert(glm::length(mappedTangent - glm::vec3(0.f, 0.f, 1.f)) < 0.0002f);
    assert(glm::length(mappedBitangent - glm::vec3(0.f, 1.f, 0.f)) < 0.0002f);
    assert(glm::determinant(glm::mat3(sourceToTarget)) > 0.f);

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

    const glm::vec3 mappedByScene = scene.WarpWorldPoint(glm::vec3(1.f, 2.f, 0.f));
    assert(glm::length(mappedByScene - warpedInside) < 0.0002f);

    volume->formulaX = "sqrt(-1)";
    assert(glm::length(scene.WarpWorldPoint(glm::vec3(1.f, 2.f, 0.f)) -
        glm::vec3(1.f, 2.f, 0.f)) < 0.0002f);
    volume->formulaX = "cos(a*y)*x - sin(a*y)*z";

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
