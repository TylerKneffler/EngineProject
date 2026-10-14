#include "Editor/Core/Importers/PmxModelDecoder.h"
#include "Editor/Core/Importers/ModelAssetWriter.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Animation/PmxRigController.h"
#include "Core/Model/AnimationClip.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/PmxMaterialMorph.h"
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
    bool impulseMorphs = false, bool extendedFeatures = false)
{
    std::vector<char> bytes{ 'P', 'M', 'X', ' ' };
    Append(bytes, jointType == 0 && !impulseMorphs && !extendedFeatures
        ? 2.f : 2.1f);
    for (uint8_t value : std::array<uint8_t, 9>{ 8, 1, 1, 1, 1, 1, 1, 1, 1 })
        Append(bytes, value);
    AppendText(bytes, "group test");
    for (int i = 0; i < 3; ++i) AppendText(bytes, "");
    Append(bytes, int32_t(3));
    for (int i = 0; i < 3; ++i)
    {
        AppendFloats(bytes, { float(i == 1), float(i == 2), 0.f,
            0.f, 0.f, 1.f, 0.f, 0.f });
        AppendFloats(bytes, { .2f, .3f, 0.f, 0.f }); // Additional UV1.
        Append(bytes, uint8_t(0)); // BDEF1.
        Append(bytes, int8_t(-1));
        Append(bytes, 1.f);
    }
    Append(bytes, int32_t(3));
    for (uint8_t i = 0; i < 3; ++i) Append(bytes, i);
    Append(bytes, int32_t(0)); // Textures.
    Append(bytes, int32_t(1)); // Materials.
    AppendText(bytes, "material"); AppendText(bytes, "");
    AppendFloats(bytes, { 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f,
        1.f, 0.f, 0.f, 0.f });
    Append(bytes, uint8_t(0));
    AppendFloats(bytes, { 0.f, 0.f, 0.f, 0.f, 1.f });
    Append(bytes, int8_t(-1)); // Base texture.
    Append(bytes, int8_t(-1)); // Sphere texture.
    Append(bytes, uint8_t(0)); // Sphere mode.
    Append(bytes, uint8_t(1)); // Built-in toon.
    Append(bytes, uint8_t(0));
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
    Append(bytes, int32_t(impulseMorphs ? 9 : extendedFeatures ? 8 : 7)); // Morphs.
    AppendText(bytes, "move"); AppendText(bytes, "");
    Append(bytes, uint8_t(4)); Append(bytes, uint8_t(1));
    Append(bytes, int32_t(1)); Append(bytes, uint8_t(0));
    AppendFloats(bytes, { 1.f, 0.f, 0.f });
    AppendText(bytes, "uv0"); AppendText(bytes, "");
    Append(bytes, uint8_t(4)); Append(bytes, uint8_t(3));
    Append(bytes, int32_t(1)); Append(bytes, uint8_t(0));
    AppendFloats(bytes, { 1.f, 0.f, 0.f, 0.f });
    AppendText(bytes, "uv1"); AppendText(bytes, "");
    Append(bytes, uint8_t(4)); Append(bytes, uint8_t(4));
    Append(bytes, int32_t(1)); Append(bytes, uint8_t(0));
    AppendFloats(bytes, { 0.f, 1.f, 0.f, 0.f });
    AppendText(bytes, "group"); AppendText(bytes, "");
    Append(bytes, uint8_t(4)); Append(bytes, uint8_t(0));
    Append(bytes, int32_t(3));
    Append(bytes, int8_t(0)); Append(bytes, .5f);
    Append(bytes, int8_t(1)); Append(bytes, .25f);
    Append(bytes, int8_t(2)); Append(bytes, .75f);
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
    for (int i = 0; i < 17; ++i) Append(bytes, i == 5 ? .4f : 0.f);
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
    if (jointType != 0 || impulseMorphs || extendedFeatures)
    {
        Append(bytes, int32_t(extendedFeatures ? 1 : 0));
        if (extendedFeatures)
        {
            AppendText(bytes, "cloth"); AppendText(bytes, "");
            Append(bytes, uint8_t(0)); Append(bytes, int8_t(0));
            Append(bytes, uint8_t(3)); Append(bytes, uint16_t(4));
            Append(bytes, uint8_t(0));
            Append(bytes, int32_t(0)); Append(bytes, int32_t(0));
            Append(bytes, 1.f); Append(bytes, .01f);
            Append(bytes, int32_t(0));
            AppendFloats(bytes, { 0.f, .1f, .01f, 0.f, 0.f, 0.f,
                .5f, 0.f, 0.f, 0.f, 0.f, 0.f });
            AppendFloats(bytes, { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f });
            Append(bytes, int32_t(0)); Append(bytes, int32_t(8));
            Append(bytes, int32_t(0)); Append(bytes, int32_t(0));
            AppendFloats(bytes, { .8f, .2f, 0.f });
            Append(bytes, int32_t(1)); // Anchor count.
            Append(bytes, int8_t(0)); Append(bytes, uint8_t(0));
            Append(bytes, uint8_t(0));
            Append(bytes, int32_t(1)); // Pin count.
            Append(bytes, uint8_t(1));
        }
    }
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
}

int main()
{
    namespace fs = std::filesystem;
    const fs::path source = fs::path(PMX_TEST_ASSET_ROOT) /
        "Mococo/Mococo Abyssgard.pmx";
    Engine::Model::ImportedModel model;
    std::string error;
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
            { return body.mode > 0 && body.mass > 0.f &&
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
        [](const auto& material) { return material.useMmdSpecular; }));
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

    const fs::path outputRoot = fs::path(PMX_TEST_OUTPUT_ROOT) /
        ("engine_pmx_import_" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(outputRoot);
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
        assert(serialized.find("PmxRigController") != std::string::npos);
        assert(serialized.find("PmxMaterialMorph") != std::string::npos);
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
                if (contents["useMmdSpecular"].AsBool())
                    ++mmdMaterials;
            }
        }
    assert(morphFiles == morphMeshes);
    assert(mmdMaterials == model.materials.size());
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
    assert(groupModel.primitives.size() == 1);
    assert(groupModel.primitives[0].vertices[0].uv1[0] == .2f);
    assert(groupModel.primitives[0].morphTargets.size() == 5);
    assert(groupModel.boneMorphs[1].name == "mixed group");
    assert(groupModel.boneMorphs[1].offsets[0].translation.y == .5f);
    assert(groupModel.materialMorphs[1].name == "mixed group");
    assert(groupModel.materialMorphs[1].offsets[0].diffuse.a == -.25f);
    assert(groupModel.primitives[0].morphTargets[4].positions[0].x == .25f);
    groupModel.unitScaleToMeters = .1f;
    const auto scaledImport = Engine::Editor::ModelAssetWriter::Write(
        groupModel, groupFixture.string(), outputRoot.string());
    assert(scaledImport.success);
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
    assert(extendedModel.softBodies[0].pinnedVertices.size() == 1);
    assert(extendedModel.softBodies[0].collisionGroup == 3);
    assert(extendedModel.softBodies[0].nonCollisionMask == 4);
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
    assert(importedCloth->collisionLayer == 8);
    assert(importedCloth->collisionMask == 0xfffb);
    extendedScene.Start();
    extendedScene.Update(1.f / 60.f);
    assert(importedCloth->IsSimulating());
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
        assert(jointModel.physicsJoints[0].type == type);
        Engine::Components::PmxRigController restored;
        restored.physicsJoints = jointModel.physicsJoints;
        Engine::Components::PmxRigController roundTrip;
        roundTrip.Deserialize(restored.Serialize());
        assert(roundTrip.physicsJoints.size() == 1 &&
            roundTrip.physicsJoints[0].type == type);
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
        Engine::Components::PmxRigController>();
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
    impulseRig->Apply();
    assert(impulseBody->GetLinearVelocity().x > 0.f);
    impulseRig->impulseMorphs[0].weight = 0.f;
    impulseRig->impulseMorphs[2].weight = 1.f;
    impulseRig->Apply();
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
            Engine::Components::PmxRigController>();
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
    auto* rig = rigRoot->AddComponent<Engine::Components::PmxRigController>();
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
    Engine::Components::PmxRigController disabledIk;
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
    auto* physicsBone = rigScene.AddObject("PMX physics bone");
    physicsBody->Parent = rigRoot;
    physicsBone->Parent = rigRoot;
    rigRoot->Children.push_back(physicsBody);
    rigRoot->Children.push_back(physicsBone);
    rigModel->BindNode(8, physicsBone);
    rig->physicsBones.push_back({ "PMX physics body", 8, 1 });
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
    rig->appendRules.pop_back();
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
    Engine::Components::PmxRigController restoredRig;
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
            Engine::Components::PmxRigController>();
        fallingRig->physicsBones.push_back({ "Falling body", 0, 1 });
        auto* collider = fallingBody->AddComponent<
            Engine::Components::PrimitiveObjectCollider>();
        collider->shape = "Sphere";
        collider->radius = .2f;
        auto* fallingRigid = fallingBody->AddComponent<
            Engine::Components::RigidBody>();
        fallingRigid->bodyType = "Dynamic";
        fallingRigid->mass = 1.f;
        physicsScene.Start();
        for (int frame = 0; frame < 30; ++frame)
            physicsScene.Update(1.f / 60.f);
        assert(fallingBone->transform.GetWorldPosition().y < 1.5f);
    }
    assert(restoredRig.boneMorphs.size() == 1);
    assert(restoredRig.boneMorphs[0].offsets[0].translation.y == 2.f);
    auto* materialObject = rigScene.AddObject("Material");
    materialObject->Parent = rigRoot;
    rigRoot->Children.push_back(materialObject);
    auto* material = materialObject->AddComponent<Engine::Components::Material>();
    auto* materialMorph = materialObject->AddComponent<
        Engine::Components::PmxMaterialMorph>();
    materialMorph->morphs = groupModel.materialMorphs;
    materialMorph->morphs[0].weight = 1.f;
    materialMorph->Apply();
    assert(material->baseColorAlpha == .5f);
    assert(material->specularColor.x == .75f);
    assert(material->shininess == 40.f);
    assert(material->alphaMode == "Blend");
    materialMorph->Apply();
    assert(material->baseColorAlpha == .5f);
    materialMorph->morphs[0].weight = 0.f;
    materialMorph->Apply();
    assert(material->baseColorAlpha == 1.f);
    assert(material->specularColor.x == 1.f);
    assert(material->shininess == 32.f);
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
    Engine::Components::PmxMaterialMorph restoredMaterialMorph;
    restoredMaterialMorph.Deserialize(materialMorph->Serialize());
    assert(restoredMaterialMorph.morphs.size() == 2);
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
