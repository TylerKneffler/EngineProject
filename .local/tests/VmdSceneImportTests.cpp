#include "Editor/Core/Importers/VmdSceneImporter.h"
#include "Core/Compoonents/Animation/SceneCameraLightTrack.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
namespace
{
void U32(std::vector<uint8_t>& bytes, uint32_t value)
{
    for (int i = 0; i < 32; i += 8) bytes.push_back(static_cast<uint8_t>(value >> i));
}
void F32(std::vector<uint8_t>& bytes, float value)
{
    uint32_t bits; std::memcpy(&bits, &value, 4); U32(bytes, bits);
}
void Fixed(std::vector<uint8_t>& bytes, const char* value, size_t width)
{
    const size_t length = std::strlen(value);
    bytes.insert(bytes.end(), value, value + length);
    bytes.insert(bytes.end(), width - length, 0);
}
void Camera(std::vector<uint8_t>& bytes, uint32_t frame, float x,
    float distance, uint32_t fov)
{
    U32(bytes, frame); F32(bytes, distance);
    F32(bytes, x); F32(bytes, 0.f); F32(bytes, 0.f);
    F32(bytes, 0.f); F32(bytes, 0.f); F32(bytes, 0.f);
    bytes.insert(bytes.end(), 24, 0);
    U32(bytes, fov); bytes.push_back(0);
}
void Light(std::vector<uint8_t>& bytes, uint32_t frame, float x, float color)
{
    U32(bytes, frame);
    F32(bytes, color); F32(bytes, color); F32(bytes, color);
    F32(bytes, x); F32(bytes, 0.f); F32(bytes, 0.f);
}
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
bool Near(float a, float b) { return std::abs(a - b) < 0.01f; }
}

int main()
{
    const fs::path testRoot = fs::current_path() / ".local" / "test-output" / "vmd-scene";
    try
    {
        fs::create_directories(testRoot);
        std::vector<uint8_t> bytes;
        Fixed(bytes, "Vocaloid Motion Data 0002", 30);
        Fixed(bytes, "scene", 20);
        U32(bytes, 1); // bone
        Fixed(bytes, "center", 15); U32(bytes, 0);
        F32(bytes, 1.f); F32(bytes, 0.f); F32(bytes, 0.f);
        F32(bytes, 0.f); F32(bytes, 0.f); F32(bytes, 0.f); F32(bytes, 1.f);
        bytes.insert(bytes.end(), 64, 0);
        U32(bytes, 1); // morph
        Fixed(bytes, "smile", 15); U32(bytes, 15); F32(bytes, 0.7f);
        U32(bytes, 2); Camera(bytes, 30, 10.f, -20.f, 60); Camera(bytes, 0, 0.f, -10.f, 40);
        U32(bytes, 2); Light(bytes, 30, 10.f, 1.f); Light(bytes, 0, 0.f, 0.f);
        U32(bytes, 1); U32(bytes, 15); bytes.push_back(1); F32(bytes, 0.25f);
        U32(bytes, 1); U32(bytes, 30); bytes.push_back(1); U32(bytes, 0);
        const fs::path source = testRoot / "source.vmd";
        { std::ofstream file(source, std::ios::binary); file.write(
            reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
        const auto result = Engine::Editor::VmdSceneImporter::Import(
            source.string(), testRoot.string());
        Check(result.success, result.message.c_str());
        Check(result.prefabPath.empty() &&
            fs::path(result.primaryAssetPath).extension() == ".animpack" &&
            fs::exists(result.primaryAssetPath),
            "VMD primary import result should be motion data");
        Check(!fs::exists(fs::path(result.outputDirectory) / "source VMD.prefab"),
            "VMD import should not generate a combined scene prefab");
        Check(fs::exists(fs::path(result.outputDirectory) / "Camera.prefab"),
            "Camera track prefab missing");
        Check(fs::exists(fs::path(result.outputDirectory) / "Light.prefab"),
            "Light track prefab missing");
        for (const char* name : { "Body", "Facial", "Shadow", "Properties" })
        {
            const auto track = Engine::Serialization::JsonParseFile(
                (fs::path(result.outputDirectory) / (std::string(name) + ".vmdtrack")).string());
            Check(track["frames"].ArraySize() == 1 && track["lastFrame"].AsInt() == 30,
                "Independent VMD data track missing or lost source timing");
        }
        std::string packError;
        const auto pack = Engine::Model::AnimationPackAsset::Load(
            (fs::path(result.outputDirectory) / "source.animpack").string(),
            packError);
        Check(pack && pack->clips.empty() && pack->sourceTracks.size() == 6,
            "VMD source pack did not preserve all six track types");
        const fs::path extractedCamera = fs::path(result.outputDirectory) /
            "ExtractedCamera.vmdtrack";
        Check(pack->ExtractSourceTrack("Camera", extractedCamera.string(), packError),
            "Camera could not be extracted from animation pack");
        const auto cameraData = Engine::Serialization::JsonParseFile(
            extractedCamera.string());
        Check(cameraData["kind"].AsString() == "Camera" &&
            cameraData["frames"].ArraySize() == 2 &&
            cameraData["lastFrame"].AsInt() == 30,
            "Extracted camera lost movement or source timing");
        Engine::Scene::Scene scene;
        auto* cameraObject = Engine::Serialization::SceneSerializer::InstantiatePrefab(
            scene, (fs::path(result.outputDirectory) / "Camera.prefab").string());
        auto* lightObject = Engine::Serialization::SceneSerializer::InstantiatePrefab(
            scene, (fs::path(result.outputDirectory) / "Light.prefab").string());
        Check(cameraObject && lightObject, "Separate VMD track prefabs did not reload");
        auto* cameraTrack = cameraObject->GetComponent<Engine::Components::SceneCameraLightTrack>();
        auto* lightTrack = lightObject->GetComponent<Engine::Components::SceneCameraLightTrack>();
        auto* camera = cameraObject->GetComponent<Engine::Components::Camera>();
        auto* light = lightObject->GetComponent<Engine::Components::Light>();
        Check(cameraTrack && lightTrack && camera && light, "Track components missing after reload");
        Check(cameraTrack->GetTypeName() == "SceneCameraLightTrack" &&
            lightTrack->GetTypeName() == "SceneCameraLightTrack",
            "VMD importer still writes a format-specific runtime track");
        Check(cameraTrack->cameras.size() == 2 && cameraTrack->lights.empty() &&
            lightTrack->lights.size() == 2 && lightTrack->cameras.empty(),
            "VMD tracks were not split by type");
        Check(cameraTrack->framesPerSecond == 30.f &&
            cameraTrack->cameras[1].interpolation[0] <= 1.f,
            "VMD camera interpolation was not converted to reusable track data");
        Engine::Components::SceneCameraLightTrack authored;
        authored.framesPerSecond = 24.f;
        Engine::Components::SceneCameraLightTrack::CameraKey authoredKey;
        authoredKey.frame = 48;
        authoredKey.interpolation[0] = 64.f / 127.f;
        authored.cameras.push_back(authoredKey);
        Check(Near(authored.Duration(), 2.f),
            "Reusable scene track ignored authored frame rate");
        Engine::Components::SceneCameraLightTrack restoredAuthored;
        restoredAuthored.Deserialize(authored.Serialize());
        Check(Near(restoredAuthored.Duration(), 2.f) &&
            Near(restoredAuthored.cameras[0].interpolation[0], 64.f / 127.f),
            "Reusable scene track changed authored timing or interpolation on reload");
        auto* oldTrackObject = scene.AddObject("Old VMD track");
        oldTrackObject->AddComponent<Engine::Components::Camera>();
        auto* oldTrack = oldTrackObject->AddComponent<
            Engine::Components::SceneCameraLightTrack>();
        Engine::Components::SceneCameraLightTrack::CameraKey oldKey;
        oldKey.interpolation[0] = 64.f;
        oldTrack->cameras.push_back(oldKey);
        std::string oldXml = Engine::Serialization::SceneSerializer::SaveObjectToString(
            *oldTrackObject);
        const std::string currentType = "SceneCameraLightTrack";
        const std::string legacyType = "VmdSceneTrack";
        for (size_t at = 0; (at = oldXml.find(currentType, at)) != std::string::npos;
            at += legacyType.size())
            oldXml.replace(at, currentType.size(), legacyType);
        auto* convertedTrackObject =
            Engine::Serialization::SceneSerializer::InstantiateObjectFromString(
                scene, oldXml);
        auto* convertedTrack = convertedTrackObject
            ? convertedTrackObject->GetComponent<Engine::Components::SceneCameraLightTrack>()
            : nullptr;
        Check(convertedTrack && convertedTrack->cameras.size() == 1 &&
            Near(convertedTrack->cameras[0].interpolation[0], 64.f / 127.f),
            "Serialized legacy VMD controls were not converted on load");
        Check(!Engine::Serialization::SceneSerializer::GetRegisteredFactory(
            "VmdSceneTrack"),
            "Legacy VMD component class remains registered");
        cameraTrack->Seek(0.5f);
        Check(Near(cameraObject->transform.position.x, 5.f) &&
            Near(cameraObject->transform.position.z, -15.f) && Near(camera->fov, 50.f),
            "Camera seek did not apply movement and FOV");
        Check(Near(lightObject->transform.position.x, 0.f),
            "Camera seek changed the separate light track");
        lightTrack->Seek(0.5f);
        Check(Near(lightObject->transform.position.x, 5.f) &&
            Near(light->color.r, 0.5f) && Near(camera->fov, 50.f),
            "Light seek did not apply only the light track");
        const auto prefabResult = Engine::Editor::VmdSceneImporter::Import(
            source.string(), testRoot.string(), true);
        Check(prefabResult.success && fs::exists(prefabResult.prefabPath),
            "Explicit VMD prefab import did not save a prefab");
        Engine::Scene::Scene prefabScene;
        auto* combined = Engine::Serialization::SceneSerializer::InstantiatePrefab(
            prefabScene, prefabResult.prefabPath);
        Check(combined && combined->Children.size() == 2,
            "Combined VMD prefab did not reload camera and light children");
        bool foundCamera = false;
        bool foundLight = false;
        for (auto* child : combined->Children)
        {
            foundCamera |= child->GetComponent<Engine::Components::Camera>() != nullptr &&
                child->GetComponent<Engine::Components::SceneCameraLightTrack>() != nullptr;
            foundLight |= child->GetComponent<Engine::Components::Light>() != nullptr &&
                child->GetComponent<Engine::Components::SceneCameraLightTrack>() != nullptr;
        }
        Check(foundCamera && foundLight,
            "Combined VMD prefab lost its track components");
        fs::remove_all(prefabResult.outputDirectory);
        fs::remove_all(result.outputDirectory);
        fs::remove(source);
        std::cout << "VMD pack preserves all six source tracks; separate and combined prefabs reload\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
