#include "Core/Compoonents/Obj/Mesh.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <vector>

namespace
{
using Vertex = Engine::Components::Mesh::Vertex;

void Near(float actual, float expected)
{
    if (std::abs(actual - expected) >= 2e-4f)
        std::cerr << "actual=" << actual << " expected=" << expected << '\n';
    assert(std::abs(actual - expected) < 2e-4f);
}

glm::mat4 Pose(float degrees, glm::vec3 translation = {})
{
    return glm::translate(glm::mat4(1.f), translation) *
        glm::rotate(glm::mat4(1.f), glm::radians(degrees), glm::vec3(0, 0, 1));
}

Vertex SdefVertex()
{
    Vertex vertex{};
    vertex.pos[0] = 2.f;
    vertex.normal[1] = 1.f;
    vertex.tangent[0] = 1.f;
    vertex.joints0[0] = 0.f;
    vertex.joints0[1] = 1.f;
    vertex.weights0[0] = .75f;
    vertex.weights0[1] = .25f;
    vertex.sdefCenterAndMode[0] = 1.f;
    vertex.sdefCenterAndMode[3] = 3.f;
    vertex.sdefRadius0[0] = .2f;
    vertex.sdefRadius1[0] = -.1f;
    return vertex;
}

Vertex QdefVertex()
{
    Vertex vertex{};
    vertex.pos[0] = 1.f;
    vertex.normal[1] = 1.f;
    vertex.tangent[0] = 1.f;
    vertex.joints0[0] = 0.f;
    vertex.joints0[1] = 2.f;
    vertex.weights0[0] = .5f;
    vertex.weights0[1] = .5f;
    vertex.sdefCenterAndMode[3] = 4.f;
    return vertex;
}

void CheckPose(const std::vector<glm::mat4>& palette,
    glm::vec3 sdef, glm::vec3 qdef, glm::vec3 qdefNormal)
{
    Engine::Components::Mesh mesh;
    const auto s = SdefVertex();
    const auto q = QdefVertex();
    assert(mesh.SetAuthoredVertices({ s, q, s }));
    const auto posed = mesh.BuildPosedVertices(&palette);
    assert(posed.size() == 3);
    for (int axis = 0; axis < 3; ++axis)
    {
        Near(posed[0].pos[axis], sdef[axis]);
        Near(posed[1].pos[axis], qdef[axis]);
        Near(posed[1].normal[axis], qdefNormal[axis]);
    }
}
}

int main()
{
    // Bind pose, translation pose, and rotation pose. Expected
    // positions are independent reference values, not another skinning call.
    CheckPose({ Pose(0), Pose(0), Pose(0) },
        { 2, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 });
    CheckPose({ Pose(0, { 2, 0, 0 }), Pose(0, { 0, 2, 0 }),
        Pose(0, { 0, 2, 0 }) },
        { 3.5f, .5f, 0 }, { 2, 1, 0 }, { 0, 1, 0 });
    constexpr float c = .7071067811865475f;
    CheckPose({ Pose(90), Pose(0), Pose(0) },
        { .225f + .875f * std::cos(glm::radians(67.5f)),
          .9f + .875f * std::sin(glm::radians(67.5f)), 0 },
        { c, c, 0 }, { -c, c, 0 });

    // QDEF must consume all four PMX influences.
    Vertex four = QdefVertex();
    for (int i = 0; i < 4; ++i)
    {
        four.joints0[i] = static_cast<float>(i);
        four.weights0[i] = .25f;
    }
    Engine::Components::Mesh fourMesh;
    assert(fourMesh.SetAuthoredVertices({ four, four, four }));
    const std::vector<glm::mat4> fourPalette{
        Pose(0), Pose(0, { 2, 0, 0 }), Pose(0, { 4, 0, 0 }),
        Pose(0, { 6, 0, 0 }) };
    Near(fourMesh.BuildPosedVertices(&fourPalette)[0].pos[0], 4.f);

    // Native mesh assets must retain the PMX mode, center, and both radii.
    Engine::Components::Mesh mesh;
    assert(mesh.SetAuthoredVertices({ SdefVertex(), QdefVertex(), SdefVertex() }));
    const auto path = std::filesystem::temp_directory_path() /
        "engine-skin-deformation-roundtrip.mesh";
    assert(Engine::Components::Mesh::SaveNativeFile(path.string(), mesh.GetVertices()));
    Engine::Components::Mesh loaded;
    loaded.LoadFromFile(path.string());
    std::filesystem::remove(path);
    assert(loaded.GetVertices().size() == 3);
    const auto& s = loaded.GetVertices()[0];
    Near(s.sdefCenterAndMode[0], 1.f);
    Near(s.sdefCenterAndMode[3], 3.f);
    Near(s.sdefRadius0[0], .2f);
    Near(s.sdefRadius1[0], -.1f);
    Near(loaded.GetVertices()[1].sdefCenterAndMode[3], 4.f);
}
