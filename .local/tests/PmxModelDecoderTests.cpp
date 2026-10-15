#include "Editor/Core/Importers/PmxModelDecoder.h"
#include "Editor/Core/Importers/ModelAssetWriter.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/RigController.h"
#include "Core/Compoonents/Animation/RigConstraints.h"
#include "Core/Compoonents/Animation/RigMorphControls.h"
#include "Core/Compoonents/Physics/RigPhysicsBinding.h"
#include "Core/Model/AnimationClip.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Materials/MaterialPropertyMorph.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

namespace
{
template<class T> void Append(std::vector<char>& bytes, const T& value)
{
    const char* data = reinterpret_cast<const char*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(T));
}

void AppendText(std::vector<char>& bytes, const char* value)
{
    const int32_t length = static_cast<int32_t>(std::strlen(value));
    Append(bytes, length);
    bytes.insert(bytes.end(), value, value + length);
}

void AppendFloats(std::vector<char>& bytes, std::initializer_list<float> values)
{
    for (float value : values) Append(bytes, value);
}

void WriteGroupMorphFixture(const std::filesystem::path& path,
    uint8_t jointType = 0, int8_t jointBodyA = 0,
    bool impulseMorphs = false, bool extendedFeatures = false,
    bool fullUv = false, bool skinModes = false,
    uint8_t sphereMode = 1, const char* sphereTextureName = "toon.png",
    bool ropeSoftBody = false)
{
    std::vector<char> bytes{ 'P', 'M', 'X', ' ' };
    Append(bytes, jointType == 0 && !impulseMorphs && !extendedFeatures && !fullUv && !skinModes
        ? 2.f : 2.1f);
    for (uint8_t value : std::array<uint8_t, 9>{ 8, 1, uint8_t(fullUv ? 4 : 1), 1, 1, 1, 1, 1, 1 })
        Append(bytes, value);
    AppendText(bytes, "group test");
    for (int i = 0; i < 3; ++i) AppendText(bytes, "");
    Append(bytes, int32_t(3));
    for (int i = 0; i < 3; ++i)
    {
        AppendFloats(bytes, { float(i == 1), float(i == 2), 0.f,
            0.f, 0.f, 1.f, 0.f, 0.f });
        AppendFloats(bytes, { .2f, .3f, fullUv ? .4f : 0.f, fullUv ? .5f : 0.f });
        if (fullUv)
            for (int channel = 2; channel <= 4; ++channel)
                AppendFloats(bytes, { float(channel), float(channel) + .1f,
                    float(channel) + .2f, float(channel) + .3f });
        if (skinModes && i == 0)
        {
            Append(bytes, uint8_t(3)); // SDEF.
            Append(bytes, int8_t(0)); Append(bytes, int8_t(0));
            Append(bytes, .25f);
            AppendFloats(bytes, { 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f });
        }
        else if (skinModes && i == 1)
        {
            Append(bytes, uint8_t(4)); // QDEF.
            for (int bone = 0; bone < 4; ++bone) Append(bytes, int8_t(0));
            AppendFloats(bytes, { .4f, .3f, .2f, .1f });
        }
        else
        {
            Append(bytes, uint8_t(0)); // BDEF1.
            Append(bytes, int8_t(skinModes ? 0 : -1));
        }
        Append(bytes, 1.f);
    }
    Append(bytes, int32_t(3));
    for (uint8_t i = 0; i < 3; ++i) Append(bytes, i);
    Append(bytes, int32_t(1)); // Textures.
    AppendText(bytes, sphereTextureName);
    Append(bytes, int32_t(1)); // Materials.
    AppendText(bytes, "material"); AppendText(bytes, "");
    AppendFloats(bytes, { 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f,
        1.f, 0.f, 0.f, 0.f });
    Append(bytes, uint8_t(0));
    AppendFloats(bytes, { .1f, .2f, .3f, .4f, 2.f });
    Append(bytes, int8_t(-1)); // Base texture.
    Append(bytes, int8_t(0)); // Sphere texture.
    Append(bytes, sphereMode);
    Append(bytes, uint8_t(0)); // Custom toon.
    Append(bytes, int8_t(0));
    AppendText(bytes, "");
    Append(bytes, int32_t(3));
    Append(bytes, int32_t(1)); // Bones.
    AppendText(bytes, "bone"); AppendText(bytes, "");
    AppendFloats(bytes, { 0.f, 0.f, 0.f });
    Append(bytes, int8_t(-1)); // Parent.
    Append(bytes, int32_t(0)); // Deform layer.
    Append(bytes, uint16_t(0x2400)); // Fixed axis and external parent.
    AppendFloats(bytes, { 0.f, 1.f, 0.f });
    AppendFloats(bytes, { 0.f, 0.f, -1.f }); // Reflected to engine +Z.
    Append(bytes, int32_t(42)); // External-parent key.
    Append(bytes, int32_t((impulseMorphs ? 9 : extendedFeatures ? 8 : 7) +
        (fullUv ? 3 : 0))); // Morphs.
    AppendText(bytes, "move"); AppendText(bytes, "");
    Append(bytes, uint8_t(4)); Append(bytes, uint8_t(1));
    Append(bytes, int32_t(1)); Append(bytes, uint8_t(0));
    AppendFloats(bytes, { 1.f, 0.f, 0.f });
    AppendText(bytes, "uv0"); AppendText(bytes, "");
    Append(bytes, uint8_t(4)); Append(bytes, uint8_t(3));
    Append(bytes, int32_t(1)); Append(bytes, uint8_t(0));
    AppendFloats(bytes, { 1.f, 0.f, fullUv ? .6f : 0.f,
        fullUv ? .7f : 0.f });
    AppendText(bytes, "uv1"); AppendText(bytes, "");
    Append(bytes, uint8_t(4)); Append(bytes, uint8_t(4));
    Append(bytes, int32_t(1)); Append(bytes, uint8_t(0));
    AppendFloats(bytes, { 0.f, 1.f, fullUv ? .8f : 0.f,
        fullUv ? .9f : 0.f });
    AppendText(bytes, "group"); AppendText(bytes, "");
    Append(bytes, uint8_t(4)); Append(bytes, uint8_t(0));
    Append(bytes, int32_t(fullUv ? 6 : 3));
    Append(bytes, int8_t(0)); Append(bytes, .5f);
    Append(bytes, int8_t(1)); Append(bytes, .25f);
    Append(bytes, int8_t(2)); Append(bytes, .75f);
    if (fullUv)
        for (int8_t child = 7; child <= 9; ++child)
        {
            Append(bytes, child); Append(bytes, .5f);
        }
    AppendText(bytes, "bone move"); AppendText(bytes, "");
    Append(bytes, uint8_t(4)); Append(bytes, uint8_t(2));
    Append(bytes, int32_t(1)); Append(bytes, int8_t(0));
    AppendFloats(bytes, { 0.f, 2.f, 0.f, 0.f, 0.f, 0.f, 1.f });
    AppendText(bytes, "fade"); AppendText(bytes, "");
    Append(bytes, uint8_t(4)); Append(bytes, uint8_t(8));
    Append(bytes, int32_t(1)); Append(bytes, int8_t(0));
    Append(bytes, uint8_t(1)); // Additive material morph.
    AppendFloats(bytes, { 0.f, 0.f, 0.f, -.5f,
        -.25f, 0.f, 0.f, 8.f, 0.f, 0.f, 0.f });
    AppendFloats(bytes, { .2f, .3f, .4f, .5f, .6f,
        .4f, 0.f, 0.f, 0.f,
        .7f, .8f, .9f, 1.f,
        .1f, .2f, .3f, .4f });
    AppendText(bytes, "mixed group"); AppendText(bytes, "");
    Append(bytes, uint8_t(4)); Append(bytes, uint8_t(0));
    Append(bytes, int32_t(impulseMorphs ? 4 : 3));
    Append(bytes, int8_t(3)); Append(bytes, .5f);  // Nested mesh group.
    Append(bytes, int8_t(4)); Append(bytes, .25f); // Bone morph.
    Append(bytes, int8_t(5)); Append(bytes, .5f);  // Material morph.
    if (impulseMorphs)
    {
        Append(bytes, int8_t(7)); Append(bytes, .5f); // Impulse morph.
        AppendText(bytes, "push"); AppendText(bytes, "");
        Append(bytes, uint8_t(4)); Append(bytes, uint8_t(10));
        Append(bytes, int32_t(1)); Append(bytes, int8_t(0));
        Append(bytes, uint8_t(0)); // World-space impulse.
        AppendFloats(bytes, { 5.f, 0.f, 0.f, 0.f, 0.f, 0.f });
        AppendText(bytes, "stop"); AppendText(bytes, "");
        Append(bytes, uint8_t(4)); Append(bytes, uint8_t(10));
        Append(bytes, int32_t(1)); Append(bytes, int8_t(0));
        Append(bytes, uint8_t(0));
        AppendFloats(bytes, { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f });
    }
    if (extendedFeatures)
    {
        AppendText(bytes, "flip"); AppendText(bytes, "");
        Append(bytes, uint8_t(4)); Append(bytes, uint8_t(9));
        Append(bytes, int32_t(2));
        Append(bytes, int8_t(0)); Append(bytes, .25f);
        Append(bytes, int8_t(5)); Append(bytes, .5f);
    }
    if (fullUv)
        for (uint8_t type = 5; type <= 7; ++type)
        {
            const std::string name = "uv" + std::to_string(type - 3);
            AppendText(bytes, name.c_str()); AppendText(bytes, "");
            Append(bytes, uint8_t(4)); Append(bytes, type);
            Append(bytes, int32_t(1)); Append(bytes, uint8_t(0));
            AppendFloats(bytes, { .1f * type, .2f * type,
                .3f * type, .4f * type });
        }
    Append(bytes, int32_t(0)); // Display frames.
    Append(bytes, int32_t(2)); // Rigid bodies.
    AppendText(bytes, "falling"); AppendText(bytes, "");
    Append(bytes, int8_t(0)); // Related bone.
    Append(bytes, uint8_t(0)); // Collision group.
    Append(bytes, uint16_t(0)); // Non-collision mask.
    Append(bytes, uint8_t(0)); // Sphere.
    AppendFloats(bytes, { 1.f, 1.f, 1.f, 0.f, 2.f, 0.f,
        0.f, 0.f, 0.f, 1.f, .1f, .1f, 0.f, .5f });
    Append(bytes, uint8_t(1)); // Dynamic.
    AppendText(bytes, "partner"); AppendText(bytes, "");
    Append(bytes, int8_t(0));
    Append(bytes, uint8_t(0));
    Append(bytes, uint16_t(0));
    Append(bytes, uint8_t(0));
    AppendFloats(bytes, { 1.f, 1.f, 1.f, 2.f, 2.f, 0.f,
        0.f, 0.f, 0.f, 1.f, .1f, .1f, 0.f, .5f });
    Append(bytes, uint8_t(1));
    Append(bytes, int32_t(1)); // Spring 6DOF joint.
    AppendText(bytes, "link"); AppendText(bytes, "");
    Append(bytes, jointType);
    Append(bytes, jointBodyA); Append(bytes, int8_t(1));
    AppendFloats(bytes, { 1.f, 2.f, 0.f, 0.f, 0.f, 0.f,
        0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
        0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
        0.f, 0.f, 0.f, 0.f, 0.f, 0.f });
    if (jointType != 0 || impulseMorphs || extendedFeatures || fullUv || skinModes)
    {
        Append(bytes, int32_t(extendedFeatures ? 1 : 0));
        if (extendedFeatures)
        {
            AppendText(bytes, "cloth"); AppendText(bytes, "");
            Append(bytes, uint8_t(ropeSoftBody ? 1 : 0)); Append(bytes, int8_t(0));
            Append(bytes, uint8_t(3)); Append(bytes, uint16_t(4));
            Append(bytes, uint8_t(0));
            Append(bytes, int32_t(0)); Append(bytes, int32_t(1));
            Append(bytes, 1.f); Append(bytes, .01f);
            Append(bytes, int32_t(0));
            AppendFloats(bytes, { 0.f, .1f, .01f, 0.f, 0.f, 0.f,
                .5f, 0.f, 0.f, 0.f, 0.f, 0.f });
            AppendFloats(bytes, { .2f, .3f, .4f, .5f, .6f, .7f });
            Append(bytes, int32_t(0)); Append(bytes, int32_t(8));
            Append(bytes, int32_t(0)); Append(bytes, int32_t(3));
            AppendFloats(bytes, { .8f, .2f, 0.f });
            Append(bytes, int32_t(1)); // Anchor count.
            Append(bytes, int8_t(0)); Append(bytes, uint8_t(0));
            Append(bytes, uint8_t(1)); // Near anchor.
            Append(bytes, int32_t(1)); // Pin count.
            Append(bytes, uint8_t(1));
        }
    }
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void TestFullUv(const std::filesystem::path& outputRoot)
{
    namespace fs = std::filesystem;
    std::string error;
    const fs::path fullUvFixture = outputRoot / "full-uv.pmx";
    WriteGroupMorphFixture(fullUvFixture, 0, 0, false, false, true);
    Engine::Model::ImportedModel fullUvModel;
    Engine::Editor::PmxImportReport fullUvReport;
    assert(Engine::Editor::PmxModelDecoder::Decode(
        fullUvFixture.string(), fullUvModel, error, &fullUvReport));
    const auto& fullPrimitive = fullUvModel.primitives[0];
    assert(fullPrimitive.vertices[0].uvChannels[1][2] == .4f);
    assert(std::abs(fullPrimitive.vertices[0].uvChannels[4][3] - 4.3f) < 1e-5f);
    assert(fullPrimitive.morphTargets.size() == 8);
    assert(fullPrimitive.morphTargets[5].uvChannels[2][0].x == .5f);
    assert(fullPrimitive.morphTargets[3].uvChannels[2][0].x == .25f);
    const fs::path fullUvMeshFile = outputRoot / "full-uv.mesh";
    const fs::path fullUvMorphFile = outputRoot / "full-uv.morph";
    assert(Engine::Components::Mesh::SaveNativeFile(
        fullUvMeshFile.string(), fullPrimitive.vertices));
    assert(Engine::Components::Mesh::SaveMorphFile(
        fullUvMorphFile.string(), fullPrimitive.morphTargets));
    Engine::Components::Mesh fullUvMesh;
    fullUvMesh.LoadFromFile(fullUvMeshFile.string());
    assert(std::abs(fullUvMesh.GetVertices()[0].uvChannels[4][3] - 4.3f) < 1e-5f);
    const fs::path legacyMeshFile = outputRoot / "legacy-uv.mesh";
    {
        std::ofstream legacy(legacyMeshFile, std::ios::binary);
        const uint32_t header[]{ 0x4853454d, 5,
            static_cast<uint32_t>(fullPrimitive.vertices.size()), 0 };
        legacy.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (const auto& vertex : fullPrimitive.vertices)
            legacy.write(reinterpret_cast<const char*>(&vertex), 136);
    }
    Engine::Components::Mesh legacyMesh;
    legacyMesh.LoadFromFile(legacyMeshFile.string());
    assert(legacyMesh.GetVertices()[0].uvChannels[0][0] ==
        legacyMesh.GetVertices()[0].uv[0]);
    assert(legacyMesh.GetVertices()[0].uvChannels[1][1] == .3f);
    const fs::path version6MeshFile = outputRoot / "version6-uv.mesh";
    {
        std::ofstream version6(version6MeshFile, std::ios::binary);
        const uint32_t header[]{ 0x4853454d, 6,
            static_cast<uint32_t>(fullPrimitive.vertices.size()), 0 };
        version6.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (const auto& vertex : fullPrimitive.vertices)
            version6.write(reinterpret_cast<const char*>(&vertex), 216);
    }
    Engine::Components::Mesh version6Mesh;
    version6Mesh.LoadFromFile(version6MeshFile.string());
    assert(std::abs(version6Mesh.GetVertices()[0].uvChannels[4][3] - 4.3f) < 1e-5f);
    assert(version6Mesh.GetVertices()[0].sdefCenterAndMode[3] == 0.f);
    assert(fullUvMesh.LoadMorphFile(fullUvMorphFile.string(), 0,
        { 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f }));
    const auto fullPosed = fullUvMesh.BuildPosedVertices();
    assert(fullPosed[0].uvChannels[0][2] == .6f);
    assert(std::abs(fullPosed[0].uvChannels[1][2] - 1.2f) < 1e-5f);
    assert(fullPosed[0].uvChannels[2][0] == 2.5f);
    assert(std::abs(fullPosed[0].uvChannels[3][1] - 4.3f) < 1e-5f);
    assert(std::abs(fullPosed[0].uvChannels[4][3] - 7.1f) < 1e-5f);
}

void TestSkinModes(const std::filesystem::path& outputRoot)
{
    const auto source = outputRoot / "skin-modes.pmx";
    WriteGroupMorphFixture(source, 0, 0, false, false, false, true);
    Engine::Model::ImportedModel model;
    Engine::Editor::PmxImportReport report;
    std::string error;
    if (!Engine::Editor::PmxModelDecoder::Decode(
            source.string(), model, error, &report))
    {
        std::cerr << "Skin-mode PMX fixture: " << error << '\n';
        assert(false);
    }
    const auto& vertices = model.primitives[0].vertices;
    assert(vertices[0].sdefCenterAndMode[3] == 3.f);
    assert(vertices[0].sdefCenterAndMode[2] == -3.f);
    assert(vertices[0].sdefRadius0[2] == -6.f);
    assert(vertices[0].sdefRadius1[2] == -9.f);
    assert(vertices[1].sdefCenterAndMode[3] == 0.f);
    assert(vertices[2].sdefCenterAndMode[3] == 4.f);
    const auto saved = outputRoot / "skin-modes.mesh";
    assert(Engine::Components::Mesh::SaveNativeFile(saved.string(), vertices));
    Engine::Components::Mesh mesh;
    mesh.LoadFromFile(saved.string());
    assert(mesh.GetVertices()[0].sdefCenterAndMode[0] == 1.f);
    assert(mesh.GetVertices()[0].sdefRadius1[1] == 8.f);
    assert(mesh.GetVertices()[2].sdefCenterAndMode[3] == 4.f);
}

void TestSkinPoseReferences()
{
    using Vertex = Engine::Components::Mesh::Vertex;
    Vertex sdef{}, qdef{};
    sdef.pos[0] = 2.f; sdef.pos[1] = 1.f;
    sdef.normal[2] = 1.f;
    sdef.joints0[0] = 0.f; sdef.joints0[1] = 1.f;
    sdef.weights0[0] = .25f; sdef.weights0[1] = .75f;
    sdef.sdefCenterAndMode[0] = 1.f;
    sdef.sdefCenterAndMode[3] = 3.f;
    sdef.sdefRadius0[1] = 1.f;
    sdef.sdefRadius1[1] = -1.f;
    qdef.pos[0] = 1.f;
    qdef.normal[0] = 1.f;
    qdef.joints0[0] = 0.f; qdef.joints0[1] = 1.f;
    qdef.weights0[0] = .5f; qdef.weights0[1] = .5f;
    qdef.sdefCenterAndMode[3] = 4.f;
    Engine::Components::Mesh mesh;
    assert(mesh.SetAuthoredVertices({ sdef, qdef, qdef }));
    const auto close = [](const Vertex& actual, glm::vec3 expected)
    {
        assert(glm::length(glm::vec3(actual.pos[0], actual.pos[1],
            actual.pos[2]) - expected) < 1e-4f);
    };
    const glm::vec3 p(sdef.pos[0], sdef.pos[1], sdef.pos[2]);
    const glm::vec3 center(sdef.sdefCenterAndMode[0], 0.f, 0.f);
    const glm::vec3 r0(0.f, 1.f, 0.f), r1(0.f, -1.f, 0.f);
    for (const float angle : { 0.f, 30.f, 90.f })
    {
        const glm::quat q0 = glm::angleAxis(glm::radians(-angle), glm::vec3(0, 0, 1));
        const glm::quat q1 = glm::angleAxis(glm::radians(angle), glm::vec3(0, 0, 1));
        const glm::vec3 translation(2.f, 1.f, 0.f);
        std::vector<glm::mat4> palette(2);
        palette[0] = glm::translate(glm::mat4(1.f), translation) * glm::mat4_cast(q0);
        palette[1] = glm::translate(glm::mat4(1.f), translation) * glm::mat4_cast(q1);
        const auto posed = mesh.BuildPosedVertices(&palette);
        const glm::quat blend = glm::slerp(q1, q0, .25f);
        const glm::vec3 sdefReference =
            .25f * (q0 * (p - center + r0) + translation) +
            .75f * (q1 * (p - center + r1) + translation) +
            blend * (center - .25f * r0 - .75f * r1);
        close(posed[0], sdefReference);
        // Equal QDEF weights on opposite rotations cancel the rotation,
        // while the common translation remains intact at every pose.
        close(posed[1], glm::vec3(1.f, 0.f, 0.f) + translation);
    }
    std::vector<glm::mat4> palette(2);
    palette[0] = glm::mat4(1.f);
    palette[1] = glm::mat4_cast(glm::angleAxis(glm::radians(90.f),
        glm::vec3(0, 0, 1)));
    const auto quarterTurn = mesh.BuildPosedVertices(&palette);
    const float diagonal = std::sqrt(.5f);
    close(quarterTurn[1], glm::vec3(diagonal, diagonal, 0.f));
    assert(glm::length(glm::vec3(quarterTurn[1].normal[0],
        quarterTurn[1].normal[1], quarterTurn[1].normal[2]) -
        glm::vec3(diagonal, diagonal, 0.f)) < 1e-4f);
}
}

int main(int argc, char** argv)
{
    namespace fs = std::filesystem;
    if (argc > 1 && std::string(argv[1]) == "--uv-only")
    {
        const fs::path root = fs::path(PMX_TEST_OUTPUT_ROOT) / "pmx-uv-fixture";
        fs::create_directories(root);
        TestFullUv(root);
        TestSkinModes(root);
        TestSkinPoseReferences();
        fs::remove_all(root);
        return 0;
    }
    const fs::path outputRoot = fs::path(PMX_TEST_OUTPUT_ROOT) /
        ("engine_pmx_import_" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(outputRoot);
    Engine::Model::ImportedModel model;
    std::string error;
    // The bundled model exercises the full asset writer but takes minutes.
    // The default CTest path uses small generated PMX fixtures instead.
    if (argc > 1 && std::string(argv[1]) == "--bundled")
    {
    const fs::path source = fs::path(PMX_TEST_ASSET_ROOT) /
        "Mococo/Mococo Abyssgard.pmx";
    Engine::Editor::PmxImportReport report;
    std::cerr << "Decoding PMX\n";
    if (!Engine::Editor::PmxModelDecoder::Decode(
        source.string(), model, error, &report))
    {
        std::cerr << "PMX decode failed: " << error << '\n';
        return 1;
    }
    std::cerr << "Decoded PMX\n";
    std::cerr << "Imported supported groups: " <<
        report.importedSupportedGroupMorphs << '\n';
    for (const auto& warning : report.warnings)
        std::cerr << warning << '\n';
    assert(!report.warnings.empty());
    assert(report.importedKinematicRigidBodies == 24);
    assert(model.rigidBodies.size() == 24);
    assert(report.importedAppendRules == 25);
    assert(report.importedIkChains == 4);
    assert(model.appendRules[0].deformOrder >= 0);
    assert(model.ikChains[0].deformOrder >= 0);
    assert(model.sourceFormat == "pmx");
    assert(model.skins.size() == 1);
    assert(model.skins[0].jointNodes.size() == 239);
    assert(!model.materials.empty() && !model.primitives.empty());
    {
        Engine::Model::ImportedModel dynamicModel;
        Engine::Editor::PmxImportReport dynamicReport;
        assert(Engine::Editor::PmxModelDecoder::Decode(source.string(),
            dynamicModel, error, &dynamicReport, { true, true, true, true }));
        assert(dynamicReport.importedDynamicRigidBodies == 133);
        assert(dynamicModel.rigidBodies.size() == 157);
        assert(dynamicReport.importedPhysicsJoints > 0);
        assert(dynamicReport.importedPhysicsJoints ==
            static_cast<int>(dynamicModel.physicsJoints.size()));
        assert(std::any_of(dynamicModel.rigidBodies.begin(),
            dynamicModel.rigidBodies.end(), [](const auto& body)
            { return body.mode != Engine::Model::RigidBodyMotion::FollowTransform &&
                body.mass > 0.f &&
                body.relatedBoneNode >= 0; }));
    }
    {
        Engine::Model::ImportedModel minimalModel;
        Engine::Editor::PmxImportReport minimalReport;
        assert(Engine::Editor::PmxModelDecoder::Decode(source.string(),
            minimalModel, error, &minimalReport, { false, false, false }));
        assert(!minimalModel.primitives.empty() && !minimalModel.skins.empty());
        assert(minimalModel.primitives[0].morphTargets.empty());
        assert(minimalModel.boneMorphs.empty() && minimalModel.materialMorphs.empty());
        assert(minimalModel.appendRules.empty() && minimalModel.ikChains.empty());
        assert(minimalModel.rigidBodies.empty());
        assert(minimalReport.importedSupportedGroupMorphs == 0);
        assert(minimalReport.importedBoneMorphs == 0);
        assert(minimalReport.importedMaterialMorphs == 0);
        assert(minimalReport.importedAppendRules == 0);
        assert(minimalReport.importedIkChains == 0);
        assert(minimalReport.importedKinematicRigidBodies == 0);
    }
    assert(std::all_of(model.materials.begin(), model.materials.end(),
        [](const auto& material) { return material.specularModel == "BlinnPhong"; }));
    assert(std::any_of(model.materials.begin(), model.materials.end(),
        [](const auto& material) { return material.builtinToon >= 0; }));
    size_t morphMeshes = 0;
    bool hasNonzeroMorphOffset = false;
    for (const auto& primitive : model.primitives)
    {
        assert(!primitive.vertices.empty());
        assert(primitive.vertices.size() % 3 == 0);
        assert(primitive.materialIndex >= 0);
        assert(primitive.skinIndex == 0);
        if (!primitive.morphTargets.empty()) ++morphMeshes;
        assert(primitive.morphTargets.size() == primitive.morphWeights.size());
        for (const auto& morph : primitive.morphTargets)
        {
            assert(!morph.name.empty());
            assert(morph.positions.empty() ||
                morph.positions.size() == primitive.vertices.size());
            assert(morph.uv.empty() || morph.uv.size() == primitive.vertices.size());
            for (const auto& offset : morph.positions)
                hasNonzeroMorphOffset |= offset.x != 0.f || offset.y != 0.f ||
                    offset.z != 0.f;
        }
    }
    assert(morphMeshes > 0 && hasNonzeroMorphOffset);

    std::cerr << "Writing PMX assets\n";
    const auto imported = Engine::Editor::ModelAssetWriter::Write(
        model, source.string(), outputRoot.string());
    std::cerr << "Wrote PMX assets: " << imported.message << "\n";
    assert(imported.success);
    assert(fs::exists(imported.prefabPath));
    {
        std::ifstream prefab(imported.prefabPath, std::ios::binary);
        const std::string serialized((std::istreambuf_iterator<char>(prefab)),
            std::istreambuf_iterator<char>());
        assert(serialized.find("PrimitiveObjectCollider") != std::string::npos);
        assert(serialized.find("Kinematic") != std::string::npos);
        assert(serialized.find("RigConstraints") != std::string::npos);
        if (!model.physicsJoints.empty())
            assert(serialized.find("RigPhysicsBinding") != std::string::npos);
        assert(serialized.find("RigController") == std::string::npos);
        assert(serialized.find("MaterialPropertyMorph") != std::string::npos);
    }
    assert(fs::exists(fs::path(imported.outputDirectory) /
        (fs::path(imported.outputDirectory).filename().string() + ".rig")));
    size_t morphFiles = 0;
    size_t mmdMaterials = 0;
    for (const auto& entry : fs::recursive_directory_iterator(imported.outputDirectory))
        if (entry.is_regular_file())
        {
            if (entry.path().extension() == ".morph") ++morphFiles;
            if (entry.path().extension() == ".material")
            {
                const auto contents = Engine::Serialization::JsonParseFile(
                    entry.path().string());
                if (contents["specularModel"].AsString() == "BlinnPhong")
                    ++mmdMaterials;
            }
        }
    assert(morphFiles == morphMeshes);
    assert(mmdMaterials == model.materials.size());
    }
    assert(outputRoot.parent_path() == fs::path(PMX_TEST_OUTPUT_ROOT));
    const fs::path groupFixture = outputRoot / "group.pmx";
    WriteGroupMorphFixture(groupFixture);
    Engine::Model::ImportedModel groupModel;
    Engine::Editor::PmxImportReport groupReport;
    assert(Engine::Editor::PmxModelDecoder::Decode(
        groupFixture.string(), groupModel, error, &groupReport));
    assert(groupReport.importedSupportedGroupMorphs == 2);
    assert(groupReport.importedBoneMorphs == 2);
    assert(groupModel.boneMorphs[0].offsets[0].translation.y == 2.f);
    assert(groupReport.importedMaterialMorphs == 2);
    assert(groupModel.fixedAxes.size() == 1);
    assert(groupModel.fixedAxes[0].axis.z == 1.f);
    assert(groupModel.externalParents.size() == 1);
    assert(groupModel.externalParents[0].key == 42);
    assert(groupModel.materialMorphs[0].offsets[0].diffuse.a == -.5f);
    assert(groupModel.materials[0].edgeColor.a == .4f);
    assert(groupModel.materials[0].edgeWidth == 2.f);
    assert(groupModel.materials[0].sphereMode == 1);
    assert(groupModel.materials[0].sphereTexture == "toon.png");
    assert(groupModel.materials[0].toonTexture == "toon.png");
    assert(groupModel.materialMorphs[0].offsets[0].edgeColor.x == .2f);
    assert(groupModel.materialMorphs[0].offsets[0].edgeWidth == .6f);
    assert(groupModel.materialMorphs[0].offsets[0].sphereTint.x == .7f);
    assert(groupModel.materialMorphs[0].offsets[0].toonTint.w == .4f);
    assert(groupModel.primitives.size() == 1);
    assert(groupModel.primitives[0].vertices[0].uv1[0] == .2f);
    assert(groupModel.primitives[0].morphTargets.size() == 5);
    assert(groupModel.boneMorphs[1].name == "mixed group");
    assert(groupModel.boneMorphs[1].offsets[0].translation.y == .5f);
    assert(groupModel.materialMorphs[1].name == "mixed group");
    assert(groupModel.materialMorphs[1].offsets[0].diffuse.a == -.25f);
    assert(groupModel.materialMorphs[1].offsets[0].edgeWidth == .3f);
    assert(groupModel.primitives[0].morphTargets[4].positions[0].x == .25f);
    // MMD sphere maps are BMP images with .sph/.spa suffixes. Exercise the
    // texture loader and the full PMX -> native material import for each mode.
    const auto writeSphereBmp = [](const fs::path& path)
    {
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + 4;
        info.biSize = sizeof(info);
        info.biWidth = 1;
        info.biHeight = 1;
        info.biPlanes = 1;
        info.biBitCount = 24;
        info.biSizeImage = 4;
        const uint8_t pixel[] = { 32, 64, 128, 0 };
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(&file), sizeof(file));
        output.write(reinterpret_cast<const char*>(&info), sizeof(info));
        output.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
        assert(output.good());
    };
    const auto sphereSources = outputRoot / "sphere-sources";
    const auto sphereImports = outputRoot / "sphere-imports";
    fs::create_directories(sphereSources);
    fs::create_directories(sphereImports);
    writeSphereBmp(sphereSources / "sphere.sph");
    writeSphereBmp(sphereSources / "sphere.spa");
    for (const auto& [mode, suffix] :
        std::array<std::pair<uint8_t, const char*>, 3>{
            std::pair<uint8_t, const char*>{ uint8_t{ 1 }, "sphere.sph" },
            { uint8_t{ 2 }, "sphere.spa" },
            { uint8_t{ 3 }, "sphere.sph" } })
    {
        const auto fixture = sphereSources /
            ("sphere_mode_" + std::to_string(mode) + ".pmx");
        WriteGroupMorphFixture(fixture, 0, 0, false, false,
            mode == 3, false, mode, suffix);
        Engine::Model::ImportedModel sphereModel;
        assert(Engine::Editor::PmxModelDecoder::Decode(
            fixture.string(), sphereModel, error));
        assert(sphereModel.materials[0].sphereMode == mode);
        assert(sphereModel.materials[0].sphereTexture == suffix);
        const auto sphereImport = Engine::Editor::ModelAssetWriter::Write(
            sphereModel, fixture.string(), sphereImports.string());
        assert(sphereImport.success);
        bool checkedMaterial = false;
        for (const auto& entry : fs::recursive_directory_iterator(
            sphereImport.outputDirectory))
            if (entry.path().extension() == ".material" &&
                entry.path().filename() != "Default.material")
            {
                Engine::Components::Material material;
                assert(material.LoadFromFile(entry.path().string()));
                assert(material.sphereMode == mode);
                assert(material.sphereTexture);
                assert(fs::exists(material.sphereTexture->GetFilePath()));
                assert(fs::path(material.sphereTexture->GetFilePath()).parent_path() ==
                    fs::path(sphereImport.outputDirectory) / "Textures");
                assert(material.sphereTexture->Load());
                assert(material.sphereTexture->GetPixels()[0] == 128);
                assert(material.sphereTexture->GetPixels()[1] == 64);
                assert(material.sphereTexture->GetPixels()[2] == 32);
                if (mode == 3)
                    assert(sphereModel.primitives[0].vertices[0].uv1[0] == .2f);
                checkedMaterial = true;
            }
        assert(checkedMaterial);
    }
    if (argc > 1 && std::string(argv[1]) == "--sphere-only")
    {
        fs::remove_all(outputRoot);
        return 0;
    }
    groupModel.unitScaleToMeters = .1f;
    const auto scaledImport = Engine::Editor::ModelAssetWriter::Write(
        groupModel, groupFixture.string(), outputRoot.string());
    assert(scaledImport.success);
    {
        bool foundEdgeMaterial = false;
        for (const auto& entry : fs::recursive_directory_iterator(
            scaledImport.outputDirectory))
            if (entry.path().extension() == ".material")
            {
                const auto contents = Engine::Serialization::JsonParseFile(
                    entry.path().string());
                foundEdgeMaterial |= contents["edgeWidth"].AsFloat() == 2.f &&
                    contents["edgeColor"].ArrayAt(3).AsFloat() == .4f &&
                    contents["sphereMode"].AsInt() == 1;
            }
        assert(foundEdgeMaterial);
    }
    Engine::Model::ImportedModel dynamicFixtureModel;
    Engine::Editor::PmxImportReport dynamicFixtureReport;
    assert(Engine::Editor::PmxModelDecoder::Decode(groupFixture.string(),
        dynamicFixtureModel, error, &dynamicFixtureReport,
        { true, true, true, true }));
    assert(dynamicFixtureReport.importedDynamicRigidBodies == 2);
    assert(dynamicFixtureReport.importedPhysicsJoints == 1);
    assert(dynamicFixtureModel.physicsJoints[0].bodyA == 0);
    assert(dynamicFixtureModel.physicsJoints[0].bodyB == 1);
    const fs::path extendedFixture = outputRoot / "flip_soft_body.pmx";
    WriteGroupMorphFixture(extendedFixture, 0, 0, false, true);
    Engine::Model::ImportedModel extendedModel;
    Engine::Editor::PmxImportReport extendedReport;
    assert(Engine::Editor::PmxModelDecoder::Decode(extendedFixture.string(),
        extendedModel, error, &extendedReport, { true, true, true, true }));
    assert(extendedReport.importedSupportedGroupMorphs == 3);
    assert(extendedReport.importedSoftBodies == 1);
    assert(extendedModel.materialMorphs.size() == 3);
    assert(extendedModel.materialMorphs.back().name == "flip");
    assert(extendedModel.materialMorphs.back().offsets[0].diffuse.a == -.25f);
    assert(std::abs(extendedModel.materialMorphs.back().offsets[0]
        .textureTint.r - .2f) < .0001f);
    assert(extendedModel.primitives[0].morphTargets.back().name == "flip");
    assert(extendedModel.primitives[0].morphTargets.back().positions[0].x == .25f);
    assert(extendedModel.softBodies[0].anchors.size() == 1);
    assert(extendedModel.softBodies[0].anchors[0].nearMode);
    assert(extendedModel.softBodies[0].pinnedVertices.size() == 1);
    assert(extendedModel.softBodies[0].collisionGroup == 3);
    assert(extendedModel.softBodies[0].nonCollisionMask == 4);
    assert(extendedModel.softBodies[0].clusterCount == 1);
    assert(extendedModel.softBodies[0].clusterIterations == 3);
    assert(extendedModel.softBodies[0].clusterHardness.x == .2f);
    assert(extendedModel.softBodies[0].clusterImpulseSplit.z == .7f);
    const auto extendedImport = Engine::Editor::ModelAssetWriter::Write(
        extendedModel, extendedFixture.string(), outputRoot.string());
    assert(extendedImport.success);
    Engine::Scene::Scene extendedScene;
    assert(Engine::Serialization::SceneSerializer::InstantiatePrefab(
        extendedScene, extendedImport.prefabPath));
    Engine::Components::Cloth* importedCloth = nullptr;
    for (const auto& object : extendedScene.GetObjects())
        if (auto* cloth = object->GetComponent<Engine::Components::Cloth>())
            importedCloth = cloth;
    assert(importedCloth && importedCloth->anchors.size() == 1 &&
        importedCloth->explicitPinPositions.size() == 1);
    assert(importedCloth->anchors[0].nearMode);
    assert(importedCloth->collisionLayer == 8);
    assert(importedCloth->collisionMask == 0xfffb);
    assert(importedCloth->clusterCount == 1);
    assert(importedCloth->clusterIterations == 3);
    assert(importedCloth->clusterHardness.y == .3f);
    importedCloth->windVelocity = { 0.f, 0.f, 1.f };
    importedCloth->windStrength = 10.f;
    Engine::Components::RigidBody* anchoredRigid = nullptr;
    for (const auto& object : extendedScene.GetObjects())
        if (auto* rigid = object->GetComponent<Engine::Components::RigidBody>())
        {
            rigid->useGravity = false;
            if (object->name == importedCloth->anchors[0].bodyName)
                anchoredRigid = rigid;
        }
    assert(anchoredRigid);
    extendedScene.Start();
    extendedScene.Update(1.f / 60.f);
    assert(importedCloth->IsSimulating());
    assert(importedCloth->GetActiveClusterCount() == 1);
    assert(importedCloth->GetActiveAnchorCount() == 1);
    for (int frame = 0; frame < 30; ++frame)
        extendedScene.Update(1.f / 60.f);
    assert(anchoredRigid->GetLinearVelocity().z > .001f);
    extendedScene.GetPhysics().Reset();
    extendedScene.Update(1.f / 60.f);
    assert(anchoredRigid->GetPhysicsBodyGeneration() > 0);
    assert(importedCloth->GetActiveAnchorCount() == 1);
    const uint64_t resetBodyGeneration =
        anchoredRigid->GetPhysicsBodyGeneration();
    anchoredRigid->MarkConfigurationDirty();
    extendedScene.Update(1.f / 60.f);
    assert(anchoredRigid->GetPhysicsBodyGeneration() > resetBodyGeneration);
    assert(importedCloth->GetActiveAnchorCount() == 1);
    {
    const fs::path ropeFixture = outputRoot / "rope_soft_body.pmx";
    WriteGroupMorphFixture(ropeFixture, 0, 0, false, true, false, false,
        1, "toon.png", true);
    Engine::Model::ImportedModel ropeModel;
    Engine::Editor::PmxImportReport ropeReport;
    assert(Engine::Editor::PmxModelDecoder::Decode(ropeFixture.string(),
        ropeModel, error, &ropeReport, { true, true, true, true }));
    assert(ropeReport.importedSoftBodies == 1);
    assert(ropeModel.softBodies[0].shape == Engine::Model::SoftBodyTopology::Rope);
    const auto ropeImport = Engine::Editor::ModelAssetWriter::Write(
        ropeModel, ropeFixture.string(), outputRoot.string());
    assert(ropeImport.success);
    Engine::Scene::Scene ropeScene;
    assert(Engine::Serialization::SceneSerializer::InstantiatePrefab(
        ropeScene, ropeImport.prefabPath));
    Engine::Components::Cloth* ropeCloth = nullptr;
    Engine::Components::Mesh* ropeMesh = nullptr;
    for (const auto& object : ropeScene.GetObjects())
        if (auto* cloth = object->GetComponent<Engine::Components::Cloth>())
        {
            ropeCloth = cloth;
            ropeMesh = object->GetComponent<Engine::Components::Mesh>();
        }
    assert(ropeCloth && ropeMesh && ropeCloth->topology == "Rope");
    // PMX winding reversal places source vertex 2 at render index 1.
    const float ropeRestZ = ropeMesh->GetVertices()[1].pos[2];
    ropeCloth->windVelocity = { 0.f, 0.f, 1.f };
    ropeCloth->windStrength = 10.f;
    ropeScene.Start();
    for (int frame = 0; frame < 30; ++frame)
        ropeScene.Update(1.f / 60.f);
    assert(ropeCloth->GetActiveLinkCount() == 2);
    assert(ropeCloth->GetActiveFaceCount() == 0);
    assert(ropeCloth->GetActiveAnchorCount() == 1);
    assert(ropeMesh->GetVertices()[1].pos[2] > ropeRestZ + .0001f);
    ropeCloth->ResetSimulation();
    assert(std::abs(ropeMesh->GetVertices()[1].pos[2] - ropeRestZ) < .0001f);
    }
    for (uint8_t type = 1; type <= 5; ++type)
    {
        const fs::path fixture = outputRoot /
            ("joint_" + std::to_string(type) + ".pmx");
        WriteGroupMorphFixture(fixture, type);
        Engine::Model::ImportedModel jointModel;
        Engine::Editor::PmxImportReport jointReport;
        assert(Engine::Editor::PmxModelDecoder::Decode(fixture.string(),
            jointModel, error, &jointReport, { true, true, true, true }));
        assert(jointReport.importedPhysicsJoints == 1);
        assert(static_cast<int>(jointModel.physicsJoints[0].type) == type);
        Engine::Components::RigController restored;
        restored.physicsJoints = jointModel.physicsJoints;
        Engine::Components::RigController roundTrip;
        roundTrip.Deserialize(restored.Serialize());
        assert(roundTrip.physicsJoints.size() == 1 &&
            static_cast<int>(roundTrip.physicsJoints[0].type) == type);
        const auto jointImport = Engine::Editor::ModelAssetWriter::Write(
            jointModel, fixture.string(), outputRoot.string());
        assert(jointImport.success);
        Engine::Scene::Scene jointScene;
        auto* jointRoot = Engine::Serialization::SceneSerializer::InstantiatePrefab(
            jointScene, jointImport.prefabPath);
        assert(jointRoot);
        jointScene.Start();
        jointScene.Update(1.f / 60.f);
    }
    const fs::path worldFixture = outputRoot / "world_joint.pmx";
    WriteGroupMorphFixture(worldFixture, 2, -1);
    Engine::Model::ImportedModel worldJointModel;
    Engine::Editor::PmxImportReport worldJointReport;
    assert(Engine::Editor::PmxModelDecoder::Decode(worldFixture.string(),
        worldJointModel, error, &worldJointReport,
        { true, true, true, true }));
    assert(worldJointModel.physicsJoints.size() == 1);
    assert(worldJointModel.physicsJoints[0].bodyA == -1);
    const auto worldJointImport = Engine::Editor::ModelAssetWriter::Write(
        worldJointModel, worldFixture.string(), outputRoot.string());
    assert(worldJointImport.success);
    Engine::Scene::Scene worldJointScene;
    assert(Engine::Serialization::SceneSerializer::InstantiatePrefab(
        worldJointScene, worldJointImport.prefabPath));
    worldJointScene.Start();
    worldJointScene.Update(1.f / 60.f);
    const fs::path impulseFixture = outputRoot / "impulse.pmx";
    WriteGroupMorphFixture(impulseFixture, 0, 0, true);
    Engine::Model::ImportedModel impulseModel;
    Engine::Editor::PmxImportReport impulseReport;
    assert(Engine::Editor::PmxModelDecoder::Decode(impulseFixture.string(),
        impulseModel, error, &impulseReport, { true, true, true, true }));
    assert(impulseModel.impulseMorphs.size() == 3);
    assert(impulseModel.impulseMorphs[0].name == "mixed group");
    assert(impulseModel.impulseMorphs[0].offsets[0].velocity.x == 2.5f);
    assert(impulseModel.impulseMorphs[1].name == "push");
    assert(impulseModel.impulseMorphs[2].name == "stop");
    const auto impulseImport = Engine::Editor::ModelAssetWriter::Write(
        impulseModel, impulseFixture.string(), outputRoot.string());
    assert(impulseImport.success);
    Engine::Scene::Scene impulseScene;
    auto* impulseRoot = Engine::Serialization::SceneSerializer::InstantiatePrefab(
        impulseScene, impulseImport.prefabPath);
    assert(impulseRoot);
    auto* impulseRig = impulseRoot->GetComponent<
        Engine::Components::RigMorphControls>();
    assert(impulseRig && impulseRig->impulseMorphs.size() == 3);
    assert(!impulseRig->impulseMorphs[0].offsets[0].bodyName.empty());
    auto* impulseBodyObject = impulseRoot->FindObjectInChildrenByName(
        impulseRig->impulseMorphs[0].offsets[0].bodyName);
    assert(impulseBodyObject);
    auto* impulseBody = impulseBodyObject->GetComponent<
        Engine::Components::RigidBody>();
    assert(impulseBody);
    impulseScene.Start();
    impulseRig->impulseMorphs[0].weight = 1.f;
    impulseRig->ApplyImpulseMorphs();
    assert(impulseBody->GetLinearVelocity().x > 0.f);
    impulseRig->impulseMorphs[0].weight = 0.f;
    impulseRig->impulseMorphs[2].weight = 1.f;
    impulseRig->ApplyImpulseMorphs();
    assert(std::abs(impulseBody->GetLinearVelocity().x) < 1e-5f);
    const auto dynamicImport = Engine::Editor::ModelAssetWriter::Write(
        dynamicFixtureModel, groupFixture.string(), outputRoot.string());
    assert(dynamicImport.success);
    {
        Engine::Scene::Scene dynamicScene;
        auto* dynamicRoot = Engine::Serialization::SceneSerializer::InstantiatePrefab(
            dynamicScene, dynamicImport.prefabPath);
        assert(dynamicRoot);
        auto* dynamicRig = dynamicRoot->GetComponent<
            Engine::Components::RigPhysicsBinding>();
        assert(dynamicRig && dynamicRig->physicsBones.size() == 2);
        assert(dynamicRig->physicsJoints.size() == 1);
        auto* bodyObject = dynamicRoot->FindObjectInChildrenByName(
            dynamicRig->physicsBones[0].bodyName);
        assert(bodyObject && bodyObject->GetComponent<
            Engine::Components::RigidBody>()->bodyType == "Dynamic");
        auto* partnerObject = dynamicRoot->FindObjectInChildrenByName(
            dynamicRig->physicsBones[1].bodyName);
        assert(partnerObject);
        dynamicScene.Start();
        dynamicScene.Update(1.f / 60.f);
        auto* firstBody = bodyObject->GetComponent<Engine::Components::RigidBody>();
        firstBody->AddImpulse({ -8.f, 0.f, 0.f });
        for (int frame = 0; frame < 45; ++frame)
            dynamicScene.Update(1.f / 60.f);
        const float separation = glm::distance(
            bodyObject->transform.GetWorldPosition(),
            partnerObject->transform.GetWorldPosition());
        assert(separation > 1.f && separation < 3.f);
    }
    {
        Engine::Scene::Scene scaledScene;
        auto* scaledRoot = Engine::Serialization::SceneSerializer::InstantiatePrefab(
            scaledScene, scaledImport.prefabPath);
        assert(scaledRoot && scaledRoot->transform.scale.x == .1f);
    }
    const auto& group = groupModel.primitives[0].morphTargets[3];
    assert(group.name == "group");
    assert(group.positions[0].x == .5f);
    assert(group.uv[0].x == .25f && group.uv[0].w == .75f);
    const fs::path uvMorphFile = outputRoot / "uv.morph";
    assert(Engine::Components::Mesh::SaveMorphFile(uvMorphFile.string(),
        groupModel.primitives[0].morphTargets));
    Engine::Components::Mesh uvMesh;
    assert(uvMesh.SetAuthoredVertices(groupModel.primitives[0].vertices));
    assert(uvMesh.LoadMorphFile(uvMorphFile.string(), 0, { 0.f, 0.f, 0.f, 1.f }));
    assert(uvMesh.GetMorphTargets()[3].uv[0].w == .75f);
    const auto posed = uvMesh.BuildPosedVertices();
    assert(posed[0].uv[0] == .25f);
    assert(posed[0].uv1[1] == 1.05f);
    TestFullUv(outputRoot);

    // Append transforms must not accumulate when no animation rewrites the pose.
    Engine::Scene::Scene rigScene;
    auto* rigRoot = rigScene.AddObject("Rig");
    auto* sourceBone = rigScene.AddObject("Source");
    auto* drivenBone = rigScene.AddObject("Driven");
    sourceBone->Parent = rigRoot;
    drivenBone->Parent = rigRoot;
    rigRoot->Children.push_back(sourceBone);
    rigRoot->Children.push_back(drivenBone);
    auto* rigModel = rigRoot->AddComponent<Engine::Components::Model>();
    rigModel->BindNode(0, sourceBone);
    rigModel->BindNode(1, drivenBone);
    auto* rig = rigRoot->AddComponent<Engine::Components::RigController>();
    Engine::Model::ImportedAppendRule append;
    append.nodeIndex = 1;
    append.sourceNodeIndex = 0;
    append.translation = true;
    append.influence = .5f;
    rig->appendRules.push_back(append);
    sourceBone->transform.position = { 2.f, 0.f, 0.f };
    rig->Apply();
    assert(drivenBone->transform.position.x == 1.f);
    auto* ikGoal = rigScene.AddObject("Goal");
    auto* ikJoint = rigScene.AddObject("Joint");
    auto* ikTip = rigScene.AddObject("Tip");
    ikGoal->Parent = rigRoot;
    ikJoint->Parent = rigRoot;
    ikTip->Parent = ikJoint;
    rigRoot->Children.push_back(ikGoal);
    rigRoot->Children.push_back(ikJoint);
    ikJoint->Children.push_back(ikTip);
    ikGoal->transform.position = { 0.f, 1.f, 0.f };
    ikTip->transform.position = { 1.f, 0.f, 0.f };
    rigModel->BindNode(2, ikGoal);
    rigModel->BindNode(3, ikTip);
    rigModel->BindNode(4, ikJoint);
    Engine::Model::ImportedIkChain ik;
    ik.controllerNodeIndex = 2;
    ik.effectorNodeIndex = 3;
    ik.iterations = 8;
    ik.angleLimit = 1.6f;
    ik.links.push_back({ 4 });
    rig->ikChains.push_back(ik);
    rig->Apply();
    assert(glm::distance(ikTip->transform.GetWorldPosition(),
        ikGoal->transform.GetWorldPosition()) < .01f);
    rig->ikChains[0].enabled = false;
    rig->Apply();
    assert(glm::distance(ikTip->transform.GetWorldPosition(),
        ikGoal->transform.GetWorldPosition()) > .5f);
    Engine::Components::RigController disabledIk;
    disabledIk.Deserialize(rig->Serialize());
    assert(!disabledIk.ikChains[0].enabled);
    rig->ikChains[0].enabled = true;
    rig->Apply();
    assert(glm::distance(ikTip->transform.GetWorldPosition(),
        ikGoal->transform.GetWorldPosition()) < .01f);
    rig->SetAnimatedIkSwitches({ { "Goal", false } });
    rig->Apply();
    assert(glm::distance(ikTip->transform.GetWorldPosition(),
        ikGoal->transform.GetWorldPosition()) > .5f);
    rig->ikChains[0].enabled = false;
    rig->SetAnimatedIkSwitches({ { "Goal", true } });
    rig->Apply();
    assert(glm::distance(ikTip->transform.GetWorldPosition(),
        ikGoal->transform.GetWorldPosition()) < .01f);
    rig->SetAnimatedIkSwitches({});
    rig->ikChains[0].enabled = true;
    Engine::Model::AnimationClip switchClip;
    switchClip.ikSwitches = { { 0.f, "Goal", true },
        { .5f, "Goal", false } };
    Engine::Model::AnimationClip restoredClip;
    restoredClip.Deserialize(switchClip.Serialize());
    assert(restoredClip.ikSwitches.size() == 2);
    assert(restoredClip.ikSwitches[1].time == .5f &&
        !restoredClip.ikSwitches[1].enabled);
    rig->ikChains[0].links[0].fixedAxis = true;
    rig->ikChains[0].links[0].axis = { 1.f, 0.f, 0.f };
    rig->Apply();
    assert(glm::distance(ikTip->transform.GetWorldPosition(),
        ikGoal->transform.GetWorldPosition()) > .5f);
    rig->ikChains[0].links[0].axis = { 0.f, 0.f, 1.f };
    rig->Apply();
    assert(glm::distance(ikTip->transform.GetWorldPosition(),
        ikGoal->transform.GetWorldPosition()) < .01f);
    auto* axisBone = rigScene.AddObject("Fixed axis");
    axisBone->Parent = rigRoot;
    rigRoot->Children.push_back(axisBone);
    rigModel->BindNode(6, axisBone);
    rig->fixedAxes.push_back({ 6, { 0.f, 0.f, 1.f } });
    axisBone->transform.rotation = { .4f, .3f, .2f };
    rig->Apply();
    const glm::quat projectedFirst(axisBone->transform.rotation);
    assert(std::abs(projectedFirst.x) < .01f &&
        std::abs(projectedFirst.y) < .01f);
    axisBone->transform.rotation = { .2f, .1f, .6f };
    rig->Apply();
    const glm::quat projectedSecond(axisBone->transform.rotation);
    assert(std::abs(projectedSecond.x) < .01f &&
        std::abs(projectedSecond.y) < .01f);
    auto* localBone = rigScene.AddObject("Local axis bone");
    localBone->Parent = rigRoot;
    rigRoot->Children.push_back(localBone);
    rigModel->BindNode(10, localBone);
    rig->localAxes.push_back({ 10, { 0.f, 0.f, 1.f },
        { -1.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } });
    localBone->transform.position = { 1.f, 0.f, 0.f };
    localBone->transform.rotation = { .5f, 0.f, 0.f };
    rig->Apply();
    assert(std::abs(localBone->transform.position.x) < .01f &&
        std::abs(localBone->transform.position.z - 1.f) < .01f);
    assert(std::abs(glm::quat(localBone->transform.rotation).x) < .01f &&
        std::abs(glm::quat(localBone->transform.rotation).z) > .1f);
    rig->Apply();
    assert(std::abs(localBone->transform.position.z - 1.f) < .01f);
    auto* externalTarget = rigScene.AddObject("External target");
    auto* externalBone = rigScene.AddObject("External bone");
    externalBone->Parent = rigRoot;
    rigRoot->Children.push_back(externalBone);
    rigModel->BindNode(7, externalBone);
    rig->externalParents.push_back({ 7, 42, 0, 7, false,
        "External target" });
    rig->Apply(); // Capture the binding pose.
    externalTarget->transform.position.x = 3.f;
    rig->Apply();
    assert(std::abs(externalBone->transform.position.x - 3.f) < .01f);
    auto* physicsBody = rigScene.AddObject("PMX physics body");
    physicsBody->AddComponent<Engine::Components::RigidBody>();
    auto* physicsBone = rigScene.AddObject("PMX physics bone");
    physicsBody->Parent = rigRoot;
    physicsBone->Parent = rigRoot;
    rigRoot->Children.push_back(physicsBody);
    rigRoot->Children.push_back(physicsBone);
    rigModel->BindNode(8, physicsBone);
    rig->physicsBones.push_back({ "PMX physics body", 8,
        Engine::Model::RigidBodyMotion::Simulated });
    rig->Apply();
    physicsBody->transform.position.x = 2.f;
    rig->PostPhysicsUpdate();
    assert(std::abs(physicsBone->transform.position.x - 2.f) < .01f);
    auto* postPhysicsFollower = rigScene.AddObject("Post physics follower");
    postPhysicsFollower->Parent = rigRoot;
    rigRoot->Children.push_back(postPhysicsFollower);
    rigModel->BindNode(9, postPhysicsFollower);
    Engine::Model::ImportedAppendRule postPhysicsAppend;
    postPhysicsAppend.nodeIndex = 9;
    postPhysicsAppend.sourceNodeIndex = 8;
    postPhysicsAppend.translation = true;
    postPhysicsAppend.influence = 1.f;
    postPhysicsAppend.afterPhysics = true;
    rig->appendRules.push_back(postPhysicsAppend);
    rig->Apply();
    assert(std::abs(postPhysicsFollower->transform.position.x) < .01f);
    rig->PostPhysicsUpdate();
    assert(std::abs(postPhysicsFollower->transform.position.x - 2.f) < .01f);
    rig->appendRules.pop_back();
    rig->Apply();
    assert(std::abs(externalBone->transform.position.x - 3.f) < .01f);
    auto* ikFollower = rigScene.AddObject("IK follower");
    ikFollower->Parent = rigRoot;
    rigRoot->Children.push_back(ikFollower);
    rigModel->BindNode(5, ikFollower);
    Engine::Model::ImportedAppendRule afterIk;
    afterIk.nodeIndex = 5;
    afterIk.sourceNodeIndex = 4;
    afterIk.rotation = true;
    afterIk.influence = 1.f;
    afterIk.deformLayer = 1;
    afterIk.deformOrder = 5;
    rig->appendRules.push_back(afterIk);
    rig->ikChains[0].deformOrder = 2;
    rig->Apply();
    assert(glm::distance(ikFollower->transform.rotation,
        ikJoint->transform.rotation) < .01f);
    rig->appendRules.back().deformLayer = -1;
    rig->Apply();
    assert(glm::length(ikFollower->transform.rotation) < .01f);
    // Post-physics IK must run before a later-layer append that reads its link.
    rig->appendRules.back().deformLayer = 1;
    rig->appendRules.back().afterPhysics = true;
    rig->ikChains[0].afterPhysics = true;
    ikJoint->transform.rotation = { 0.f, 0.f, 0.f };
    rig->Apply();
    assert(glm::distance(ikTip->transform.GetWorldPosition(),
        ikGoal->transform.GetWorldPosition()) > .5f);
    rig->PostPhysicsUpdate();
    assert(glm::distance(ikTip->transform.GetWorldPosition(),
        ikGoal->transform.GetWorldPosition()) < .01f);
    assert(glm::distance(ikFollower->transform.rotation,
        ikJoint->transform.rotation) < .01f);
    rig->ikChains[0].afterPhysics = false;
    rig->appendRules.pop_back();
    rig->externalParents[0].afterPhysics = true;
    externalTarget->transform.position.x = 4.f;
    rig->Apply();
    rig->PostPhysicsUpdate();
    assert(std::abs(externalBone->transform.position.x - 4.f) < .01f);
    rig->externalParents[0].afterPhysics = false;
    Engine::Model::ImportedBoneMorph boneMorph;
    boneMorph.name = "raise";
    boneMorph.weight = .5f;
    boneMorph.offsets.push_back({ 1, { 0.f, 2.f, 0.f } });
    rig->boneMorphs.push_back(boneMorph);
    rig->Apply();
    assert(drivenBone->transform.position.x == 1.f);
    assert(drivenBone->transform.position.y == 1.f);
    rig->Apply();
    assert(drivenBone->transform.position.y == 1.f);
    rig->SetAnimatedMorphWeights({ { "raise", 1.f } });
    rig->Apply();
    assert(drivenBone->transform.position.y == 2.f);
    rig->SetAnimatedMorphWeights({});
    rig->Apply();
    assert(drivenBone->transform.position.y == 1.f);
    Engine::Model::AnimationClip morphClip;
    morphClip.componentMorphs.push_back({ "raise", { 0.f, 1.f },
        { 0.f, 1.f } });
    Engine::Model::AnimationClip restoredMorphClip;
    restoredMorphClip.Deserialize(morphClip.Serialize());
    assert(restoredMorphClip.componentMorphs.size() == 1);
    assert(restoredMorphClip.componentMorphs[0].weights[1] == 1.f);
    Engine::Components::RigController restoredRig;
    restoredRig.Deserialize(rig->Serialize());
    assert(restoredRig.appendRules.size() == 1);
    assert(restoredRig.ikChains.size() == 1);
    assert(restoredRig.ikChains[0].links[0].nodeIndex == 4);
    assert(restoredRig.ikChains[0].links[0].fixedAxis);
    assert(restoredRig.ikChains[0].links[0].axis.z == 1.f);
    assert(restoredRig.ikChains[0].deformOrder == 2);
    assert(restoredRig.fixedAxes.size() == 1);
    assert(restoredRig.fixedAxes[0].nodeIndex == 6);
    assert(restoredRig.localAxes.size() == 1);
    assert(restoredRig.localAxes[0].nodeIndex == 10);
    assert(restoredRig.externalParents.size() == 1);
    assert(restoredRig.externalParents[0].key == 42);
    assert(restoredRig.externalParents[0].targetName == "External target");
    assert(restoredRig.physicsBones.size() == 1);
    assert(restoredRig.physicsBones[0].nodeIndex == 8);
    {
        Engine::Scene::Scene physicsScene;
        auto* physicsRoot = physicsScene.AddObject("PMX physics rig");
        auto* fallingBone = physicsScene.AddObject("Falling bone");
        auto* fallingBody = physicsScene.AddObject("Falling body");
        fallingBone->Parent = physicsRoot;
        fallingBody->Parent = physicsRoot;
        physicsRoot->Children.push_back(fallingBone);
        physicsRoot->Children.push_back(fallingBody);
        fallingBone->transform.position.y = 2.f;
        fallingBody->transform.position.y = 2.f;
        auto* fallingModel = physicsRoot->AddComponent<Engine::Components::Model>();
        fallingModel->BindNode(0, fallingBone);
        auto* fallingRig = physicsRoot->AddComponent<
            Engine::Components::RigController>();
        fallingRig->physicsBones.push_back({ "Falling body", 0,
            Engine::Model::RigidBodyMotion::Simulated });
        auto* collider = fallingBody->AddComponent<
            Engine::Components::PrimitiveObjectCollider>();
        collider->shape = "Sphere";
        collider->radius = .2f;
        auto* fallingRigid = fallingBody->AddComponent<
            Engine::Components::RigidBody>();
        fallingRigid->bodyType = "Dynamic";
        fallingRigid->mass = 1.f;
        physicsScene.Start();
        std::array<float, 2> playbackHeights{};
        for (int playback = 0; playback < 2; ++playback)
        {
            if (playback != 0)
            {
                physicsScene.GetPhysics().Reset();
                fallingBone->transform.position.y = 2.f;
                fallingBody->transform.position.y = 2.f;
                physicsScene.Start();
            }
            for (int frame = 0; frame < 30; ++frame)
                physicsScene.Update(1.f / 60.f);
            assert(fallingBone->transform.GetWorldPosition().y < 1.5f);
            assert(glm::distance(fallingBone->transform.GetWorldPosition(),
                fallingBody->transform.GetWorldPosition()) < .01f);
            playbackHeights[playback] =
                fallingBone->transform.GetWorldPosition().y;
        }
        assert(std::abs(playbackHeights[0] - playbackHeights[1]) < 1e-4f);
    }
    assert(restoredRig.boneMorphs.size() == 1);
    assert(restoredRig.boneMorphs[0].offsets[0].translation.y == 2.f);
    auto* materialObject = rigScene.AddObject("Material");
    materialObject->Parent = rigRoot;
    rigRoot->Children.push_back(materialObject);
    auto* material = materialObject->AddComponent<Engine::Components::Material>();
    material->edgeColor = groupModel.materials[0].edgeColor;
    material->edgeWidth = groupModel.materials[0].edgeWidth;
    auto* materialMorph = materialObject->AddComponent<
        Engine::Components::MaterialPropertyMorph>();
    materialMorph->morphs = groupModel.materialMorphs;
    materialMorph->morphs[0].weight = 1.f;
    materialMorph->Apply();
    assert(material->baseColorAlpha == .5f);
    assert(material->specularColor.x == .75f);
    assert(material->shininess == 40.f);
    assert(material->alphaMode == "Blend");
    assert(std::abs(material->edgeColor.r - .3f) < .0001f);
    assert(std::abs(material->edgeWidth - 2.6f) < .0001f);
    assert(std::abs(material->sphereTint.r - 1.7f) < .0001f);
    assert(std::abs(material->toonTint.a - 1.4f) < .0001f);
    materialMorph->Apply();
    assert(material->baseColorAlpha == .5f);
    materialMorph->morphs[0].weight = 0.f;
    materialMorph->Apply();
    assert(material->baseColorAlpha == 1.f);
    assert(material->specularColor.x == 1.f);
    assert(material->shininess == 32.f);
    assert(material->edgeWidth == 2.f);
    assert(material->sphereTint.r == 1.f);
    materialMorph->SetAnimatedMorphWeights({ { materialMorph->morphs[0].name, 1.f } });
    materialMorph->Apply();
    assert(material->baseColorAlpha == .5f);
    materialMorph->SetAnimatedMorphWeights({});
    materialMorph->Apply();
    assert(material->baseColorAlpha == 1.f);
    materialMorph->SetAnimatedMorphWeights({ { "mixed group", 1.f } });
    materialMorph->Apply();
    assert(material->baseColorAlpha == .75f);
    assert(material->shininess == 36.f);
    assert(std::abs(material->baseColorTextureTint.r - 1.2f) < .0001f);
    materialMorph->SetAnimatedMorphWeights({});
    materialMorph->Apply();
    Engine::Components::MaterialPropertyMorph restoredMaterialMorph;
    restoredMaterialMorph.Deserialize(materialMorph->Serialize());
    assert(restoredMaterialMorph.morphs.size() == 2);
    assert(restoredMaterialMorph.morphs[0].offsets[0].edgeWidth == .6f);
    assert(restoredMaterialMorph.morphs[0].offsets[0].sphereTint.x == .7f);
    assert(restoredMaterialMorph.morphs[0].offsets[0].toonTint.w == .4f);
    auto* authoredObject = rigScene.AddObject("Authored material morph");
    auto* authoredMaterial = authoredObject->AddComponent<Engine::Components::Material>();
    auto* authoredMorph = authoredObject->AddComponent<
        Engine::Components::MaterialPropertyMorph>();
    assert(authoredMorph->editorAddable);
    Engine::Model::ImportedMaterialMorph authoredEffect;
    authoredEffect.name = "fade";
    Engine::Model::ImportedMaterialMorphOffset authoredOffset;
    authoredOffset.additive = true;
    authoredOffset.diffuse.a = -.25f;
    authoredEffect.offsets.push_back(authoredOffset);
    authoredMorph->morphs.push_back(authoredEffect);
    authoredMorph->SetAnimatedMorphWeights({ { "fade", 1.f } });
    authoredMorph->Apply();
    assert(std::abs(authoredMaterial->baseColorAlpha - .75f) < .0001f);
    authoredMorph->SetAnimatedMorphWeights({});
    authoredMorph->Apply();
    assert(authoredMaterial->baseColorAlpha == 1.f);
    Engine::Model::ImportedMaterialMorph multiplyMorph;
    multiplyMorph.name = "multiply tint";
    multiplyMorph.weight = .5f;
    Engine::Model::ImportedMaterialMorphOffset multiplyOffset;
    multiplyOffset.edgeColor = glm::vec4(.5f);
    multiplyOffset.edgeWidth = .5f;
    multiplyOffset.sphereTint = glm::vec4(.5f);
    multiplyOffset.toonTint = glm::vec4(.5f);
    multiplyMorph.offsets.push_back(multiplyOffset);
    materialMorph->morphs.clear();
    materialMorph->morphs.push_back(multiplyMorph);
    materialMorph->Apply();
    assert(std::abs(material->edgeWidth - 1.5f) < .0001f);
    assert(std::abs(material->sphereTint.r - .75f) < .0001f);
    assert(std::abs(material->toonTint.a - .75f) < .0001f);
    materialMorph->morphs.clear();
    materialMorph->Apply();
    assert(material->edgeWidth == 2.f);
    rig->Apply();
    assert(drivenBone->transform.position.x == 1.f);
    fs::remove_all(outputRoot);

    const fs::path truncated = fs::path(PMX_TEST_OUTPUT_ROOT) /
        "engine_pmx_truncated.pmx";
    { std::ofstream output(truncated, std::ios::binary); output.write("PMX ", 4); }
    assert(!Engine::Editor::PmxModelDecoder::Decode(truncated.string(), model, error));
    assert(error.find("Truncated") != std::string::npos);
    fs::remove(truncated);
}
