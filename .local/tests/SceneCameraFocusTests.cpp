#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include <cmath>
#include <stdexcept>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool Near(const glm::vec3& a, const glm::vec3& b, float tolerance = 0.001f)
{
    return glm::length(a - b) < tolerance;
}

Engine::Components::Mesh::Vertex Vertex(float x, float y, float z)
{
    Engine::Components::Mesh::Vertex vertex{};
    vertex.pos[0] = x;
    vertex.pos[1] = y;
    vertex.pos[2] = z;
    return vertex;
}
}

int main()
{
    Engine::Scene::Scene scene;
    auto* camera = scene.editorCamera.AddComponent<Engine::Components::Camera>();
    scene.editorCamera.transform.position = { 0.f, 0.f, -3.f };
    camera->target = { 0.f, 0.f, 0.f };

    auto* root = scene.AddObject("root");
    root->transform.position = { 100.f, 0.f, 0.f };
    root->transform.rotation.z = glm::radians(90.f);
    auto* child = scene.AddObject("mesh");
    child->transform.position = { 2.f, 0.f, 0.f };
    Check(scene.MoveObject(child, root,
        Engine::Scene::Scene::ObjectPlacement::AsChild), "Could not parent mesh");
    auto* mesh = child->AddComponent<Engine::Components::Mesh>();
    Check(mesh->SetIndexedGeometry({ Vertex(9.f, -1.f, 0.f),
        Vertex(11.f, -1.f, 0.f), Vertex(11.f, 1.f, 0.f) },
        { 0, 1, 2 }), "Could not create test mesh");

    const glm::vec3 center = glm::vec3(child->transform.GetWorldMatrix() *
        glm::vec4(10.f, 0.f, 0.f, 1.f));
    scene.FocusEditorCamera(root);
    Check(Near(camera->target, center), "Focus missed the transformed mesh center");
    const float firstDistance = glm::length(
        scene.editorCamera.transform.position - camera->target);
    Check(firstDistance > 0.f && firstDistance < 10.f,
        "Focus used a fixed or oversized viewing distance");

    root->transform.scale = { 2.f, 2.f, 2.f };
    scene.FocusEditorCamera(root);
    const glm::vec3 scaledCenter = glm::vec3(child->transform.GetWorldMatrix() *
        glm::vec4(10.f, 0.f, 0.f, 1.f));
    Check(Near(camera->target, scaledCenter), "Scaled mesh center was missed");
    const float scaledDistance = glm::length(
        scene.editorCamera.transform.position - camera->target);
    Check(std::abs(scaledDistance / firstDistance - 2.f) < 0.01f,
        "Focus distance did not scale with object size");
    return 0;
}
