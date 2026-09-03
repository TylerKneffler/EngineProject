#include "Server/Application/ServerApplication.h"

#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#ifdef ENGINE_BUILTIN_ASSET_SCRIPTS
#include "Core/Assets/Scripts/FirstPersonController.h"
#include "Core/Assets/Scripts/MainMenuGameManager.h"
#include "Core/Assets/Scripts/Rotate.h"
#endif

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace Engine::Server
{
namespace
{
struct RunnerOptions
{
    std::string scenePath;
    int frames = 240;
    int every = 15;
    float deltaTime = 1.f / 60.f;
    bool showHelp = false;
};

void RegisterServerComponents()
{
#ifdef ENGINE_BUILTIN_ASSET_SCRIPTS
    Engine::Serialization::RegisterComponentType<Rotate>("Rotate");
    Engine::Serialization::RegisterComponentType<FirstPersonController>(
        "FirstPersonController");
    Engine::Serialization::RegisterComponentType<MainMenuGameManager>(
        "MainMenuGameManager");
#endif
}

void PrintUsage()
{
    std::cout << "Usage: Server.exe --scene <scene-file> [--frames N] "
        "[--every N] [--dt seconds]\n"
        "Runs a scene without graphics or a window and writes one JSON object "
        "per sampled frame to stdout.\n";
}

bool ParseNonNegativeInt(const char* text, int& value)
{
    try
    {
        size_t consumed = 0;
        const long parsed = std::stol(text, &consumed);
        if (text[consumed] != '\0' || parsed < 0 ||
            parsed > std::numeric_limits<int>::max())
        {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ParsePositiveFloat(const char* text, float& value)
{
    try
    {
        size_t consumed = 0;
        const float parsed = std::stof(text, &consumed);
        if (text[consumed] != '\0' || !std::isfinite(parsed) || parsed <= 0.f)
            return false;
        value = parsed;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ParseOptions(int argc, char** argv, RunnerOptions& options)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h")
        {
            options.showHelp = true;
            PrintUsage();
            return false;
        }
        if (argument == "--scene" && index + 1 < argc)
        {
            options.scenePath = argv[++index];
            continue;
        }
        if (argument == "--frames" && index + 1 < argc &&
            ParseNonNegativeInt(argv[++index], options.frames))
        {
            continue;
        }
        if (argument == "--every" && index + 1 < argc &&
            ParseNonNegativeInt(argv[++index], options.every) && options.every > 0)
        {
            continue;
        }
        if (argument == "--dt" && index + 1 < argc &&
            ParsePositiveFloat(argv[++index], options.deltaTime))
        {
            continue;
        }
        std::cerr << "Invalid server argument: " << argument << '\n';
        PrintUsage();
        return false;
    }

    if (options.scenePath.empty())
    {
        std::cerr << "A scene is required.\n";
        PrintUsage();
        return false;
    }
    return true;
}

void WriteJsonString(std::ostream& output, const std::string& value)
{
    output << '"';
    for (const char character : value)
    {
        switch (character)
        {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default: output << character; break;
        }
    }
    output << '"';
}

void WriteVec3(std::ostream& output, const glm::vec3& value)
{
    output << '[' << value.x << ',' << value.y << ',' << value.z << ']';
}

void CollectObjects(const Engine::Core::Object* object,
    std::vector<const Engine::Core::Object*>& output)
{
    if (!object)
        return;
    output.push_back(object);
    for (const Engine::Core::Object* child : object->Children)
        CollectObjects(child, output);
}

void WriteObjectState(std::ostream& output, const Engine::Core::Object& object)
{
    output << "{\"name\":";
    WriteJsonString(output, object.name);
    output << ",\"enabled\":" << (object.IsEnabledInHierarchy() ? "true" : "false")
        << ",\"worldPosition\":";
    WriteVec3(output, glm::vec3(object.transform.GetWorldMatrix()[3]));

    if (const auto* body = object.GetComponent<Engine::Components::RigidBody>())
    {
        output << ",\"rigidBody\":{\"linearVelocity\":";
        WriteVec3(output, body->GetLinearVelocity());
        output << ",\"angularVelocity\":";
        WriteVec3(output, body->GetAngularVelocity());
        output << ",\"colliding\":" << (body->IsColliding() ? "true" : "false")
            << ",\"grounded\":" << (body->IsGrounded() ? "true" : "false")
            << '}';
    }

    if (const auto* mesh = object.GetComponent<Engine::Components::Mesh>())
    {
        output << ",\"mesh\":{\"vertexCount\":" << mesh->GetVertexCount()
            << ",\"boundsMin\":";
        WriteVec3(output, mesh->GetBoundsMin());
        output << ",\"boundsMax\":";
        WriteVec3(output, mesh->GetBoundsMax());
        output << '}';
    }

    output << '}';
}

void WriteSceneState(const Engine::Scene::Scene& scene, int frame, float time)
{
    std::vector<const Engine::Core::Object*> objects;
    for (const auto& root : scene.GetObjects())
        CollectObjects(root.get(), objects);

    std::cout << std::setprecision(7)
        << "{\"type\":\"scene_state\",\"frame\":" << frame
        << ",\"time\":" << time << ",\"objects\":[";
    for (size_t index = 0; index < objects.size(); ++index)
    {
        if (index != 0u)
            std::cout << ',';
        WriteObjectState(std::cout, *objects[index]);
    }
    std::cout << "]}\n";
}
}

int ServerApplication::Run(int argc, char** argv)
{
    RunnerOptions options;
    if (!ParseOptions(argc, argv, options))
        return options.showHelp ? 0 : 1;

    RegisterServerComponents();
    Engine::Scene::Scene scene;
    // Scene::Load is deliberately renderer-facing and rejects a null graphics
    // provider. The serializer supports headless deserialization directly;
    // meshes and other CPU scene state remain available for simulation.
    if (!Engine::Serialization::SceneSerializer::Load(scene, options.scenePath,
            nullptr))
    {
        std::cerr << "Could not load scene: " << options.scenePath << '\n';
        return 1;
    }

    scene.Start();
    // Establish static bodies and portal links before the initial sample.
    scene.Update(0.f);
    std::cout << "{\"type\":\"scene_loaded\",\"scene\":";
    WriteJsonString(std::cout, options.scenePath);
    std::cout << ",\"headless\":true,\"frames\":" << options.frames << ",\"dt\":"
        << std::setprecision(7) << options.deltaTime << "}\n";
    WriteSceneState(scene, 0, 0.f);

    for (int frame = 1; frame <= options.frames; ++frame)
    {
        scene.Update(options.deltaTime);
        if (frame % options.every == 0 || frame == options.frames)
            WriteSceneState(scene, frame, frame * options.deltaTime);
    }
    return 0;
}
}
