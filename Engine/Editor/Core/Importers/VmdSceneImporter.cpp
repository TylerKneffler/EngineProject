#include "VmdSceneImporter.h"
#include "VmdMotionDecoder.h"
#include "Core/AssetRecord.h"
#include "Core/Model/AnimationPackAsset.h"
#include "Core/Compoonents/Animation/SceneCameraLightTrack.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/Json.h"
#include "Core/Serialization/SceneSerializer.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace Engine::Editor
{
namespace fs = std::filesystem;
namespace
{
using JsonValue = Engine::Serialization::JsonValue;

template<typename T, size_t N>
JsonValue Numbers(const std::array<T, N>& values)
{
    JsonValue result = JsonValue::MakeArray();
    for (T value : values) result.Push(JsonValue(static_cast<double>(value)));
    return result;
}

template<typename Frame, typename Serialize>
std::string WriteTrack(const fs::path& output, const fs::path& source,
    const VmdMotion& motion, const char* kind,
    const std::vector<Frame>& frames, Serialize serialize)
{
    if (frames.empty()) return {};
    const fs::path path = output / (std::string(kind) + ".vmdtrack");
    std::ofstream file(path, std::ios::binary);
    file << "{\"version\":1,\"kind\":" <<
        Engine::Serialization::JsonWrite(JsonValue(kind)) <<
        ",\"sourceModel\":" <<
        Engine::Serialization::JsonWrite(JsonValue(motion.modelName)) <<
        ",\"sourceFps\":30,\"lastFrame\":" << motion.lastFrame <<
        ",\"frames\":[";
    for (size_t index = 0; index < frames.size(); ++index)
    {
        if (index) file << ',';
        file << Engine::Serialization::JsonWrite(serialize(frames[index]));
    }
    file << "]}";
    if (!file.good()) throw std::runtime_error("Could not save VMD track: " + path.string());
    file.close();
    if (!file) throw std::runtime_error("Could not finish VMD track: " + path.string());
    Engine::Core::AssetRecord::Ensure(path, source,
        { { "importer", std::string("vmd-track") }, { "trackKind", std::string(kind) } });
    std::ifstream saved(path, std::ios::binary);
    if (!saved) throw std::runtime_error("Could not reopen VMD track: " + path.string());
    return std::string(std::istreambuf_iterator<char>(saved),
        std::istreambuf_iterator<char>());
}
}

Engine::Model::ModelImportResult VmdSceneImporter::Import(
    const std::string& sourcePath, const std::string& assetsDirectory,
    bool createScenePrefab)
{
    Engine::Model::ModelImportResult result;
    VmdMotion motion;
    std::string error;
    if (!VmdMotionDecoder::DecodeFile(sourcePath, motion, error))
    { result.message = error; return result; }

    const fs::path source = fs::path(sourcePath).lexically_normal();
    const fs::path assets = fs::path(assetsDirectory).lexically_normal();
    const std::string base = source.stem().string() + " VMD";
    fs::path output = assets / base;
    for (unsigned suffix = 2; fs::exists(output); ++suffix)
        output = assets / (base + " " + std::to_string(suffix));
    try
    {
        fs::create_directories(output);
        fs::copy_file(source, output / source.filename());
        Engine::Core::AssetRecord::Ensure(output / source.filename(), source,
            { { "importer", std::string("vmd-source") } });

        Engine::Model::AnimationPackAsset pack;
        const auto appendTrack = [&](const char* kind, const auto& frames,
            auto serialize)
        {
            std::string json = WriteTrack(output, source, motion, kind,
                frames, serialize);
            if (!json.empty()) pack.sourceTracks.push_back({ kind, std::move(json) });
        };
        appendTrack("Body", motion.bones, [](const auto& key)
        { return JsonValue::MakeObject()
                .Set("name", JsonValue(key.name))
                .Set("frame", JsonValue(static_cast<double>(key.frame)))
                .Set("translation", Numbers(key.translation))
                .Set("rotation", Numbers(key.rotation))
                .Set("interpolation", Numbers(key.interpolation)); });
        appendTrack("Facial", motion.morphs, [](const auto& key)
        { return JsonValue::MakeObject()
                .Set("name", JsonValue(key.name))
                .Set("frame", JsonValue(static_cast<double>(key.frame)))
                .Set("weight", JsonValue(key.weight)); });
        appendTrack("Shadow", motion.shadows, [](const auto& key)
        { return JsonValue::MakeObject()
                .Set("frame", JsonValue(static_cast<double>(key.frame)))
                .Set("mode", JsonValue(static_cast<int>(key.mode)))
                .Set("distance", JsonValue(key.distance)); });
        appendTrack("Properties", motion.properties, [](const auto& key)
        {
            JsonValue switches = JsonValue::MakeArray();
            for (const auto& item : key.ik)
                switches.Push(JsonValue::MakeObject()
                    .Set("name", JsonValue(item.name))
                    .Set("enabled", JsonValue(item.enabled)));
            return JsonValue::MakeObject()
                .Set("frame", JsonValue(static_cast<double>(key.frame)))
                .Set("visible", JsonValue(key.visible))
                .Set("ik", std::move(switches)); });
        appendTrack("Camera", motion.cameras, [](const auto& key)
        { return JsonValue::MakeObject()
                .Set("frame", JsonValue(static_cast<double>(key.frame)))
                .Set("distance", JsonValue(key.distance))
                .Set("position", Numbers(key.position))
                .Set("rotation", Numbers(key.rotation))
                .Set("interpolation", Numbers(key.interpolation))
                .Set("fieldOfViewDegrees", JsonValue(
                    static_cast<double>(key.fieldOfViewDegrees)))
                .Set("perspective", JsonValue(key.perspective)); });
        appendTrack("Light", motion.lights, [](const auto& key)
        { return JsonValue::MakeObject()
                .Set("frame", JsonValue(static_cast<double>(key.frame)))
                .Set("color", Numbers(key.color))
                .Set("position", Numbers(key.position)); });
        const fs::path packPath = output / (source.stem().string() + ".animpack");
        if (!pack.Save(packPath.string(), error))
            throw std::runtime_error("Could not save VMD animation pack: " + error);
        Engine::Core::AssetRecord::Ensure(packPath, source,
            { { "importer", std::string("vmd-animation-pack") } });

        Engine::Scene::Scene scene;
        Engine::Core::Object* root = createScenePrefab
            ? scene.AddObject(base) : nullptr;
        if (!motion.cameras.empty())
        {
            auto* object = scene.AddObject("VMD Camera");
            if (root) { object->Parent = root; root->Children.push_back(object); }
            object->AddComponent<Engine::Components::Camera>();
            auto* track = object->AddComponent<Engine::Components::SceneCameraLightTrack>();
            track->kind = static_cast<int>(Engine::Components::SceneCameraLightTrack::Kind::Camera);
            for (const auto& sourceKey : motion.cameras)
            {
                Engine::Components::SceneCameraLightTrack::CameraKey key;
                key.frame = sourceKey.frame;
                key.distance = sourceKey.distance;
                key.target = sourceKey.position;
                key.rotation = sourceKey.rotation;
                for (size_t i = 0; i < key.interpolation.size(); ++i)
                    key.interpolation[i] = sourceKey.interpolation[i] / 127.f;
                key.fov = static_cast<float>(sourceKey.fieldOfViewDegrees);
                key.perspective = sourceKey.perspective;
                track->cameras.push_back(key);
            }
            const fs::path path = output / "Camera.prefab";
            if (!Engine::Serialization::SceneSerializer::SavePrefab(*object, path.string()))
                throw std::runtime_error("Could not save VMD camera prefab");
            Engine::Core::AssetRecord::Ensure(path, source,
                { { "importer", std::string("vmd-camera") } });
        }
        if (!motion.lights.empty())
        {
            auto* object = scene.AddObject("VMD Light");
            if (root) { object->Parent = root; root->Children.push_back(object); }
            auto* light = object->AddComponent<Engine::Components::Light>();
            light->lightType = static_cast<int>(Engine::Components::Light::Type::Directional);
            light->intensity = 1.f;
            auto* track = object->AddComponent<Engine::Components::SceneCameraLightTrack>();
            track->kind = static_cast<int>(Engine::Components::SceneCameraLightTrack::Kind::Light);
            for (const auto& sourceKey : motion.lights)
            {
                Engine::Components::SceneCameraLightTrack::LightKey key;
                key.frame = sourceKey.frame;
                key.color = sourceKey.color;
                key.position = sourceKey.position;
                track->lights.push_back(key);
            }
            const fs::path path = output / "Light.prefab";
            if (!Engine::Serialization::SceneSerializer::SavePrefab(*object, path.string()))
                throw std::runtime_error("Could not save VMD light prefab");
            Engine::Core::AssetRecord::Ensure(path, source,
                { { "importer", std::string("vmd-light") } });
        }
        if (createScenePrefab)
        {
            if (motion.cameras.empty() && motion.lights.empty())
                throw std::runtime_error(
                    "VMD has no camera or light tracks for a scene prefab");
            const fs::path prefab = output / (base + ".prefab");
            if (!Engine::Serialization::SceneSerializer::SavePrefab(*root,
                    prefab.string()))
                throw std::runtime_error("Could not save VMD scene prefab");
            Engine::Core::AssetRecord::Ensure(prefab, source,
                { { "importer", std::string("vmd-scene") },
                  { "sourceModel", motion.modelName },
                  { "lastFrame", static_cast<double>(motion.lastFrame) },
                  { "animationPack", packPath.generic_string() } });
            result.prefabPath = prefab.generic_string();
        }
        result.success = true;
        result.primaryAssetPath = packPath.generic_string();
        result.outputDirectory = output.generic_string();
        result.message = "Imported VMD animation pack and tracks from " +
            source.filename().string();
        if (!motion.bones.empty() || !motion.morphs.empty() ||
            !motion.shadows.empty() || !motion.properties.empty())
            result.message += "; character, shadow and property tracks need target binding";
    }
    catch (const std::exception& exception)
    {
        std::error_code cleanupError;
        if (output.parent_path().lexically_normal() == assets)
            fs::remove_all(output, cleanupError);
        result.message = exception.what();
    }
    return result;
}
}
