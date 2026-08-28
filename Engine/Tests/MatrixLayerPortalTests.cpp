#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/SpatialManipulator.h"
#include "Core/Scene/Scene.h"
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
    assert(glm::length(mapped - glm::vec3(-0.5f, 0.5f, 0.f)) < 0.05f);

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

    const glm::vec3 mappedCenter = glm::vec3(sourceToTarget *
        glm::vec4(sourceObject->transform.position, 1.f));
    assert(glm::length(mappedCenter - targetObject->transform.position) < 0.0002f);

    const glm::vec3 sourceNormal(0.f, 0.f, 1.f);
    const glm::vec3 targetNormal(1.f, 0.f, 0.f);
    const glm::vec3 mappedNormal = glm::vec3(sourceToTarget *
        glm::vec4(sourceNormal, 0.f));
    assert(glm::length(mappedNormal - (-2.f * targetNormal)) < 0.0002f);
    const glm::vec3 mappedTangent = glm::vec3(sourceToTarget *
        glm::vec4(1.f, 0.f, 0.f, 0.f));
    const glm::vec3 mappedBitangent = glm::vec3(sourceToTarget *
        glm::vec4(0.f, 1.f, 0.f, 0.f));
    assert(glm::length(mappedTangent - glm::vec3(0.f, 0.f, 2.f)) < 0.0002f);
    assert(glm::length(mappedBitangent - glm::vec3(0.f, 2.f, 0.f)) < 0.0002f);
    assert(glm::determinant(glm::mat3(sourceToTarget)) > 0.f);

    source->Update();
    const auto& sourceConnection = sourceObject->transform.matrixLayer.connection;
    const auto& targetConnection = targetObject->transform.matrixLayer.connection;
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
        {
            assert(std::abs(sourceConnection.localToRemote[column][row] -
                sourceToTarget[column][row]) < 0.0002f);
            assert(std::abs(targetConnection.localToRemote[column][row] -
                targetToSource[column][row]) < 0.0002f);
        }

    return 0;
}
