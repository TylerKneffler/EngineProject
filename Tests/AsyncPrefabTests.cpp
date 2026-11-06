#include "Core/Serialization/PreparedPrefab.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using Engine::Serialization::JsonValue;
using Engine::Serialization::PreparedPrefab;
using Engine::Serialization::SceneSerializer;
using Engine::Components::Mesh;
namespace fs = std::filesystem;
void Check(bool result, const char* message)
{ if (!result) throw std::runtime_error(message); }

struct OwnerProbe : Engine::Core::Component
{
    inline static std::thread::id owner = std::this_thread::get_id();
    inline static int constructions = 0;
    OwnerProbe()
    {
        Check(std::this_thread::get_id() == owner, "custom component constructed on worker");
        ++constructions;
        SetTypeName("OwnerProbe");
    }
};

struct StubProvider : Engine::Graphics::IGraphicsProvider
{
    Engine::Graphics::IShaderCompiler* GetShaderCompiler() override { return nullptr; }
    Engine::Graphics::IGraphicsBufferFactory* GetBufferFactory() override { return nullptr; }
    Engine::Graphics::IPipelineStateFactory* GetPipelineStateFactory() override { return nullptr; }
    Engine::Graphics::IGraphicsContextFactory* GetContextFactory() override { return nullptr; }
};

struct MemoryBuffer : Engine::Graphics::IGraphicsBuffer
{
    Usage usage;
    AccessMode access;
    uint32_t stride;
    std::vector<unsigned char> bytes;
    MemoryBuffer(Usage u, AccessMode a, uint64_t size, const void* initial, uint32_t s)
        : usage(u), access(a), stride(s), bytes(static_cast<size_t>(size))
    { if (initial) std::memcpy(bytes.data(), initial, bytes.size()); }
    Usage GetUsage() const override { return usage; }
    AccessMode GetAccessMode() const override { return access; }
    uint64_t GetSize() const override { return bytes.size(); }
    uint32_t GetElementStride() const override { return stride; }
    void* Map() override { return bytes.data(); }
    void Unmap() override {}
    void* GetNativeHandle() const override { return nullptr; }
};

struct MemoryBufferFactory : Engine::Graphics::IGraphicsBufferFactory
{
    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> CreateBuffer(
        MemoryBuffer::Usage usage, MemoryBuffer::AccessMode access,
        uint64_t size, const void* initial, uint32_t stride) override
    { return std::make_unique<MemoryBuffer>(usage, access, size, initial, stride); }
};

struct UploadProbe : Engine::Core::Component
{
    bool ready = false;
    void PrepareGraphicsResources(IGraphicsProvider*) override
    {
        if (ready) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ready = true;
    }
};

void CheckPreparation()
{
    const auto directory = fs::path(".local/tests-generated") /
        ("async-prefab-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    const auto meshPath = (directory / "triangle.mesh").generic_string();
    const auto morphPath = (directory / "triangle.morph").generic_string();
    const auto prefabPath = (directory / "triangle.prefab").generic_string();
    std::vector<Mesh::Vertex> vertices(3);
    vertices[1].pos[0] = 1.f;
    vertices[2].pos[1] = 1.f;
    Check(Mesh::SaveNativeFile(meshPath, vertices), "mesh fixture failed");
    Mesh::MorphTarget morph;
    morph.name = "Lift";
    morph.positions.assign(3, glm::vec3(0.f, 0.f, .25f));
    morph.normals.assign(3, glm::vec3(0.f, .1f, 0.f));
    morph.uvChannels[4].assign(3, glm::vec4(1.f, 2.f, 3.f, 4.f));
    Check(Mesh::SaveMorphFile(morphPath, {morph}), "morph fixture failed");
    auto mesh = JsonValue::MakeObject().Set("type", JsonValue("Mesh"))
        .Set("file", JsonValue(meshPath)).Set("morphFile", JsonValue(morphPath));
    auto components = JsonValue::MakeArray();
    components.Push(mesh);
    components.Push(JsonValue::MakeObject().Set("type", JsonValue("Material")));
    components.Push(JsonValue::MakeObject().Set("type", JsonValue("Skeleton")));
    components.Push(JsonValue::MakeObject().Set("type", JsonValue("SkinnedMesh")));
    components.Push(JsonValue::MakeObject().Set("type", JsonValue("AnimationManager")));
    components.Push(JsonValue::MakeObject().Set("type", JsonValue("OwnerProbe")));
    auto object = JsonValue::MakeObject().Set("name", JsonValue("Triangle"))
        .Set("components", std::move(components)).Set("children", JsonValue::MakeArray());
    const auto document = JsonValue::MakeObject().Set("version", JsonValue(1))
        .Set("type", JsonValue("prefab")).Set("object", std::move(object));
    { std::ofstream file(prefabPath); file << Engine::Serialization::JsonWrite(document); }
    SceneSerializer::Register("OwnerProbe", [] { return new OwnerProbe; });
    OwnerProbe::constructions = 0;
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto future = std::async(std::launch::async, [=] { return PreparedPrefab::Load(prefabPath, canceled); });
    auto prepared = future.get();
    Check(prepared && OwnerProbe::constructions == 0, "worker constructed owner-only components");
    for (const auto& [key, queue] : prepared->components)
        for (const auto& component : queue)
            Check(component->Owner == nullptr, "prepared component has a live owner");
    Engine::Scene::Scene actual;
    Engine::Core::Object* root;
    {
        PreparedPrefab::Publication publication(*prepared);
        root = SceneSerializer::InstantiatePrefabFromValue(actual, prefabPath, prepared->document);
    }
    Check(root && OwnerProbe::constructions == 1, "owner publication failed");
    auto* loaded = root->GetComponent<Mesh>();
    Check(loaded && loaded->GetVertices().size() == 3 && loaded->GetMorphTargetCount() == 1,
        "prepared geometry or morphs were lost");
    Check(!loaded->IsReady(), "worker allocated GPU resources");
    for (const auto& [key, queue] : prepared->components)
        Check(queue.empty(), "publication decoded a component twice");
    Engine::Scene::Scene expected;
    auto* synchronous = SceneSerializer::InstantiatePrefab(expected, prefabPath);
    Check(synchronous && synchronous->PrefabSourceSnapshot == root->PrefabSourceSnapshot,
        "async and synchronous prefab state differ");
    auto* expectedMesh = synchronous->GetComponent<Mesh>();
    Check(expectedMesh && expectedMesh->GetVertices().size() == loaded->GetVertices().size() &&
        expectedMesh->GetMorphTargets()[0].positions == loaded->GetMorphTargets()[0].positions,
        "async geometry differs from synchronous decoding");
    MemoryBufferFactory buffers;
    loaded->CreateBuffer(&buffers);
    expectedMesh->CreateBuffer(&buffers);
    auto* actualDeltas = dynamic_cast<MemoryBuffer*>(loaded->GetMorphDeltaBuffer());
    auto* expectedDeltas = dynamic_cast<MemoryBuffer*>(expectedMesh->GetMorphDeltaBuffer());
    Check(actualDeltas && expectedDeltas && actualDeltas->bytes == expectedDeltas->bytes &&
        loaded->GetMorphDeltaStrideFloat4() == 9, "worker-packed morph buffer differs from synchronous upload");

    const auto parentPath = (directory / "parent.prefab").generic_string();
    auto children = JsonValue::MakeArray();
    children.Push(JsonValue::MakeObject().Set("prefab", JsonValue(prefabPath)));
    children.Push(JsonValue::MakeObject().Set("prefab", JsonValue(prefabPath)));
    auto parent = JsonValue::MakeObject().Set("name", JsonValue("Parent"))
        .Set("components", JsonValue::MakeArray()).Set("children", std::move(children));
    const auto parentDocument = JsonValue::MakeObject().Set("version", JsonValue(1))
        .Set("type", JsonValue("prefab")).Set("object", std::move(parent));
    { std::ofstream file(parentPath); file << Engine::Serialization::JsonWrite(parentDocument); }
    auto nestedFuture = std::async(std::launch::async, [=] { return PreparedPrefab::Load(parentPath, canceled); });
    auto nested = nestedFuture.get();
    Check(nested && nested->dependencies.size() == 1, "nested dependency preparation failed");
    Engine::Scene::Scene nestedScene;
    {
        PreparedPrefab::Publication publication(*nested);
        auto* parentRoot = SceneSerializer::InstantiatePrefabFromValue(nestedScene, parentPath, nested->document);
        Check(parentRoot && parentRoot->Children.size() == 2, "nested prefab publication failed");
    }
    Check(nestedScene.GetComponents<Mesh>().size() == 2, "repeated dependency lost an instance");
    for (const auto& [key, queue] : nested->components)
        Check(queue.empty(), "nested components were decoded again on the owner");
    canceled->store(true);
    Check(!PreparedPrefab::Load(prefabPath, canceled), "canceled preparation published a payload");
    fs::remove(parentPath);
    fs::remove(prefabPath); fs::remove(meshPath); fs::remove(morphPath); fs::remove(directory);
    SceneSerializer::Unregister("OwnerProbe");
}

void CheckBudgetAndRemoval()
{
    StubProvider provider;
    Engine::Scene::Scene scene;
    auto* first = scene.AddObject("First")->AddComponent<UploadProbe>();
    auto* removed = scene.AddObject("Removed");
    removed->AddComponent<UploadProbe>();
    auto* third = scene.AddObject("Third")->AddComponent<UploadProbe>();
    scene.QueueGraphicsPreparation(.1);
    scene.ProcessGraphicsPreparation(&provider);
    Check(first->ready && !third->ready && scene.HasPendingGraphicsPreparation(), "upload budget did not yield");
    scene.RemoveObject(removed);
    auto* added = scene.AddObject("Added during upload")->AddComponent<UploadProbe>();
    scene.QueueGraphicsPreparation(.1);
    int iterations = 0;
    while (scene.HasPendingGraphicsPreparation() && iterations++ < 10)
        scene.ProcessGraphicsPreparation(&provider);
    Check(third->ready && added->ready && !scene.HasPendingGraphicsPreparation(),
        "upload queue did not drain after removal/addition");
}

int main()
{
    try
    {
        CheckPreparation();
        CheckBudgetAndRemoval();
        std::cout << "Async prefab CPU publication, cancellation, parity, and upload budget passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
