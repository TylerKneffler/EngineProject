#include "Server/Application/ServerApplication.h"

#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Object.h"
#include "Core/Physics/Physics.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#ifdef ENGINE_BUILTIN_ASSET_SCRIPTS
#include "Core/Assets/Scripts/Controllers/FirstPersonController.h"
#include "Core/Assets/Scripts/Gameplay/MainMenuGameManager.h"
#include "Core/Assets/Scripts/TerrainGen/TerrainGen.h"
#include "Core/Assets/Scripts/Portals/PortalSplitAfterDelay.h"
#include "Core/Assets/Scripts/Utilities/Rotate.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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
    Engine::Serialization::RegisterComponentType<PortalSplitAfterDelay>(
        "PortalSplitAfterDelay");
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

void WriteVec3Array(std::ostream& output, const std::vector<glm::vec3>& values)
{
    output << '[';
    for (size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0u)
            output << ',';
        WriteVec3(output, values[index]);
    }
    output << ']';
}

uint64_t MeshPositionHash(const Engine::Components::Mesh& mesh)
{
    // Stable CPU-side fingerprint: catches an unexpected mesh upload/cut even
    // when its vertex count and bounds happen to be unchanged.
    uint64_t hash = 1469598103934665603ull;
    for (const Engine::Model::AnimationVertex& vertex : mesh.GetVertices())
    {
        for (const float coordinate : { vertex.pos[0], vertex.pos[1], vertex.pos[2] })
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &coordinate, sizeof(bits));
            hash ^= bits;
            hash *= 1099511628211ull;
        }
    }
    return hash;
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

void WriteTraversalRenderInstances(std::ostream& output,
    const Engine::Scene::Scene& scene, const Engine::Core::Object& object)
{
    std::vector<const Engine::Core::Object*> sceneObjects;
    for (const auto& root : scene.GetObjects())
        CollectObjects(root.get(), sceneObjects);

    output << ",\"traversalRenderInstances\":[";
    bool first = true;
    for (const Engine::Core::Object* sceneObject : sceneObjects)
    {
        if (!sceneObject)
            continue;
        const auto* manipulator = sceneObject->GetComponent<
            Engine::Components::SpatialManipulator>();
        if (!manipulator)
            continue;

        std::vector<Engine::Components::SpatialManipulator::TraversalRenderInstance>
            instances;
        manipulator->AppendTraversalRenderInstances(instances);
        for (const auto& instance : instances)
        {
            if (instance.object != &object)
                continue;
            if (!first)
                output << ',';
            first = false;
            output << "{\"remote\":"
                << (instance.remote ? "true" : "false")
                << ",\"worldPosition\":";
            WriteVec3(output, glm::vec3(instance.world[3]));
            output << '}';
        }
    }
    output << ']';
}

void WriteObjectState(std::ostream& output, const Engine::Scene::Scene& scene,
    const Engine::Core::Object& object)
{
    output << "{\"name\":";
    WriteJsonString(output, object.name);
    output << ",\"enabled\":" << (object.IsEnabledInHierarchy() ? "true" : "false")
        << ",\"worldPosition\":";
    WriteVec3(output, glm::vec3(object.transform.GetWorldMatrix()[3]));
    output << ",\"renderPosition\":";
    WriteVec3(output, glm::vec3(object.transform.GetWorldMatrixWithLayer()[3]));
    output << ",\"localScale\":";
    WriteVec3(output, object.transform.scale);

    if (const auto* body = object.GetComponent<Engine::Components::RigidBody>())
    {
        output << ",\"rigidBody\":{\"linearVelocity\":";
        WriteVec3(output, body->GetLinearVelocity());
        output << ",\"angularVelocity\":";
        WriteVec3(output, body->GetAngularVelocity());
        output << ",\"colliding\":" << (body->IsColliding() ? "true" : "false")
            << ",\"grounded\":" << (body->IsGrounded() ? "true" : "false")
            << ",\"portalLocalPiece\":"
            << (body->HasPortalLocalMeshCollider() ? "true" : "false")
            << ",\"portalRemotePieceCount\":"
            << scene.GetPhysics().GetPortalMeshColliderCount(*body)
            << '}';
    }

    if (const auto* mesh = object.GetComponent<Engine::Components::Mesh>())
    {
        output << ",\"mesh\":{\"vertexCount\":" << mesh->GetVertexCount()
            << ",\"positionHash\":" << MeshPositionHash(*mesh)
            << ",\"boundsMin\":";
        WriteVec3(output, mesh->GetBoundsMin());
        output << ",\"boundsMax\":";
        WriteVec3(output, mesh->GetBoundsMax());
        output << ",\"morphWeights\":[";
        const auto& morphWeights = mesh->GetMorphWeights();
        for (size_t index = 0; index < morphWeights.size(); ++index)
        {
            if (index != 0u)
                output << ',';
            output << morphWeights[index];
        }
        output << ']';
        output << '}';
    }

#ifdef ENGINE_BUILTIN_ASSET_SCRIPTS
    if (const auto* terrain = object.GetComponent<TerrainGen>())
    {
        output << ",\"terrainStreaming\":{\"loadedChunks\":"
            << terrain->GetLoadedChunkCount()
            << ",\"totalBuilt\":" << terrain->GetTotalChunksBuilt()
            << ",\"totalUnloaded\":" << terrain->GetTotalChunksUnloaded()
            << ",\"cachedChunks\":" << terrain->GetCachedChunkCount()
            << ",\"cacheHits\":" << terrain->GetMeshCacheHits()
            << ",\"cacheMisses\":" << terrain->GetMeshCacheMisses()
            << ",\"queuedChunks\":" << terrain->GetQueuedChunkCount()
            << ",\"generatingChunks\":" << terrain->GetInFlightChunkCount()
            << ",\"lastMilliseconds\":"
            << terrain->GetLastStreamingMilliseconds()
            << ",\"maximumMilliseconds\":"
            << terrain->GetMaximumStreamingMilliseconds() << '}';
    }
#endif

    WriteTraversalRenderInstances(output, scene, object);

    if (const auto* portal = object.GetComponent<
            Engine::Components::SpatialManipulator>())
    {
        const glm::mat4 sourceFrame = portal->GetPortalWorldFrame();
        output << ",\"portal\":{\"mode\":" << portal->connectionMode
            << ",\"validAperture\":"
            << (portal->IsValidPortalAperture() ? "true" : "false")
            << ",\"anchor\":";
        WriteVec3(output, glm::vec3(sourceFrame[3]));
        output << ",\"normal\":";
        WriteVec3(output, glm::vec3(sourceFrame[2]));
        output << ",\"aperture\":";
        WriteVec3Array(output, portal->GetWorldPortalShapePoints());
        output << ",\"connectionEnabled\":"
            << (object.transform.matrixLayer.connection.enabled ? "true" : "false");

        const auto* target = portal->ResolveTarget();
        output << ",\"target\":";
        if (!target || !target->Owner)
            output << "null";
        else
        {
            WriteJsonString(output, target->Owner->name);
            const glm::mat4 targetFrame = target->GetPortalWorldFrame();
            const glm::vec3 mappedAnchor = portal->MapWorldPointThroughPortalShape(
                glm::vec3(sourceFrame[3]), *target);
            const glm::vec3 mappedNormal = glm::normalize(glm::vec3(
                portal->GetPortalWorldTransformTo(*target) *
                glm::vec4(glm::vec3(sourceFrame[2]), 0.f)));
            output << ",\"targetAnchor\":";
            WriteVec3(output, glm::vec3(targetFrame[3]));
            output << ",\"targetNormal\":";
            WriteVec3(output, glm::vec3(targetFrame[2]));
            output << ",\"mappedAnchor\":";
            WriteVec3(output, mappedAnchor);
            output << ",\"mappedNormal\":";
            WriteVec3(output, mappedNormal);
        }
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
        WriteObjectState(std::cout, scene, *objects[index]);
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
