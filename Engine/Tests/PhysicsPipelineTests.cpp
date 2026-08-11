#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Physics/PhysicsSystem.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>

namespace
{
void Step(Scene& scene, int count)
{
    for (int i = 0; i < count; ++i)
        PhysicsSystem::Step(scene, 1.f / 60.f);
}

bool PrimitiveSimulation()
{
    Scene scene;
    Object* floor = scene.AddObject("Floor");
    floor->transform.position = { 0.f, -0.5f, 0.f };
    auto* floorCollider = floor->AddComponent<PrimitiveObjectCollider>();
    floorCollider->shape = "Box";
    floorCollider->size = { 20.f, 1.f, 20.f };
    auto* floorBody = floor->AddComponent<RigidBody>();
    floorBody->bodyType = "Static";

    Object* falling = scene.AddObject("Falling Box");
    falling->transform.position = { 0.f, 4.f, 0.f };
    auto* fallingCollider = falling->AddComponent<PrimitiveObjectCollider>();
    fallingCollider->shape = "Cube"; // Alias accepted by the Box fallback.
    fallingCollider->size = { 1.f, 1.f, 1.f };
    auto* fallingBody = falling->AddComponent<RigidBody>();
    fallingBody->restitution = 0.f;

    floor->Start();
    falling->Start();
    Step(scene, 240);
    if (std::abs(falling->transform.position.y - 0.5f) > 0.15f ||
        !fallingBody->IsColliding() || !fallingBody->IsGrounded())
    {
        std::cerr << "Dynamic box did not settle on its primitive collider floor: y="
                  << falling->transform.position.y << '\n';
        return false;
    }

    const float beforeImpulse = falling->transform.position.y;
    fallingBody->AddImpulse({ 0.f, 4.f, 0.f });
    Step(scene, 10);
    if (falling->transform.position.y <= beforeImpulse + 0.05f)
    {
        std::cerr << "RigidBody impulse did not move the dynamic body\n";
        return false;
    }
    return true;
}

bool MeshSimulation(const char* meshPath)
{
    Scene scene;
    Object* colliderSource = scene.AddObject("Collider Geometry Source");
    Mesh* colliderMesh = colliderSource->AddComponent<Mesh>();
    colliderMesh->LoadFromFile(meshPath);
    Object* floor = scene.AddObject("Mesh Floor");
    floor->transform.position = { 0.f, -0.5f, 0.f };
    floor->transform.scale = { 12.f, 1.f, 12.f };
    auto* meshCollider = floor->AddComponent<MeshObjectCollider>();
    meshCollider->meshReference = CaptureComponentReference(colliderMesh, "Mesh");
    meshCollider->convex = false;
    auto* floorBody = floor->AddComponent<RigidBody>();
    floorBody->bodyType = "Static";

    Object* sphere = scene.AddObject("Sphere");
    sphere->transform.position = { 0.f, 3.f, 0.f };
    auto* sphereCollider = sphere->AddComponent<PrimitiveObjectCollider>();
    sphereCollider->shape = "Circle"; // Checklist spelling aliases Sphere.
    sphereCollider->radius = 0.5f;
    auto* sphereBody = sphere->AddComponent<RigidBody>();

    floor->Start();
    sphere->Start();
    Step(scene, 240);
    if (sphere->transform.position.y < -0.2f || !sphereBody->IsColliding())
    {
        std::cerr << "Sphere did not collide with the concave mesh collider\n";
        return false;
    }

    Scene restored;
    if (!SceneSerializer::LoadFromString(restored, scene.SaveToString()))
    {
        std::cerr << "Scene did not round-trip component references\n";
        return false;
    }
    MeshObjectCollider* restoredCollider = nullptr;
    Mesh* restoredSource = nullptr;
    for (const auto& candidate : restored.GetObjects())
    {
        if (candidate->name == "Mesh Floor")
            restoredCollider = candidate->GetComponent<MeshObjectCollider>();
        if (candidate->name == "Collider Geometry Source")
            restoredSource = candidate->GetComponent<Mesh>();
    }
    if (!restoredCollider || ResolveComponentReference<Mesh>(
        restoredCollider->Owner, restoredCollider->meshReference) != restoredSource)
    {
        std::cerr << "Serialized mesh collider reference did not resolve after load\n";
        return false;
    }
    return true;
}

bool ClothSimulation()
{
    std::vector<Vertex> clothVertices(6);
    const glm::vec3 positions[] = {
        { -1.f, 1.f, 0.f }, { -1.f, -1.f, 0.f }, { 1.f, -1.f, 0.f },
        { -1.f, 1.f, 0.f }, { 1.f, -1.f, 0.f }, { 1.f, 1.f, 0.f }
    };
    for (std::size_t index = 0; index < clothVertices.size(); ++index)
    {
        clothVertices[index].pos[0] = positions[index].x;
        clothVertices[index].pos[1] = positions[index].y;
        clothVertices[index].pos[2] = positions[index].z;
        clothVertices[index].normal[2] = 1.f;
    }
    const std::filesystem::path fixture =
        std::filesystem::temp_directory_path() / "EngineProjectClothTest.mesh";
    if (!Mesh::SaveNativeFile(fixture.string(), clothVertices))
    {
        std::cerr << "Could not create the cloth mesh fixture\n";
        return false;
    }

    // The visible mesh deliberately has a different topology from the
    // six-vertex simulation cage. This verifies the independent cloth mesh
    // reference and nearest-node render binding.
    std::vector<Vertex> renderVertices = clothVertices;
    const glm::vec3 detailTriangle[] = {
        { -0.4f, 0.4f, 0.05f }, { 0.f, -0.4f, 0.05f }, { 0.4f, 0.4f, 0.05f }
    };
    for (const glm::vec3& position : detailTriangle)
    {
        Vertex vertex{};
        vertex.pos[0] = position.x;
        vertex.pos[1] = position.y;
        vertex.pos[2] = position.z;
        vertex.normal[2] = 1.f;
        renderVertices.push_back(vertex);
    }
    const std::filesystem::path renderFixture =
        std::filesystem::temp_directory_path() / "EngineProjectClothRenderTest.mesh";
    if (!Mesh::SaveNativeFile(renderFixture.string(), renderVertices))
    {
        std::cerr << "Could not create the cloth render-mesh fixture\n";
        return false;
    }

    Scene scene;
    Object* simulationSource = scene.AddObject("Cloth Simulation Source");
    Mesh* simulationMesh = simulationSource->AddComponent<Mesh>();
    simulationMesh->LoadFromFile(fixture.string());
    Object* object = scene.AddObject("Cloth Mesh");
    object->transform.position = { 0.f, 3.f, 0.f };
    Mesh* mesh = object->AddComponent<Mesh>();
    mesh->LoadFromFile(renderFixture.string());
    const std::vector<Vertex> original = mesh->GetVertices();
    Cloth* cloth = object->AddComponent<Cloth>();
    cloth->simulationMeshReference = CaptureComponentReference(simulationMesh, "Mesh");
    cloth->pinMode = "Top";
    cloth->pinThreshold = 0.01f;
    cloth->bendingStiffness = 0.1f;
    cloth->windVelocity = { 0.f, 0.f, 1.f };
    cloth->windStrength = 5.f;
    if (!cloth->Serialize()["simulationMeshReference"].IsObject() ||
        ResolveComponentReference<Mesh>(object, cloth->simulationMeshReference) != simulationMesh)
    {
        std::cerr << "Cloth did not serialize/resolve its independent mesh reference\n";
        return false;
    }

    object->Start();
    Step(scene, 90);
    if (!cloth->IsSimulating())
    {
        std::cerr << "Cloth failed to create its soft body\n";
        return false;
    }
    bool deformed = false;
    for (std::size_t index = 0; index < original.size(); ++index)
    {
        const Vertex& current = mesh->GetVertices()[index];
        if (std::abs(current.pos[0] - original[index].pos[0]) > 0.001f ||
            std::abs(current.pos[1] - original[index].pos[1]) > 0.001f ||
            std::abs(current.pos[2] - original[index].pos[2]) > 0.001f)
        {
            deformed = true;
            break;
        }
    }
    if (!deformed)
    {
        std::cerr << "Cloth simulation did not deform the render mesh\n";
        return false;
    }

    cloth->Disabled();
    for (std::size_t index = 0; index < original.size(); ++index)
        for (int axis = 0; axis < 3; ++axis)
            if (mesh->GetVertices()[index].pos[axis] != original[index].pos[axis])
            {
                std::cerr << "Cloth did not restore the original mesh\n";
                return false;
            }
    std::error_code removeError;
    std::filesystem::remove(fixture, removeError);
    std::filesystem::remove(renderFixture, removeError);
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Expected a triangle-mesh fixture path\n";
        return 2;
    }
    if (!PrimitiveSimulation() || !MeshSimulation(argv[1]) ||
        !ClothSimulation()) return 1;

    const char* componentTypes[] = {
        "RigidBody", "PrimitiveObjectCollider", "MeshObjectCollider", "Cloth"
    };
    for (const char* type : componentTypes)
    {
        std::unique_ptr<Component> component(
            SceneSerializer::CreateRegisteredComponent(type));
        if (!component || component->GetTypeName() != type)
        {
            std::cerr << type << " is missing from the component registry\n";
            return 1;
        }
        const JsonValue serialized = component->Serialize();
        if (!serialized.IsObject() || serialized.ObjectSize() < 2)
        {
            std::cerr << type << " did not serialize its settings\n";
            return 1;
        }
    }
    return 0;
}
