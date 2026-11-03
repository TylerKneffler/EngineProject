#include "Core/Scene/Scene.h"
#include "Core/SceneManager.h"
#include "Core/Input/NativeInputSnapshot.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Compoonents/UI/Canvas.h"
#include "Core/Compoonents/UI/UIButton.h"
#include "Core/Compoonents/UI/UIObject.h"
#include "Core/Compoonents/UI/UIText.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Threading/ComponentUpdateScheduler.h"
#include "Core/Script.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/IO/DirectorySnapshotCache.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Compoonents/Audio/AudioSource.h"
#include "Core/Audio/Audio.h"
#include "Core/Rendering/ShadowReceiverCulling.h"
#include <atomic>
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Physics/Adapters/BulletPhysicsAdapter.h"
#include "Core/Physics/Physics.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Obj/TerrainObjectLayer.h"
#include "Core/Assets/Scripts/Grass/GrassField.h"
#include "Core/Renderers/DX11/DX11GraphicsContext.h"
#include "Core/Renderers/DX11/DX11GraphicsBuffer.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <future>
#include <filesystem>
#include <chrono>
#include <cmath>
#include "Core/Window.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX11/DX11EditorRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Renderers/Vulkan/VulkanGameRenderer.h"
#include <type_traits>
#include "Core/Compoonents/Camera/Camera.h"

void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

struct SnapshotStartProbe : Engine::Core::Component
{
    static inline std::atomic<int> starts{0};
    bool RequiresUpdate() const override { return false; }
    void Start() override { ++starts; }
};

void TestEngineScheduling()
{
    Engine::Threading::JobScheduler jobs(2);
    int first=0,second=0;
    auto a=jobs.Schedule([&] { first=7; });
    auto b=jobs.Schedule([&] { Require(first==7,"job ran before dependency"); second=11; },{a});
    auto c=jobs.Schedule([&] { Require(first+second==18,"dependency results were not visible"); },{a,b});
    jobs.Wait(c);
    const auto sceneOwner = std::this_thread::get_id();
    int ownerPhase = 0;
    auto ownerFirst = jobs.ScheduleOnOwner([&]
    {
        Require(std::this_thread::get_id() == sceneOwner,
            "owner phase ran on a worker");
        ownerPhase = 1;
    });
    auto ownerSecond = jobs.ScheduleOnOwner([&]
    {
        Require(std::this_thread::get_id() == sceneOwner && ownerPhase == 1,
            "owner phase ignored its dependency");
        ownerPhase = 2;
    }, {ownerFirst});
    jobs.Wait(ownerSecond);
    Require(ownerPhase == 2, "owner phase graph did not finish");
    {
        Engine::Threading::JobScheduler isolated(1);
        std::promise<void> entered,release; auto gate=release.get_future().share();
        auto occupied=isolated.Submit([&entered,gate] { entered.set_value(); gate.wait(); });
        entered.get_future().wait();
        const auto producer=std::this_thread::get_id();
        auto io=isolated.Submit([producer] { return std::this_thread::get_id()!=producer; });
        auto work=isolated.Schedule([] {});
        isolated.Wait(work); // Must skip the queued worker-only operation.
        release.set_value(); occupied.get();
        Require(io.get(),"cooperative wait stole asynchronous IO onto the producer");
    }
    auto identity=jobs.Submit([] { return std::this_thread::get_id(); });
    Require(identity.get()!=std::this_thread::get_id(),"queued CPU work stayed on caller");
    std::atomic<unsigned> visits{0};
    jobs.For(256,512,[&](size_t i) { ++visits; if(i==0) jobs.For(256,512,[&](size_t) { ++visits; }); });
    Require(visits==512,"nested jobs deadlocked, skipped or duplicated work");
    const auto owner=std::this_thread::get_id();
    jobs.For(512,SIZE_MAX,[&](size_t) { Require(std::this_thread::get_id()==owner,"thread-affine loop moved to workers"); });
    bool ran=false,failed=false;
    auto broken=jobs.Schedule([] { throw std::runtime_error("dependency failure"); });
    auto skipped=jobs.Schedule([&] { ran=true; },{broken});
    try { jobs.Wait(skipped); } catch(const std::runtime_error&) { failed=true; }
    Require(failed && !ran,"failed dependency did not propagate");
    Engine::Threading::JobScheduler serial(0);
    Require(serial.Submit([] { return 9; }).get()==9,"serial scheduler lost returned value");
    auto serialFailure=serial.Schedule([] { throw std::runtime_error("serial failure"); }); failed=false;
    try { serial.Wait(serialFailure); } catch(const std::runtime_error&) { failed=true; }
    Require(failed,"serial job failure was lost");
    std::atomic<unsigned> drained{0};
    { Engine::Threading::JobScheduler shutdown(2); for(int i=0;i<32;++i) shutdown.Schedule([&] { ++drained; }); }
    Require(drained==32,"scheduler shutdown abandoned jobs");
    std::cout<<"Engine jobs: dependency ordering, failure propagation, nested progress, serial mode and shutdown passed.\n";
}

struct UpdateAccessProbe : Engine::Core::Component
{
    std::function<void()> callback;
    UpdateAccess access;
    void Update() override { callback(); }
    CallbackPolicy GetCallbackPolicy() const override
    { return { UpdateThreadAffinity::Worker, UpdateParallelSafety::DeclaredAccess }; }
    std::optional<UpdateAccess> GetUpdateAccess() const override { return access; }
};

struct UpdatePrerequisiteProbe : UpdateAccessProbe {};
struct UpdateDependentProbe : UpdateAccessProbe {};

struct LegacyUpdateProbe : Engine::Core::Script
{
    std::function<void()> callback;
    void Update() override { callback(); }
};

struct NativeSpriteWorkerProbe : Engine::Components::SpriteAnimationManager
{
    std::thread::id updateThread, publishThread;
    void Update() override
    {
        SpriteAnimationManager::Update();
        updateThread = std::this_thread::get_id();
    }
    void PublishUpdateOnOwner() override
    {
        SpriteAnimationManager::PublishUpdateOnOwner();
        publishThread = std::this_thread::get_id();
    }
};

struct UnsafeWorkerProbe : Engine::Core::Component
{
    CallbackPolicy GetCallbackPolicy() const override
    { return { UpdateThreadAffinity::Worker, UpdateParallelSafety::Serial }; }
};

void TestComponentUpdateScheduling()
{
    using Scheduler = Engine::Threading::ComponentUpdateScheduler;
    Engine::Scene::Scene scene;
    std::atomic<int> value{0};
    auto* before = scene.AddObject("parallel first")->AddComponent<UpdatePrerequisiteProbe>();
    before->callback = [&] { Require(value.fetch_add(1) == 0, "prerequisite ran late"); };
    before->access.writes = {"shared"};
    auto* after = scene.AddObject("parallel second")->AddComponent<UpdateDependentProbe>();
    after->callback = [&] { Require(value.fetch_add(1) == 1, "dependent ran early"); };
    after->access.reads = {"shared"};
    after->access.after = {typeid(UpdatePrerequisiteProbe)};
    auto* legacy = scene.AddObject("legacy")->AddComponent<LegacyUpdateProbe>();
    legacy->callback = [&]
    {
        Require(scene.IsOwnerThread() && value.fetch_add(1) == 2,
            "legacy update escaped its serial barrier");
    };
    auto* last = scene.AddObject("parallel last")->AddComponent<UpdateAccessProbe>();
    last->access.writes = {"shared"};
    last->callback = [&] { Require(value.fetch_add(1) == 3, "update crossed legacy barrier"); };
    scene.Update(0.f);
    Require(value == 4, "component update dispatch skipped a callback");

    Engine::Scene::Scene nativeScene;
    auto* sprite = nativeScene.AddObject("native sprite")
        ->AddComponent<NativeSpriteWorkerProbe>();
    Require(sprite->LoadFromFile(
        "Engine/Core/Assets/Textures/Sprites/Slime/slime_idle.spriteanim"),
        "native sprite fixture did not load");
    nativeScene.Update(0.2f);
    Require(sprite->frame > 0, "native sprite frame did not advance");
    Require(sprite->publishThread == std::this_thread::get_id(),
        "native sprite shared state was not published on the owner");
    if (Engine::Threading::JobScheduler::Get().WorkerCount() > 0)
        Require(sprite->updateThread != sprite->publishThread,
            "eligible native update did not run on a worker");
    Engine::Scene::Scene invalidScene;
    auto* invalidObject = invalidScene.AddObject("invalid worker policy");
    invalidObject->AddComponent<UnsafeWorkerProbe>();
    bool invalidPolicyRejected = false;
    try { invalidScene.Update(0.f); }
    catch (const std::logic_error&) { invalidPolicyRejected = true; }
    Require(invalidPolicyRejected, "unsafe worker policy was accepted");
    invalidScene.RemoveObject(invalidObject);
    Require(invalidScene.GetObjects().empty(),
        "failed policy check left the scene in an update");

    UpdatePrerequisiteProbe late;
    UpdateDependentProbe early;
    value = 0;
    late.callback = [&] { value = 1; };
    early.callback = [&] { Require(value == 1, "later prerequisite was ignored"); };
    early.access.after = {typeid(UpdatePrerequisiteProbe)};
    Scheduler::Run({{&early, early.access}, {&late, late.access}});
    late.access.after = {typeid(UpdateDependentProbe)};
    bool cycleRejected = false;
    try { Scheduler::Run({{&early, early.access}, {&late, late.access}}); }
    catch (const std::logic_error&) { cycleRejected = true; }
    Require(cycleRejected, "component update dependency cycle was accepted");
    UpdateAccessProbe writer, reader;
    value = 0;
    writer.access.writes = {"shared"};
    reader.access.reads = {"shared"};
    writer.callback = [&] { value = 7; };
    reader.callback = [&] { Require(value == 7, "read overtook a declared write"); };
    Scheduler::Run({{&writer, writer.access}, {&reader, reader.access}});
    if (Engine::Threading::JobScheduler::Get().WorkerCount() >= 2)
    {
        UpdateAccessProbe left, right;
        left.access.reads = {"shared"};
        right.access.reads = {"shared"};
        std::promise<void> leftStarted, rightStarted;
        const auto leftReady = leftStarted.get_future().share();
        const auto rightReady = rightStarted.get_future().share();
        left.callback = [&]
        {
            leftStarted.set_value();
            Require(rightReady.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "independent component updates did not overlap");
        };
        right.callback = [&]
        {
            rightStarted.set_value();
            Require(leftReady.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                "independent component updates did not overlap");
        };
        Scheduler::Run({{&left, left.access}, {&right, right.access}});
    }
    std::cout << "Component update access, dependencies and legacy barriers passed.\n";
}

void TestSceneSnapshotCommit()
{
    SnapshotStartProbe::starts = 0;
    Engine::Scene::Scene scene;
    auto* root = scene.AddObject("Snapshot root");
    auto* material = root->AddComponent<Engine::Components::Material>();
    const auto rootId = scene.GetObjectId(root);
    scene.CommitPendingObjectChanges();
    scene.Start();
    const auto before = scene.GetPublishedSceneSnapshot();
    Require(before && before->objects.size() == 1 && before->objects[0].id == rootId,
        "initial scene snapshot was not published");
    scene.CommitPendingObjectChanges();
    Require(scene.GetPublishedSceneSnapshot() == before,
        "unchanged commit rebuilt the published snapshot");
    bool rejectedWorkerMutation = false;
    bool rejectedWorkerComponentWrite = false;
    bool rejectedImplicitGeneration = false;
    std::thread invalidProducer([&]
    {
        try { scene.AddObject("Invalid worker object"); }
        catch (const std::logic_error&) { rejectedWorkerMutation = true; }
        try { material->MarkConfigurationDirty(); }
        catch (const std::logic_error&) { rejectedWorkerComponentWrite = true; }
        try { scene.QueueAddObject("Worker without generation"); }
        catch (const std::logic_error&) { rejectedImplicitGeneration = true; }
    });
    invalidProducer.join();
    Require(rejectedWorkerMutation && rejectedWorkerComponentWrite &&
        rejectedImplicitGeneration &&
        scene.GetPublishedSceneSnapshot() == before,
        "live scene mutation from a worker bypassed the owner boundary");
    const float originalRoughness = material->roughnessFactor;
    Engine::Scene::Scene::ObjectId childId = 0;
    const auto sceneGeneration = scene.CaptureSceneGeneration();
    std::thread producer([&]
    {
        childId = scene.QueueAddObject(sceneGeneration, "Queued child");
        scene.QueueMoveObject(sceneGeneration, childId, rootId,
            Engine::Scene::Scene::ObjectPlacement::AsChild);
        scene.QueueAddComponent<Engine::Components::Material>(sceneGeneration, childId);
        scene.QueueAddComponent<SnapshotStartProbe>(sceneGeneration, childId);
        scene.QueueObjectFieldWrite(sceneGeneration, childId,
            [](auto& value) { value.transform.position.x = 3.f; });
        scene.QueueComponentWrite<Engine::Components::Material>(sceneGeneration, rootId,
            [](auto& value) { value.roughnessFactor = 0.25f; });
    });
    producer.join();
    Require(scene.FindObjectById(childId) == nullptr &&
        material->roughnessFactor == originalRoughness,
        "queued scene mutation escaped its commit boundary");
    scene.CommitPendingObjectChanges();
    const auto after = scene.GetPublishedSceneSnapshot();
    Require(after && after->generation > before->generation &&
        after->objects.size() == 2 && before->objects.size() == 1,
        "published snapshot changed in place or omitted queued creation");
    Require(after->objects[1].id == childId &&
        after->objects[1].parentId == rootId &&
        after->objects[1].position.x == 3.f &&
        after->objects[1].components.size() == 2 &&
        after->objects[0].components.size() == 1 &&
        after->objects[0].components[0].configurationRevision >
            before->objects[0].components[0].configurationRevision,
        "snapshot omitted committed hierarchy or component revision");
    Require((*before->objects[0].components[0].authoredState)[
            "roughnessFactor"].AsFloat() == originalRoughness &&
        (*after->objects[0].components[0].authoredState)[
            "roughnessFactor"].AsFloat() == 0.25f,
        "component snapshot did not retain copied configuration values");
    Require(scene.FindObjectById(childId)->Parent == root &&
        material->roughnessFactor == 0.25f &&
        SnapshotStartProbe::starts == 1,
        "queued hierarchy or component write was not committed");
    auto* child = scene.FindObjectById(childId);
    const auto localPosition = child->transform.position;
    Require(!scene.SetParentLocal(root, child),
        "serial hierarchy edit accepted a parent cycle");
    Require(scene.SetParentLocal(child, nullptr) &&
        child->Parent == nullptr && child->transform.position == localPosition &&
        scene.GetPublishedSceneSnapshot() == after,
        "serial hierarchy edit changed local transform or published early");
    scene.CommitPendingObjectChanges();
    Require(scene.GetPublishedSceneSnapshot()->objects[1].parentId == 0 &&
        after->objects[1].parentId == rootId,
        "serial hierarchy edit did not publish at the commit boundary");
    Require(scene.SetParentLocal(child, root),
        "serial hierarchy edit could not restore the parent");
    scene.CommitPendingObjectChanges();
    material->roughnessFactor = 0.5f;
    material->MarkConfigurationDirty();
    const auto beforeOwnerCommit = scene.GetPublishedSceneSnapshot();
    Require((*beforeOwnerCommit->objects[0].components[0].authoredState)[
            "roughnessFactor"].AsFloat() == 0.25f,
        "serial component edit changed the published snapshot early");
    scene.CommitPendingObjectChanges();
    Require((*scene.GetPublishedSceneSnapshot()->objects[0].components[0].authoredState)[
            "roughnessFactor"].AsFloat() == 0.5f,
        "serial component edit was absent after the commit boundary");
    const auto transientId = scene.QueueAddObject("Transient");
    scene.QueueRemoveObject(transientId);
    scene.CommitPendingObjectChanges();
    Require(scene.FindObjectById(transientId) == nullptr &&
        scene.GetPublishedSceneSnapshot()->objects.size() == 2,
        "create and remove in one commit left a live object");
    scene.QueueRemoveObject(rootId);
    scene.CommitPendingObjectChanges();
    Require(scene.GetPublishedSceneSnapshot()->objects.empty() &&
        scene.FindObjectById(rootId) == nullptr &&
        scene.FindObjectById(childId) == nullptr && after->objects.size() == 2,
        "removal did not publish an immutable empty scene");
    scene.AddObject("Clear boundary probe");
    Require(scene.GetPublishedSceneSnapshot()->objects.empty(),
        "serial creation published outside a commit boundary");
    scene.CommitPendingObjectChanges();
    const auto beforeClear = scene.GetPublishedSceneSnapshot();
    scene.ClearObjects();
    Require(beforeClear->objects.size() == 1 &&
        scene.GetPublishedSceneSnapshot()->objects.empty() &&
        scene.GetPublishedSceneSnapshot()->generation > beforeClear->generation,
        "scene clear did not publish through its commit boundary");
}

void TestSimulationOwnerHandoff()
{
    Engine::Scene::Scene scene;
    const auto windowThread = std::this_thread::get_id();
    std::promise<void> begin;
    auto ready = begin.get_future().share();
    std::exception_ptr failure;
    std::thread simulation([&]
    {
        ready.wait();
        try
        {
            auto* object = scene.AddObject("Simulation owner");
            object->AddComponent<Engine::Components::Material>();
            scene.CommitPendingObjectChanges();
            Engine::Core::NativeInputSnapshot input;
            input.keys['W'] = static_cast<SHORT>(0x8000);
            input.cursor = { 10, 20 };
            Engine::Core::SetCurrentNativeInputSnapshot(&input);
            Require((Engine::Core::ReadNativeKeyState('W') & 0x8000) != 0,
                "simulation did not read the captured key state");
            Engine::Core::RequestNativeCursorPosition({ 30, 40 });
            POINT cursor{};
            Require(Engine::Core::ReadNativeCursorPosition(cursor) &&
                cursor.x == 30 && cursor.y == 40 &&
                input.cursorPositionRequested,
                "simulation cursor request was not retained for window handoff");
            Engine::Core::SetCurrentNativeInputSnapshot(nullptr);
        }
        catch (...) { failure = std::current_exception(); }
        scene.TransferOwnerTo(windowThread);
    });
    scene.TransferOwnerTo(simulation.get_id());
    bool rejectedWindowMutation = false;
    try { scene.AddObject("Window thread mutation"); }
    catch (const std::logic_error&) { rejectedWindowMutation = true; }
    begin.set_value();
    simulation.join();
    if (failure) std::rethrow_exception(failure);
    Require(rejectedWindowMutation &&
        scene.GetPublishedSceneSnapshot()->objects.size() == 1,
        "simulation handoff failed to isolate the scene owner");
    scene.AddObject("Window owner restored");
    scene.CommitPendingObjectChanges();
}

void TestNativeUiInteractionOwner()
{
    Engine::Scene::Scene scene;
    auto* canvas = scene.AddObject("UI canvas");
    canvas->AddComponent<Engine::Components::Canvas>();
    auto* buttonObject = scene.AddObject("UI button");
    Require(scene.SetParentLocal(buttonObject, canvas),
        "native UI button did not join its canvas");
    auto* layout = buttonObject->AddComponent<Engine::Components::UIObject>();
    auto* text = buttonObject->AddComponent<Engine::Components::UIText>();
    layout->anchorMin = { 0.5f, 0.5f, 0.f };
    layout->anchorMax = layout->anchorMin;
    auto* button = buttonObject->AddComponent<Engine::Components::UIButton>();
    int clicks = 0;
    button->AddOnClickListener([&](auto&)
    {
        scene.AssertOwnerThread();
        ++clicks;
    });
    scene.Start();
    Engine::Core::NativeInputSnapshot input;
    input.focused = true;
    input.cursorInWindow = true;
    input.clientWidth = 800;
    input.clientHeight = 600;
    input.clientCursor = { 400, 300 };
    Engine::Core::SetCurrentNativeInputSnapshot(&input);
    input.keys[VK_LBUTTON] = static_cast<SHORT>(0x8000);
    scene.Update(1.f / 60.f);
    input.keys[VK_LBUTTON] = 0;
    scene.Update(1.f / 60.f);
    Engine::Core::SetCurrentNativeInputSnapshot(nullptr);
    Require(clicks == 1 && button->ConsumeClick(),
        "native UI click did not run on the scene owner during simulation");
    Require(scene.GetPreparedNativeUiLayout().size() == 1 &&
        scene.GetPreparedNativeUiLayout().front().text == text,
        "native UI layout was not prepared before render handoff");
    scene.ClearObjects();
    Require(scene.GetPreparedNativeUiLayout().empty(),
        "removed UI components survived in the prepared render layout");
}

void TestAsyncSceneOwnerCommit()
{
    Engine::Scene::Scene source;
    source.AddObject("Async replacement");
    const auto path = std::filesystem::temp_directory_path() /
        "engine-async-scene-owner-test.scene";
    {
        std::ofstream file(path, std::ios::binary);
        file << source.SaveToString();
        Require(file.good(), "async scene fixture could not be written");
    }

    Engine::Core::Window window(GetModuleHandleW(nullptr), L"Async scene owner", 64, 64);
    Engine::Renderers::DX11GameRenderer renderer;
    Require(renderer.Init(window.GetHWND(), 64, 64), "async scene renderer init failed");
    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    scene.AddObject("Old scene");
    Engine::Core::SceneManager::SetActiveScene(&scene);
    int loadedCallbacks = 0;
    Engine::Core::SceneManager::SetOnSceneLoaded([&](auto* loaded)
    {
        scene.AssertOwnerThread();
        Require(loaded == &scene, "scene-loaded callback received another scene");
        ++loadedCallbacks;
    });
    bool completed = false;
    Engine::Core::SceneManager::LoadSceneAsync(path.string(), [&](auto* loaded)
    {
        scene.AssertOwnerThread();
        Require(loaded == &scene && scene.FindObjectByName("Async replacement"),
            "async scene callback ran before the owner committed the scene");
        completed = true;
    });
    for (int attempt = 0; attempt < 200 && !completed; ++attempt)
    {
        Engine::Core::SceneManager::ProcessPendingSceneLoad();
        if (!completed) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Require(completed && loadedCallbacks == 1 &&
        !scene.FindObjectByName("Old scene"),
        "async scene was not applied once at the owner boundary");
    bool missingCompleted = false;
    Engine::Core::SceneManager::LoadSceneAsync(path.string() + ".missing",
        [&](auto* loaded)
        {
            scene.AssertOwnerThread();
            Require(!loaded, "missing async scene reported a successful load");
            missingCompleted = true;
        });
    for (int attempt = 0; attempt < 200 && !missingCompleted; ++attempt)
    {
        Engine::Core::SceneManager::ProcessPendingSceneLoad();
        if (!missingCompleted)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Require(missingCompleted && loadedCallbacks == 1 &&
        scene.FindObjectByName("Async replacement"),
        "failed async file read changed the active scene");
    const auto brokenPath = std::filesystem::temp_directory_path() /
        "engine-async-scene-partial-test.scene";
    {
        std::ofstream file(brokenPath, std::ios::binary);
        file << R"({"version":1,"objects":[{"name":"Partial replacement"},{"prefab":"missing-partial-test.prefab"}]})";
        Require(file.good(), "partial scene fixture could not be written");
    }
    bool brokenCompleted = false;
    const auto beforeBrokenLoad = scene.GetPublishedSceneSnapshot();
    Engine::Core::SceneManager::LoadSceneAsync(brokenPath.string(),
        [&](auto* loaded)
        {
            scene.AssertOwnerThread();
            Require(!loaded && scene.FindObjectByName("Async replacement") &&
                !scene.FindObjectByName("Partial replacement"),
                "failed async deserialization published a partial scene");
            brokenCompleted = true;
        });
    for (int attempt = 0; attempt < 200 && !brokenCompleted; ++attempt)
    {
        Engine::Core::SceneManager::ProcessPendingSceneLoad();
        if (!brokenCompleted)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Require(brokenCompleted && loadedCallbacks == 1,
        "failed async deserialization did not complete on the scene owner");
    const auto afterBrokenLoad = scene.GetPublishedSceneSnapshot();
    Require(afterBrokenLoad && beforeBrokenLoad &&
        afterBrokenLoad->objects.size() == beforeBrokenLoad->objects.size() &&
        afterBrokenLoad->objects.front().name == "Async replacement",
        "failed async deserialization left an incomplete published snapshot");
    int staleCallbacks = 0;
    Engine::Core::SceneManager::LoadSceneAsync(path.string(),
        [&](auto*) { ++staleCallbacks; });
    Engine::Core::SceneManager::SetActiveScene(nullptr);
    Engine::Core::SceneManager::SetActiveScene(&scene);
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        Engine::Core::SceneManager::ProcessPendingSceneLoad();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Require(staleCallbacks == 0 && loadedCallbacks == 1,
        "canceled scene load survived an active-scene generation change");
    Engine::Core::SceneManager::LoadSceneAsync(path.string(),
        [&](auto*) { ++staleCallbacks; });
    scene.FenceBackgroundWork();
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        Engine::Core::SceneManager::ProcessPendingSceneLoad();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Require(staleCallbacks == 0 && loadedCallbacks == 1,
        "scene lifecycle fence accepted an old asynchronous file result");
    auto callbackToken = std::make_shared<int>(1);
    std::weak_ptr<int> callbackLifetime = callbackToken;
    Engine::Core::SceneManager::LoadSceneAsync(path.string(),
        [callbackToken](auto*) {});
    callbackToken.reset();
    Engine::Core::SceneManager::CancelPendingSceneLoad();
    Require(callbackLifetime.expired(),
        "canceled scene reader retained a script callback");
    Engine::Core::SceneManager::SetOnSceneLoaded({});
    Engine::Core::SceneManager::SetActiveScene(nullptr);
    renderer.WaitIdle();
    std::filesystem::remove(path);
    std::filesystem::remove(brokenPath);
}

void TestLifecycleWorkerFenceAndClock()
{
    struct WorkerComponent final : Engine::Core::Component
    {
        std::atomic<int>& completed;
        std::thread worker;
        explicit WorkerComponent(std::atomic<int>& value) : completed(value) {}
        void Start() override
        {
            worker = std::thread([this]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                completed.fetch_add(1);
            });
        }
        void FenceBackgroundWork() override
        {
            if (worker.joinable()) worker.join();
        }
        ~WorkerComponent() override
        {
            if (worker.joinable()) std::terminate();
        }
    };
    Engine::Scene::Scene scene;
    std::atomic<int> completed{0};
    scene.AddObject("Worker")->AddComponent<WorkerComponent>(completed);
    scene.SetFixedTimeStep(1.0 / 60.0);
    scene.Start();
    scene.Update(2.f);
    Require(scene.GetFrameIndex() == 1 &&
        std::abs(scene.GetElapsedTime() - 1.0 / 60.0) < 1e-8,
        "fixed step followed wall-clock delta");
    scene.FenceBackgroundWork(); // Pause: no Update while the host is paused.
    Require(completed == 1 && scene.GetFrameIndex() == 1,
        "pause transition left scene work active or advanced the clock");
    scene.Update(10.f); // Resume: wall time spent paused is ignored.
    Require(scene.GetFrameIndex() == 2 &&
        std::abs(scene.GetElapsedTime() - 2.0 / 60.0) < 1e-8,
        "fixed step accumulated paused wall time");
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    const auto oldGeneration = scene.CaptureSceneGeneration();
    const bool hasSceneWorkers =
        Engine::Threading::JobScheduler::Get().WorkerCount() > 0;
    if (!hasSceneWorkers) release.set_value();
    const auto oldJob = scene.ScheduleSceneJob([&](auto generation)
    {
        entered.set_value();
        released.wait();
        scene.QueueAddObject(generation, "Stale worker object");
    });
    entered.get_future().wait();
    std::thread releaseWorker;
    if (hasSceneWorkers)
        releaseWorker = std::thread([&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            release.set_value();
        });
    scene.FenceBackgroundWork();
    if (releaseWorker.joinable()) releaseWorker.join();
    Engine::Threading::JobScheduler::Get().Wait(oldJob);
    Require(scene.CaptureSceneGeneration() != oldGeneration &&
        scene.QueueAddObject(oldGeneration, "Late old object") == 0,
        "lifecycle fence did not invalidate old worker commands");
    scene.CommitPendingObjectChanges();
    Require(!scene.FindObjectByName("Stale worker object") &&
        !scene.FindObjectByName("Late old object"),
        "stale scene-generation command survived the lifecycle fence");
    scene.ClearObjects(); // Stop must join before destroying components.
    scene.Start();
    Require(scene.GetFrameIndex() == 0 && scene.GetElapsedTime() == 0.0,
        "Play did not reset the simulation clock");
    scene.UseVariableTimeStep();
    scene.Update(0.25f);
    Require(scene.GetFrameIndex() == 1 &&
        std::abs(scene.GetElapsedTime() - 0.25) < 1e-8,
        "variable step did not resume from a reset clock");
}

void TestComponentLifecycleDelivery()
{
    struct Probe final : Engine::Core::Component
    {
        std::vector<std::string>& events;
        std::atomic<bool>& jobFinished;
        std::thread::id owner;
        Probe(std::vector<std::string>& value, std::atomic<bool>& finished)
            : events(value), jobFinished(finished), owner(std::this_thread::get_id()) {}
        bool RequiresUpdate() const override { return false; }
        void Record(const char* event)
        {
            Require(std::this_thread::get_id() == owner,
                "lifecycle callback left the simulation owner");
            events.emplace_back(event);
        }
        void Start() override { Record("Start"); }
        void Enabled() override { Record("Enabled"); }
        void Disabled() override { Record("Disabled"); }
        void OnDestroy() override
        {
            Require(jobFinished, "OnDestroy ran before a referencing scene job completed");
            Record("OnDestroy");
        }
    };
    Engine::Scene::Scene scene;
    std::vector<std::string> events;
    std::atomic<bool> jobFinished{false};
    auto* object = scene.AddObject("Lifecycle probe");
    object->AddComponent<Probe>(events, jobFinished);
    scene.Start();
    scene.Start();
    object->Disabled();
    object->Disabled();
    object->Enabled();
    object->Enabled();
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    const bool hasWorkers = Engine::Threading::JobScheduler::Get().WorkerCount() > 0;
    if (!hasWorkers) release.set_value();
    auto job = scene.ScheduleSceneJob([&](auto)
    {
        entered.set_value();
        released.wait();
        jobFinished = true;
    });
    entered.get_future().wait();
    std::thread unblock;
    if (hasWorkers) unblock = std::thread([&]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        release.set_value();
    });
    scene.RemoveObject(object);
    if (unblock.joinable()) unblock.join();
    Engine::Threading::JobScheduler::Get().Wait(job);
    Require(events == std::vector<std::string>{"Start", "Enabled", "Disabled",
        "Enabled", "Disabled", "OnDestroy"},
        "lifecycle callbacks were duplicated or out of order");
}

void TestDeferredGraphicsPreparation()
{
    struct Provider final : Engine::Graphics::IGraphicsProvider
    {
        Engine::Graphics::IShaderCompiler* GetShaderCompiler() override { return nullptr; }
        Engine::Graphics::IGraphicsBufferFactory* GetBufferFactory() override { return nullptr; }
        Engine::Graphics::IPipelineStateFactory* GetPipelineStateFactory() override { return nullptr; }
        Engine::Graphics::IGraphicsContextFactory* GetContextFactory() override { return nullptr; }
        Engine::Graphics::IGraphicsTextureFactory* GetTextureFactory() override { return nullptr; }
    } provider;
    struct Probe final : Engine::Core::Component
    {
        int& cpu;
        int& graphics;
        Probe(int& cpuCount, int& graphicsCount) : cpu(cpuCount), graphics(graphicsCount) {}
        bool RequiresUpdate() const override { return false; }
        void OnAfterDeserialize(IGraphicsProvider* provider) override
        {
            Require(!provider, "CPU deserialization received a graphics provider");
            ++cpu;
        }
        void PrepareGraphicsResources(IGraphicsProvider* provider) override
        {
            Require(provider != nullptr, "graphics stage lacked a provider");
            ++graphics;
        }
    };
    Engine::Scene::Scene scene;
    int cpu = 0, graphics = 0;
    auto* component = scene.AddObject("Deferred resource")->AddComponent<Probe>(cpu, graphics);
    component->OnAfterDeserialize(nullptr);
    scene.QueueGraphicsPreparation();
    Require(cpu == 1 && graphics == 0, "deserialization created resources early");
    scene.ProcessGraphicsPreparation(&provider);
    scene.ProcessGraphicsPreparation(&provider);
    Require(graphics == 1, "graphics preparation was lost or duplicated");
    scene.ClearObjects();
    scene.QueueGraphicsPreparation();
    scene.ProcessGraphicsPreparation(&provider);
    Require(graphics == 1, "destroyed component received graphics preparation");
}

void TestScriptModuleJobFence()
{
    Engine::Threading::JobScheduler scheduler(1);
    Engine::Threading::ScriptModuleJobs host(scheduler);
    Engine::Threading::ScriptModuleJobsSlot() = &host;
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    std::atomic<bool> ranQueued{false};
    auto running = Engine::Core::Script::SubmitJob([&]
    {
        entered.set_value();
        released.wait();
    });
    entered.get_future().wait();
    auto queued = Engine::Core::Script::SubmitJob([&]
    {
        ranQueued = true;
    });
    std::thread unblock([&]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        release.set_value();
    });
    host.CloseAndWait();
    unblock.join();
    Engine::Threading::ScriptModuleJobsSlot() = nullptr;
    running.get();
    Require(!ranQueued && queued.wait_for(std::chrono::seconds(0)) ==
        std::future_status::ready,
        "module fence did not cancel and complete queued code");
    Require(!host.Submit([] {}), "closed module accepted another callback");
}

void TestPhasedPhysicsStep()
{
    const auto addDrop = [](Engine::Scene::Scene& scene)
    {
        auto* floor = scene.AddObject("Floor");
        floor->transform.position = {0.f, -0.5f, 0.f};
        floor->AddComponent<Engine::Components::PrimitiveObjectCollider>()
            ->size = {10.f, 1.f, 10.f};
        floor->AddComponent<Engine::Components::RigidBody>()->bodyType = "Static";
        auto* drop = scene.AddObject("Drop");
        drop->transform.position = {0.f, 2.f, 0.f};
        drop->AddComponent<Engine::Components::PrimitiveObjectCollider>();
        drop->AddComponent<Engine::Components::RigidBody>();
        // Cross the physics registry's worker-batch threshold while keeping
        // the suspended bodies out of Bullet's world.
        for (int index = 0; index < 260; ++index)
        {
            auto* inactive = scene.AddObject("Suspended body");
            inactive->AddComponent<Engine::Components::RigidBody>()
                ->SetCollisionSuspended(true);
        }
        scene.Start();
        return drop;
    };
    Engine::Scene::Scene direct, scheduled;
    auto* directDrop = addDrop(direct);
    auto* scheduledDrop = addDrop(scheduled);
    for (int frame = 0; frame < 30; ++frame)
    {
        direct.GetPhysics().Step(1.0 / 60.0);
        scheduled.Update(1.f / 60.f);
        Require(direct.GetPhysics().GetLastSubstepCount() ==
            scheduled.GetPhysics().GetLastSubstepCount(),
            "scheduled physics changed fixed-step substep count");
    }
    const double directY = directDrop->transform.position.y;
    const double scheduledY = scheduledDrop->transform.position.y;
    Require(std::abs(directY - scheduledY) < 0.001 && directY < 2.0,
        "scheduled physics changed the rigid-body trajectory");
}

void TestSceneGpuDestructionBarrier()
{
    Engine::Scene::Scene scene;
    int waits = 0;
    scene.SetBeforeGpuResourceDestruction([&]
    {
        scene.AssertOwnerThread();
        ++waits;
    });
    auto* removed = scene.AddObject("Removed after GPU idle");
    scene.RemoveObject(removed);
    Require(waits == 1, "object removal bypassed the GPU destruction barrier");
    scene.AddObject("Cleared after GPU idle");
    scene.ClearObjects();
    Require(waits == 2, "scene clear bypassed the GPU destruction barrier");
    scene.SetBeforeGpuResourceDestruction({});
}

void TestMeshBufferRetirement()
{
    Engine::Core::Window window(GetModuleHandleW(nullptr),
        L"Mesh buffer retirement regression", 64, 64);
    Engine::Renderers::DX11GameRenderer renderer;
    Require(renderer.Init(window.GetHWND(), 64, 64),
        "mesh retirement renderer failed");
    Engine::Scene::Scene scene;
    scene.SetDeferredGraphicsRetirement(true);
    auto* mesh = scene.AddObject("Replace live mesh buffers")
        ->AddComponent<Engine::Components::Mesh>();
    std::vector<Engine::Components::Mesh::Vertex> vertices(3);
    vertices[0].pos[0] = -1.f;
    vertices[1].pos[0] = 1.f;
    vertices[2].pos[1] = 1.f;
    Require(mesh->SetAuthoredVertices(vertices), "initial mesh geometry failed");
    mesh->CreateBuffer(renderer.GetGraphicsProvider()->GetBufferFactory());
    auto* original = mesh->GetGraphicsBuffer();
    vertices[0].pos[0] = -2.f;
    Require(mesh->SetAuthoredVertices(vertices), "replacement mesh geometry failed");
    auto retired = scene.TakeRetiredGraphicsResources();
    Require(original != mesh->GetGraphicsBuffer() &&
        std::any_of(retired.sharedBuffers.begin(), retired.sharedBuffers.end(),
            [original](const auto& buffer) { return buffer.get() == original; }),
        "live mesh vertex buffer was destroyed before fence retirement");

    mesh->SetSurfaceOffsets(std::vector<glm::vec3>(3, glm::vec3(0.f)), nullptr);
    auto* posed = mesh->GetPosedVertexBuffer();
    Require(posed != nullptr, "posed mesh buffer creation failed");
    mesh->ClearSurfaceOffsets();
    auto retiredPosed = scene.TakeRetiredGraphicsResources();
    Require(std::any_of(retiredPosed.buffers.begin(), retiredPosed.buffers.end(),
        [posed](const auto& buffer) { return buffer.get() == posed; }),
        "live mesh posed buffer was destroyed before fence retirement");
    renderer.WaitIdle();
}

void BenchmarkSceneSnapshot()
{
    Engine::Scene::Scene scene;
    for (int i = 0; i < 441; ++i)
        scene.AddObject("Snapshot cell " + std::to_string(i))
            ->AddComponent<Engine::Components::Material>();
    scene.CommitPendingObjectChanges();
    const double firstMs = scene.GetLastSceneSnapshotPublishMs();
    double totalMs = 0;
    for (int frame = 0; frame < 240; ++frame)
    {
        scene.CommitPendingObjectChanges();
        totalMs += scene.GetLastSceneSnapshotPublishMs();
    }
    std::cout << "Scene snapshot: 441 objects, first publication " << firstMs
              << " ms, 240 unchanged publications average " << totalMs / 240
              << " ms.\n";
}

void TestRenderBoundsCaching()
{
    using Cache=Engine::Rendering::RenderBoundsCache;
    Cache cache; std::vector<Cache::Input> input(256); std::vector<Engine::Rendering::RenderBounds> bounds;
    for(size_t i=0;i<input.size();++i) { input[i].slot=uint32_t(i); input[i].bounded=true; input[i].minimum={-1,-1,-1}; input[i].maximum={1,1,1}; }
    auto statistics=cache.Prepare(input,bounds);
    Require(statistics.updated==256 && bounds.front().minimum==glm::vec3(-1),"initial bounds preparation failed");
    statistics=cache.Prepare(input,bounds);
    Require(statistics.reused==256 && statistics.updated==0,"static bounds were rebuilt");
    input[7].world[3].x=4;
    statistics=cache.Prepare(input,bounds);
    Require(statistics.updated==1 && statistics.reused==255 && bounds[7].minimum.x==3,"moved object did not invalidate only its bounds");
    input[9].maximum.x=3; input[3].bounded=false;
    statistics=cache.Prepare(input,bounds);
    Require(statistics.updated==1 && bounds[9].maximum.x==3 && !bounds[3].bounded,"geometry or unbounded fallback was not refreshed");
    std::cout<<"World bounds: unchanged reuse, isolated transform/geometry edits and conservative fallbacks passed.\n";
}

glm::dmat4 ReferenceTransform(const Engine::Core::Object* object)
{
    if(!object) return glm::dmat4(1);
    const auto& t=object->transform;
    auto matrix=glm::translate(glm::dmat4(1),t.position);
    matrix*=glm::rotate(glm::dmat4(1),double(t.rotation.z),glm::dvec3(0,0,1));
    matrix*=glm::rotate(glm::dmat4(1),double(t.rotation.y),glm::dvec3(0,1,0));
    matrix*=glm::rotate(glm::dmat4(1),double(t.rotation.x),glm::dvec3(1,0,0));
    matrix*=glm::scale(glm::dmat4(1),glm::dvec3(t.scale));
    return ReferenceTransform(object->Parent)*matrix;
}
void CheckTransform(const Engine::Core::Object& object)
{
    const auto expected=ReferenceTransform(&object),actual=object.transform.GetWorldMatrixD();
    const auto expectedFloat=glm::mat4(expected),actualFloat=object.transform.GetWorldMatrix();
    for(int column=0;column<4;++column) for(int row=0;row<4;++row)
    {
        Require(std::abs(expected[column][row]-actual[column][row])<1.e-6,"parallel double world matrix differs from hierarchical TRS reference");
        Require(expectedFloat[column][row]==actualFloat[column][row],"parallel float matrix lost double-precision world conversion");
    }
}
void TestTransformHierarchy()
{
    using Engine::Core::Object;
    using Engine::Components::Transform;
    Engine::Components::TransformHierarchy hierarchy;
    std::vector<std::unique_ptr<Object>> objects;
    std::vector<Transform*> input;
    constexpr size_t count=2047;
    for(size_t i=0;i<count;++i)
    {
        auto object=std::make_unique<Object>();
        object->transform.position={double(i%5)*.125,double(i%3)*.25,double(i%7)*.125};
        object->transform.rotation={float(i%3)*.01f,float(i%5)*.02f,float(i%7)*.01f};
        object->transform.scale={1.01f,.99f,i%31==0 ? -1.f:1.f};
        if(i) object->Parent=objects[(i-1)/2].get();
        input.push_back(&object->transform); objects.push_back(std::move(object));
    }
    objects[0]->transform.position.x=1.e9+.125;
    std::reverse(input.begin(),input.end()); // Input order must not impose hierarchy order.
    auto result=hierarchy.Prepare(input);
    Require(result.valid && result.localUpdated==count && result.worldUpdated==count && result.topologyRebuilt,"initial hierarchy preparation failed");
    for(const auto& object:objects)
    {
        CheckTransform(*object);
        Require(hierarchy.ReadPreparedWorldMatrix(object->transform)==object->transform.GetWorldMatrix(),"prepared render matrix differs from lazy validation");
    }
    Object outside; outside.transform.position={1,2,3};
    Require(hierarchy.ReadPreparedWorldMatrix(outside.transform)==outside.transform.GetWorldMatrix(),"non-batched read did not retain lazy validation");
    const auto revision=objects.back()->transform.GetWorldRevision();
    const auto before=Engine::Threading::JobScheduler::Get().ReadStatistics();
    result=hierarchy.Prepare(input);
    const auto after=Engine::Threading::JobScheduler::Get().ReadStatistics();
    Require(result.valid && result.worldReused==count && !result.localUpdated && !result.worldUpdated && !result.topologyRebuilt,
        "unchanged hierarchy did not retain its caches");
    Require(before.jobs==after.jobs && objects.back()->transform.GetWorldRevision()==revision,"static hierarchy scheduled jobs or advanced revisions");
    objects.back()->transform.position.y+=.25;
    result=hierarchy.Prepare(input);
    Require(result.localUpdated==1 && result.worldUpdated==1,"leaf edit rebuilt unrelated branches"); CheckTransform(*objects.back());
    objects[0]->transform.position.z+=.5;
    result=hierarchy.Prepare(input);
    Require(result.localUpdated==1 && result.worldUpdated==count,"parent edit failed to propagate world caches");
    for(const auto& object:objects) CheckTransform(*object);
    objects.back()->transform.MarkDirty(); result=hierarchy.Prepare(input);
    Require(result.localUpdated==1 && result.worldUpdated==1,"explicit dirty notification was lost");
    objects.back()->Parent=objects[2].get(); result=hierarchy.Prepare(input);
    Require(result.topologyRebuilt && result.worldUpdated==1,"direct reparenting was not detected"); CheckTransform(*objects.back());
    Object external; external.transform.position={32.125,-7,3}; objects[0]->Parent=&external;
    result=hierarchy.Prepare(input); Require(result.worldUpdated==count,"external parent was not captured"); CheckTransform(*objects.back());
    external.transform.position.x+=.125; result=hierarchy.Prepare(input);
    Require(!result.topologyRebuilt && result.worldUpdated==count,"external parent edit did not invalidate descendants"); CheckTransform(*objects.back());
    // Invalid topology must be rejected before publishing any copied local edits.
    auto* oldParent=objects.back()->Parent; objects.back()->Parent=objects.back().get();
    objects.back()->transform.position.z+=.125; result=hierarchy.Prepare(input);
    Require(!result.valid,"cyclic hierarchy was accepted");
    objects.back()->Parent=oldParent; result=hierarchy.Prepare(input);
    Require(result.valid && result.localUpdated==1,"cycle recovery lost pending edits"); CheckTransform(*objects.back());
    input.push_back(input.front()); Require(!hierarchy.Prepare(input).valid,"duplicate transform was accepted"); input.pop_back();
    input.erase(input.begin()); objects.pop_back(); Require(hierarchy.Prepare(input).valid,"deleted/reordered hierarchy retained stale topology");
    auto& scheduler=Engine::Threading::JobScheduler::Get(); scheduler.SetParallelForEnabled(false);
    objects[0]->transform.rotation.y+=.1f; Require(hierarchy.Prepare(input).valid,"serial hierarchy preparation failed");
    for(const auto& object:objects) CheckTransform(*object);
    scheduler.SetParallelForEnabled(true); objects[0]->transform.rotation.y+=.1f;
    Require(hierarchy.Prepare(input).valid,"parallel hierarchy preparation failed");
    for(const auto& object:objects) CheckTransform(*object);
    // Layer/portal mapping remains a separate operation on the authored world matrix.
    objects[0]->transform.matrixLayer.SetLocalToLayer(glm::translate(glm::mat4(1),glm::vec3(3,4,5)));
    Require(objects[0]->transform.GetWorldMatrixWithLayer()==objects[0]->transform.matrixLayer.localToLayer*objects[0]->transform.GetWorldMatrix(),
        "hierarchy preparation changed matrix-layer mapping");
    for(size_t frame=0;frame<32;++frame)
    {
        auto streamed=std::make_unique<Object>(); streamed->Parent=objects[3+frame%8].get();
        streamed->transform.position={double(frame)*.25,2,3}; input.push_back(&streamed->transform);
        result=hierarchy.Prepare(input);
        Require(result.valid && result.localUpdated==1 && result.worldUpdated==1,"streaming a leaf rebuilt unrelated transform matrices");
        CheckTransform(*streamed);
        Require(hierarchy.ReadPreparedWorldMatrix(streamed->transform)==streamed->transform.GetWorldMatrix(),"streamed transform did not publish its prepared matrix");
        input.pop_back(); streamed.reset(); result=hierarchy.Prepare(input);
        Require(result.valid && result.worldUpdated==0,"removing a streamed leaf invalidated surviving matrices");
    }
    std::cout<<"Transforms: hierarchical float/double TRS, static reuse, direct edits, reparent/delete, external parents, cycle rejection, serial/parallel and layers passed.\n";
}

void TestSceneTransformScheduling()
{
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Scene transform preparation regression",64,64);
    Engine::Renderers::DX11GameRenderer renderer;
    Require(renderer.Init(window.GetHWND(),64,64),"scene transform renderer failed");
    Engine::Scene::Scene scene; scene.Init(renderer.GetGraphicsProvider()); scene.SetUpdateProfilingEnabled(true);
    auto* root=scene.AddObject("Transform root"); auto* child=scene.AddObject("Transform child");
    child->Parent=root; child->transform.position={1,2,3};
    auto* childMesh=child->AddComponent<Engine::Components::Mesh>();
    std::vector<Engine::Components::Mesh::Vertex> triangle(3);
    triangle[0].pos[0]=-1; triangle[1].pos[0]=1; triangle[2].pos[1]=1;
    Require(childMesh->SetAuthoredVertices(triangle),"spatial slot triangle creation failed");
    childMesh->CreateBuffer(renderer.GetGraphicsProvider()->GetBufferFactory());
    scene.PrepareRenderFrame();
    Require(scene.GetLastPrepareTimings().transformStructureFallback==1,"structural frame rebuilt the transform hierarchy");
    scene.PrepareRenderFrame();
    Require(scene.GetLastPrepareTimings().transformStructureFallback==0,"stable scene did not resume prepared transforms");
    Require(scene.GetLastPrepareTimings().staticRenderItemsReused==1,
        "unchanged static mesh render item was rebuilt");
    root->transform.position.x=10; scene.PrepareRenderFrame();
    Require(scene.GetLastPrepareTimings().transformWorldUpdated==2,"scene transform phase missed a parent's descendants"); CheckTransform(*child);
    Require(scene.GetLastPrepareTimings().staticRenderItemsUpdated==1 &&
        scene.GetLastPrepareTimings().staticRenderItemsReused==0,
        "parent transform edit did not refresh the child render item");
    scene.PrepareRenderFrame();
    Require(scene.GetLastPrepareTimings().staticRenderItemsReused==1,
        "static render item was not reused after its transform update");
    triangle[0].pos[0]=-2;
    Require(childMesh->SetAuthoredVertices(triangle),"static mesh geometry edit failed");
    scene.PrepareRenderFrame();
    Require(scene.GetLastPrepareTimings().staticRenderItemsUpdated==1 &&
        scene.GetLastPrepareTimings().staticRenderItemsReused==0,
        "geometry edit did not refresh the static render item");
    scene.PrepareRenderFrame();
    Require(scene.GetLastPrepareTimings().staticRenderItemsReused==1,
        "static render item was not reused after its geometry update");
    child->transform.matrixLayer.SetLocalToLayer(
        glm::translate(glm::mat4(1.f),glm::vec3(0.f,3.f,0.f)));
    scene.PrepareRenderFrame();
    Require(scene.GetLastPrepareTimings().staticRenderItemsUpdated==1 &&
        scene.GetLastPrepareTimings().staticRenderItemsReused==0,
        "matrix layer edit did not refresh the static render item");
    for(int frame=0;frame<16;++frame)
    {
        auto* streamed=scene.AddObject("Streamed transform"); streamed->Parent=root; streamed->transform.position.z=frame;
        scene.PrepareRenderFrame();
        Require(scene.GetLastPrepareTimings().transformStructureFallback==1 && scene.GetLastPrepareTimings().transformLocalUpdated==0,
            "streaming forced a whole-scene transform rebuild"); CheckTransform(*streamed);
        Require(scene.GetLastPrepareTimings().staticRenderItemsReused==1,
            "unrelated object insertion invalidated a static render item");
        scene.RemoveObject(streamed); scene.PrepareRenderFrame();
        Require(scene.GetLastPrepareTimings().transformStructureFallback==1,"streaming removal was not handled on its owner");
    }
    scene.PrepareRenderFrame();
    Require(scene.GetLastPrepareTimings().transformStructureFallback==0 && scene.GetLastPrepareTimings().transformWorldReused>=2,
        "scene did not restore static transform reuse after streaming stopped");
    child->enabled=false; scene.PrepareRenderFrame();
    Require(scene.GetLastPrepareTimings().staticRenderItemsReused==0,
        "hidden mesh retained a static render item");
    Require(scene.GetLastPrepareTimings().spatialRebuilds==0,
        "temporarily hidden draw rebuilt the spatial hierarchy");
    child->enabled=true; scene.PrepareRenderFrame();
    Require(scene.GetLastPrepareTimings().staticRenderItemsUpdated==1,
        "restored mesh did not rebuild its render item");
    Require(scene.GetLastPrepareTimings().spatialRebuilds==0,
        "restored draw rebuilt the spatial hierarchy");
    std::cout<<"Scene transforms: owner fallback during streaming, parallel-ready stable frames, descendant updates and cache recovery passed.\n";
}

void BenchmarkTransformHierarchy()
{
    using Clock=std::chrono::steady_clock;
    using Engine::Core::Object;
    std::ofstream report(".local/performance-logs/parallel-stage4-transforms.jsonl");
    for(bool tree:{false,true}) for(bool moving:{false,true})
    {
        std::vector<std::unique_ptr<Object>> objects;
        std::vector<Engine::Components::Transform*> input;
        for(size_t i=0;i<4095;++i)
        {
            auto object=std::make_unique<Object>();
            if(tree && i) object->Parent=objects[(i-1)/2].get();
            input.push_back(&object->transform); objects.push_back(std::move(object));
        }
        Engine::Components::TransformHierarchy hierarchy;
        for(unsigned mode=0;mode<3;++mode)
        {
            auto& jobs=Engine::Threading::JobScheduler::Get(); jobs.SetParallelForEnabled(mode==2);
            double total=0,capture=0,compute=0,commit=0; unsigned measured=0;
            for(unsigned frame=0;frame<68;++frame)
            {
                for(size_t i=0;i<objects.size();++i)
                {
                    auto& transform=objects[i]->transform;
                    transform.position={double(i%7)*.125,moving ? double(frame)*.002:0.,double(i%5)*.25};
                    transform.rotation={.01f,float(i%5)*.01f,moving ? float(frame)*.001f:0.f};
                }
                const auto start=Clock::now();
                Engine::Components::TransformHierarchy::Statistics stats;
                if(mode==0) for(const auto& object:objects) (void)object->transform.GetWorldMatrix();
                else
                {
                    stats=hierarchy.Prepare(input);
                    for(const auto& object:objects) (void)hierarchy.ReadPreparedWorldMatrix(object->transform);
                }
                const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
                if(frame>=8) { total+=elapsed; capture+=stats.snapshotMs; compute+=stats.computeMs; commit+=stats.commitMs; ++measured; }
            }
            CheckTransform(*objects.back());
            const char* name=mode==0 ? "lazy-owner":mode==1 ? "batch-serial":"batch-parallel";
            report<<"{\"topology\":\""<<(tree ? "tree":"roots")<<"\",\"moving\":"<<(moving ? "true":"false")
                <<",\"mode\":\""<<name<<"\",\"objects\":4095,\"frames\":"<<measured
                <<",\"total_ms\":"<<total/measured<<",\"capture_ms\":"<<capture/measured<<",\"compute_ms\":"<<compute/measured
                <<",\"commit_ms\":"<<commit/measured<<"}\n";
            std::cout<<(tree ? "tree":"roots")<<" "<<(moving ? "moving":"static")<<" "<<name<<": "<<total/measured<<" ms\n";
        }
    }
    Engine::Threading::JobScheduler::Get().SetParallelForEnabled(true);
}

void TestDirectorySnapshots()
{
    namespace fs=std::filesystem;
    const auto root=fs::current_path()/".local"/("directory-cache-test-"+std::to_string(GetCurrentProcessId()));
    fs::create_directories(root/"Folder");
    std::ofstream(root/"z.mesh")<<"test"; std::ofstream(root/"a.rig")<<"test";
    std::ofstream(root/"a.meshpack")<<"test"; std::ofstream(root/"hidden.meta")<<"test";
    Engine::IO::DirectorySnapshotCache cache; cache.Watch(root);
    const auto read=[&](const std::string& query,size_t expected) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        std::shared_ptr<const Engine::IO::DirectorySnapshot> snapshot;
        do { cache.Poll(); snapshot=cache.Get(root,query); if(snapshot && snapshot->entries.size()==expected) return snapshot;
             std::this_thread::sleep_for(std::chrono::milliseconds(1)); } while(std::chrono::steady_clock::now()<deadline);
        throw std::runtime_error("directory snapshot did not refresh");
    };
    auto snapshot=read("",4);
    Require(snapshot->entries.front().is_directory(),"directory sorting changed");
    auto rig=read("a.rig",1); Require(rig->entries.front().meshPacked,"directory metadata was not cached");
    std::ofstream(root/"new.mesh")<<"new"; snapshot=read("",5);
    fs::remove(root/"new.mesh"); snapshot=read("",4);
    fs::remove(root/"z.mesh"); fs::remove(root/"a.rig"); fs::remove(root/"a.meshpack"); fs::remove(root/"hidden.meta");
    fs::remove(root/"Folder"); fs::remove(root);
    std::cout<<"Directory snapshots: filtering, metadata, sorting and external create/delete refresh passed.\n";
}

void TestSpatialScheduling()
{
    struct Scheduled final : Engine::Core::Component,Engine::Components::SpatialUpdate
    {
        Engine::Core::Object* viewer=nullptr;
        uint64_t revision=1; int ticks=0,leaves=0;
        bool RequiresUpdate() const override { return false; }
        void Update() override { throw std::runtime_error("scheduled script received an ordinary update"); }
        uint64_t SpatialRegionRevision() const override { return revision; }
        Engine::Components::SpatialUpdateRegion DescribeSpatialRegion() override
        { const auto p=Owner->transform.GetWorldPosition(); return {Owner,viewer,{p-glm::vec3(2),p+glm::vec3(2),true}}; }
        bool TickSpatialRegion(bool) override { ++ticks; return true; }
        void LeaveSpatialRegion() override { ++leaves; }
    };
    Engine::Scene::Scene scene;
    auto* viewer=scene.AddObject("Viewer");
    auto* first=scene.AddObject("Near")->AddComponent<Scheduled>(); first->viewer=viewer;
    auto* farObject=scene.AddObject("Far"); farObject->transform.position.x=100;
    auto* second=farObject->AddComponent<Scheduled>(); second->viewer=viewer;
    scene.UpdateSpatialScripts();
    Require(first->ticks==1 && second->ticks==0,"scheduler updated a dormant field");
    viewer->transform.position.x=100; scene.UpdateSpatialScripts();
    Require(first->leaves==1 && second->ticks==1,"scheduler missed a viewer boundary crossing");
    farObject->enabled=false; scene.UpdateSpatialScripts();
    Require(second->leaves==1,"disabled scheduled field was not retired");
    farObject->enabled=true; farObject->transform.position.x=200; ++second->revision;
    scene.UpdateSpatialScripts(); Require(second->ticks==1,"changed spatial bounds were not invalidated");
    viewer->transform.position.x=200; scene.UpdateSpatialScripts(); Require(second->ticks==2,"changed spatial bounds were not queried");
    scene.RequestRemoveObject(farObject); scene.CommitPendingObjectChanges(); scene.UpdateSpatialScripts();
    Require(scene.GetSpatialUpdateScheduler().activeCount==0,"deleted scheduled field remained active");
}

void TestLandscapePreparation()
{
    Engine::Scene::Scene scene;
    Engine::Audio::Audio audio(scene);
    audio.Update(0); audio.Reset(); audio.SynchronizeToTime(0);
    const glm::vec3 point(1,2,3);
    Require(scene.SampleSpatialPoint(point).point==point,"empty volume registry changed space");
    auto* parent=scene.AddObject("Registry parent");
    auto* child=scene.AddObject("Nested registry components");
    child->Parent=parent; parent->Children.push_back(child);
    auto* volume=child->AddComponent<Engine::Components::SpatialManipulator>();
    volume->definesWarpVolume=true; volume->position={2,0,0};
    const auto expected=volume->MapWorldPointThroughVolume(point);
    Require(glm::length(expected-point)>1.f,"warp fixture did not move the test point");
    Require(glm::length(scene.SampleSpatialPoint(point).point-expected)<1.e-4f,
        "cached spatial volumes repeated a nested volume");
    auto* source=child->AddComponent<Engine::Components::AudioSource>();
    source->playOnStart=false;
    audio.Update(0); audio.Reset(); audio.SynchronizeToTime(0);
    child->AddComponent<Engine::Components::Light>();
    Engine::Rendering::RealtimeLightingPipeline lights;
    Engine::Model::LightData lightData[4]{};
    Require(lights.CollectLights(scene,lightData,4,false)==1,"cached light registry missed an addition");
    parent->enabled=false;
    Require(scene.SampleSpatialPoint(point).point==point,"disabled ancestor retained a warp volume");
    Require(lights.CollectLights(scene,lightData,4,false)==0,"disabled ancestor retained a light");
    audio.Update(0);
    scene.RequestRemoveObject(parent); scene.CommitPendingObjectChanges();
    Require(lights.CollectLights(scene,lightData,4,false)==0,"deleted light remained cached");
    Require(scene.SampleSpatialPoint(point).point==point,"deleted volume remained cached");
    audio.Update(0); audio.Reset(); audio.SynchronizeToTime(0);
    std::vector<std::future<unsigned>> queued;
    for(unsigned i=0;i<3;++i) queued.push_back(Engine::Rendering::RenderJobs::Get().Submit([i] { return i+7; }));
    unsigned returned=0;
    for(auto& job:queued) returned+=job.get();
    Require(returned==24,"asynchronous snapshot preparation lost work");
    std::vector<std::atomic<unsigned>> visits(2048);
    for(int repeat=0;repeat<3;++repeat)
        Engine::Rendering::RenderJobs::Get().For(visits.size(),512,[&](size_t i) { ++visits[i]; });
    for(const auto& visit:visits) Require(visit==3,"render jobs skipped or repeated work");
    std::array<glm::vec3,8> corners{};
    for(unsigned i=0;i<8;++i) corners[i]={float(i&1 ? 1:-1),float(i&2 ? 1:-1),float(i&4 ? 10:2)};
    Engine::Rendering::ShadowReceiverCulling receiver;
    receiver.Build(corners,glm::mat4(1),.01f);
    Require(!receiver.Outside({-.1f,-.1f,-100.f},{.1f,.1f,-99.f},glm::mat4(1)),
        "off-screen caster along a light ray was rejected");
    Require(receiver.Outside({2.f,2.f,3.f},{3.f,3.f,4.f},glm::mat4(1)),
        "caster outside the receiver silhouette was retained");
    Engine::Model::RealtimeShadowSettings quality;
    Engine::Model::ApplyShadowQualityPreset(quality,Engine::Model::ShadowQualityPreset::Low);
    Require(quality.directionalCascadeCount==1 && quality.grassCasterDistance==6.f,"low landscape shadow budget");
    Engine::Model::ApplyShadowQualityPreset(quality,Engine::Model::ShadowQualityPreset::High);
    Require(quality.directionalCascadeCount==4 && quality.grassCasterDistance==16.f,"high landscape shadow budget");
}

void TestGrass()
{
    Engine::Scene::Scene scene;
    auto* terrainObject = scene.AddObject("Terrain");
    auto* terrain = terrainObject->AddComponent<Engine::Components::Terrain>();
    terrain->cellsX = terrain->cellsY = 16;
    terrain->cellWidth = terrain->cellDepth = 1;
    auto* viewer = scene.AddObject("Viewer");
    viewer->transform.position = {2,0,2};
    auto* owner = scene.AddObject("Grass");
    owner->Parent = terrainObject;
    terrainObject->Children.push_back(owner);
    auto* layer = owner->AddComponent<Engine::Components::TerrainObjectLayer>();
    for (int i = 0; i < 40; ++i)
    {
        Engine::Components::TerrainObjectLayer::Placement placement;
        placement.asset = "Assets/Mesh/Grass/meadow_short.obj";
        placement.position = {1.f + (i % 2) * 8.f, 0, 1.f + (i % 5) * .3f};
        layer->placements.push_back(placement);
    }
    layer->MarkConfigurationDirty();
    auto* preview = scene.AddObject("Grass Preview");
    preview->Parent = owner;
    owner->Children.push_back(preview);
    auto* legacy = scene.AddObject("Grass Tile (99, 99)");
    legacy->Parent = owner;
    owner->Children.push_back(legacy);
    auto* grass = owner->AddComponent<GrassField>();
    grass->gpuLodSelection=false;
    grass->viewerObjectName = "Viewer";
    grass->tileWorkBudgetMs = .1f;
    grass->fadeDuration = 0.f;
    grass->Start();
    uint64_t builds = 0, scanned = 0;
    int frames = 0;
    for (; frames < 3000; ++frames)
    {
        scene.ResetGrassPerformance();
        grass->Update();
        scene.CommitPendingObjectChanges();
        const auto& p = grass->GetLastPerformance();
        Require(p.tilesBuilt <= 1, "multiple uploads in one grass refresh");
        builds += p.tilesBuilt;
        scanned += p.placementsScanned;
        if (p.residentLods == 6 && p.tilesBuilt == 0) break;
    }
    Require(frames < 3000, "incremental generation failed to complete");

    Require(!scene.FindObjectByName("Grass Preview") && !scene.FindObjectByName("Grass Tile (99, 99)"),
        "legacy preview objects were not retired");
    Require(builds == 2, "two tiles should prepare their shared placement data once each");
    Require(scanned == 40, "placements must be indexed once");
    uint64_t switches = 0;
    for (int frame = 0; frame < 100; ++frame)
    {
        scene.ResetGrassPerformance();
        viewer->transform.position.x = frame % 2 ? 2.f : 13.f;
        grass->Update();
        scene.CommitPendingObjectChanges();
        const auto& p = grass->GetLastPerformance();
        switches += p.lodSwitches;
        Require(p.tilesBuilt == 0 && p.uploadMs == 0 && p.generationMs == 0,
            "LOD movement rebuilt or uploaded resident geometry");
        Require(p.placementsScanned == 0, "movement rescanned placements");
    }
    Require(switches >= 90, "movement test did not exercise LOD transitions");
    auto* tile = scene.FindObjectByName("Grass Tile (0, 0)");
    Require(tile && tile->runtimeOnly, "generated tile must be runtime-only");
    auto* instancedMesh = tile->GetComponent<Engine::Components::Mesh>();
    Require(instancedMesh->IsInstanced() && instancedMesh->GetVertices().empty(),
        "grass expanded source vertices on the CPU");
    Require(instancedMesh->GetInstanceData()->transforms.size() == 20,
        "grass lost placement instances");
    const auto placements = instancedMesh->GetInstanceData();
    for (int lod = 0; lod < 3; ++lod)
    {
        instancedMesh->SelectRuntimeLod(lod);
        Require(instancedMesh->GetInstanceData() == placements,
            "grass LOD duplicated placement data");
    }
    viewer->transform.position.x = 30.f;
    scene.ResetGrassPerformance(); grass->Update(); scene.CommitPendingObjectChanges();
    Require(grass->GetLastPerformance().residentLods == 6,
        "nearby invisible grass was evicted instead of retained");
    viewer->transform.position.x = 2.f;
    scene.ResetGrassPerformance(); grass->Update(); scene.CommitPendingObjectChanges();
    Require(grass->GetLastPerformance().tilesBuilt == 0 && instancedMesh->GetInstanceData() == placements,
        "returning through the retention band rebuilt grass placements");
    const auto snapshot = scene.SaveToString();
    Require(snapshot.find("Grass Tile (") == std::string::npos,
        "generated geometry leaked into authored scene snapshot");
    layer->placements.front().scale = 2.f;
    layer->MarkConfigurationDirty();
    scene.ResetGrassPerformance();
    Engine::Rendering::GrassPerformance exhaustedBudget{};
    exhaustedBudget.generationMs = 100.;
    scene.AddGrassPerformance(exhaustedBudget);
    grass->Update();
    Require(grass->GetLastPerformance().placementsScanned == 40,
        "placement edits did not invalidate tile index");
    Require(grass->GetLastPerformance().tilesBuilt == 0 && grass->GetLastPerformance().pending > 0,
        "a grass field ignored the preparation budget already spent elsewhere in the scene");
    grass->useEditorCamera = true;
    scene.editorCamera.transform.position = {200,0,200};
    grass->Update();
    Require(grass->GetLastPerformance().residentLods == 0,
        "explicit editor viewer did not unload distant grass");
    std::cout << "Grass: " << builds << " placement uploads (6 shared-source LODs) in " << frames
        << " budgeted frames; 100 moving frames with zero rebuilds/uploads/scans.\n";
    grass->useEditorCamera=false; layer->placements.clear(); layer->MarkConfigurationDirty();
    scene.ResetGrassPerformance(); grass->Update(); scene.CommitPendingObjectChanges();
    Require(grass->GetLastPerformance().residentLods==0 && grass->GetLastPerformance().pending==0,
        "empty authored layer generated procedural grass");
    grass->allowProceduralGeneration=true;
    scene.ResetGrassPerformance(); grass->Update(); scene.CommitPendingObjectChanges();
    Require(grass->GetLastPerformance().pending>0 || grass->GetLastPerformance().tilesBuilt>0,
        "explicit procedural opt-in failed");
}

void TestEditorFrameLifecycle()
{
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Editor frame lifecycle regression",64,64);
    Engine::Renderers::DX11EditorRenderer renderer;
    Require(renderer.Init(window.GetHWND(),64,64),"editor renderer init failed");
    auto* factory=renderer.GetGraphicsProvider()->GetContextFactory();
    factory->SetNativeCallTimingEnabled(true);
    for(uint64_t frame=1;frame<=6;++frame)
    {
        renderer.SetGpuTimingEnabled(frame==3 || frame==4);
        renderer.MarkDirty();
        renderer.RenderIfNeeded([&] {
            auto context=factory->CreateContext(); uint32_t value=uint32_t(frame);
            context->SetNativeTimingPhase(Engine::Graphics::NativePhase::Camera);
            context->SetComputeConstants(&value,sizeof(value));
        });
        const auto timings=factory->GetNativeSubmissionTimings();
        Require(factory->GetPreparedFrameSerial()==frame && timings.frameSerial==frame,
            "editor frame recycling depends on GPU diagnostics");
        Require(timings.computeArenaWrites+timings.computeDiscardMaps==1,"frame upload counters were not reset");
        Require(timings.phases[size_t(Engine::Graphics::NativePhase::Camera)][size_t(Engine::Graphics::NativeOperation::Map)].calls==1,
            "native counters accumulated across editor frames");
        if(frame!=3 && frame!=4) Require(!factory->GetFrameTimingTelemetry().gpuTimingsValid,"disabled GPU timing remained valid");
    }
    factory->SetNativeCallTimingEnabled(false); renderer.MarkDirty(); renderer.RenderIfNeeded();
    Require(factory->GetPreparedFrameSerial()==7 && !factory->GetNativeSubmissionTimings().enabled,"diagnostic disable stopped recycling");
    std::cout<<"Editor: frame serial and resources advance with GPU diagnostics off and across toggles.\n";
}

void TestDx11ConstantsAndGeometry()
{
    using namespace Engine::Renderers;
    using Buffer = Engine::Graphics::IGraphicsBuffer;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate;
    D3D_FEATURE_LEVEL level{};
    Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &immediate)), "WARP device failed");
    D3D11BufferFactory factory(device.Get());
    auto resources = std::make_shared<D3D11FrameResourceState>(device.Get(), immediate.Get());
    D3D11GraphicsContext context(device.Get(), immediate.Get(), {}, resources);
    std::vector<uint32_t> values(256 * 8 / sizeof(uint32_t));
    for (size_t i = 0; i < values.size(); ++i) values[i] = static_cast<uint32_t>(i);
    auto constants = factory.CreateBuffer(Buffer::Usage::ConstantBuffer,
        Buffer::AccessMode::Upload, values.size() * sizeof(uint32_t), values.data());
    resources->PrepareFrame();
    context.PrepareConstantBuffer(constants.get(), constants->GetSize());
    for (unsigned i = 0; i < 8; ++i) context.SetConstantBuffer(0, constants.get(), i * 256);
    if (resources->arenaSupported)
    {
        Require(resources->arenaWrites == 1 && resources->discardMaps == 1,
            "shadow constants should upload once per pass");
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
        immediate.As(&context1);
        Microsoft::WRL::ComPtr<ID3D11Buffer> bound;
        UINT first = 0, count = 0;
        context1->VSGetConstantBuffers1(0, 1, &bound, &first, &count);
        Require(bound.Get() == resources->constantArena.Get() && first == 7 * 16 && count == 16,
            "incorrect constant-buffer record range");
        D3D11_BUFFER_DESC descriptor{};
        bound->GetDesc(&descriptor);
        descriptor.Usage = D3D11_USAGE_STAGING;
        descriptor.BindFlags = 0;
        descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Buffer> readback;
        Require(SUCCEEDED(device->CreateBuffer(&descriptor, nullptr, &readback)), "readback failed");
        immediate->CopyResource(readback.Get(), bound.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Require(SUCCEEDED(immediate->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "readback map failed");
        Require(std::memcmp(mapped.pData, values.data(), constants->GetSize()) == 0,
            "batched constant upload corrupted data");
        immediate->Unmap(readback.Get(), 0);
    }
    resources->native->timings.enabled=true;
    context.SetNativeTimingPhase(Engine::Graphics::NativePhase::Camera);
    if(resources->arenaSupported)
    {
        resources->PrepareFrame();
        const uint32_t a=0x12345678,b=0x87654321;
        context.SetComputeConstants(&a,sizeof(a));
        context.SetComputeConstants(&b,sizeof(b));
        Require(resources->computeArenaWrites==2 && resources->discardMaps==1 && resources->computeDiscardMaps==0,
            "compute constants did not append without discard");
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1; immediate.As(&context1);
        Microsoft::WRL::ComPtr<ID3D11Buffer> bound; UINT offset=0,count=0;
        context1->CSGetConstantBuffers1(0,1,&bound,&offset,&count);
        Require(bound.Get()==resources->constantArena.Get() && offset==16 && count==16,"compute constant range is wrong");
        D3D11_BUFFER_DESC desc{}; bound->GetDesc(&desc); desc.Usage=D3D11_USAGE_STAGING;
        desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Buffer> readback;
        Require(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&readback)),"compute staging failed");
        immediate->CopyResource(readback.Get(),bound.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
        Require(SUCCEEDED(immediate->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)),"compute readback failed");
        Require(*static_cast<uint32_t*>(mapped.pData)==a &&
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapped.pData)+256)==b,
            "compute constants overwrote previous dispatch data");
        immediate->Unmap(readback.Get(),0);
        // A different graphics context can wrap the shared arena. Cached graphics
        // snapshots must observe that rename before reusing a range.
        context.PrepareConstantBuffer(constants.get(),constants->GetSize());
        D3D11GraphicsContext other(device.Get(),immediate.Get(),{},resources);
        resources->arenaCursor=resources->ArenaSize;
        other.SetComputeConstants(&b,sizeof(b));
        const auto writes=resources->arenaWrites;
        context.SetConstantBuffer(0,constants.get(),256);
        Require(resources->arenaWrites==writes+1,"stale graphics snapshot survived another context's discard");
        resources->computeArenaEnabled=false;
    }
    uint32_t fallbackValue=7; context.SetComputeConstants(&fallbackValue,sizeof(fallbackValue));
    Require(resources->computeDiscardMaps==1,"compute fallback did not record its discard map");
    const auto& camera=resources->native->timings.phases[size_t(Engine::Graphics::NativePhase::Camera)];
    Require(camera[size_t(Engine::Graphics::NativeOperation::Map)].calls>0 &&
        camera[size_t(Engine::Graphics::NativeOperation::Bind)].calls>0,"native timings missed compute upload or binding");
    resources->native->Reset(42);
    Require(resources->native->timings.frameSerial==42 && resources->native->timings.enabled &&
        resources->native->timings.phases[size_t(Engine::Graphics::NativePhase::Camera)][0].calls==0,
        "native frame timing reset retained old samples");
    resources->native->timings.enabled=false;
    resources->arenaSupported = false;
    context.PrepareConstantBuffer(constants.get(), constants->GetSize());
    context.SetConstantBuffer(0, constants.get(), 256);
    Require(resources->fallbackConstantBuffers[0] != nullptr, "DX11 fallback binding failed");

    Engine::Components::Mesh first, second;
    std::vector<Engine::Components::Mesh::Vertex> triangle(3);
    triangle[1].pos[0] = 1;
    triangle[2].pos[2] = 1;
    Require(first.SetPreparedTriangleGeometry(triangle, {0,0,0}, {1,0,1}), "triangle failed");
    first.CreateBuffer(&factory);
    auto* resident = first.GetGraphicsBuffer();
    Require(second.ExchangeStaticGeometry(first), "geometry exchange failed");
    Require(second.GetGraphicsBuffer() == resident && first.GetVertexCount() == 0,
        "geometry exchange did not preserve resident GPU buffer");
    Require(first.ExchangeStaticGeometry(second) && first.GetGraphicsBuffer() == resident,
        "returning to resident LOD did not preserve GPU buffer");
    // Each Mesh owns its LOD buffers and hysteresis state. Selecting one object
    // must neither upload nor affect another object's current level.
    auto alternative = std::make_unique<Engine::Components::Mesh>();
    triangle[1].pos[0] = 2;
    Require(alternative->SetPreparedTriangleGeometry(triangle, {0,0,0}, {2,0,1}), "LOD geometry failed");
    alternative->CreateBuffer(&factory);
    auto* distant = alternative->GetGraphicsBuffer();
    Require(first.StoreRuntimeLod(1, std::move(alternative)), "LOD install failed");
    Require(first.GetResidentRuntimeLodCount() == 2 && first.GetActiveRuntimeLod() == 0,
        "installing LOD lost the object's base geometry");
    Require(first.ChooseRuntimeLod(8.5f, {8,13}, 1) == 0,
        "outward LOD hysteresis failed");
    Require(first.SelectRuntimeLod(1) && first.GetGraphicsBuffer() == distant,
        "LOD selection did not select resident GPU buffer");
    Require(first.ChooseRuntimeLod(7.5f, {8,13}, 1) == 1 &&
        first.ChooseRuntimeLod(6.9f, {8,13}, 1) == 0,
        "inward LOD hysteresis failed");
    Require(first.ChooseRuntimeLod(13.5f, {8,13}, 1) == 1 &&
        first.ChooseRuntimeLod(14.1f, {8,13}, 1) == 2,
        "far LOD hysteresis failed");
    Require(first.BeginRuntimeLodTransition(0, .4f), "LOD fade did not start");
    Require(first.GetPreviousRuntimeLodMesh() &&
        first.GetPreviousRuntimeLodMesh()->GetGraphicsBuffer() == distant,
        "outgoing LOD buffer was not retained for fading");
    first.AdvanceRuntimeLodTransition(.1f);
    const auto incomingFade = first.GetRuntimeGeometryFade();
    const auto outgoingFade = first.GetRuntimeGeometryFade(true);
    Require(std::abs(incomingFade.y-.25f) < 1.e-5f &&
        std::abs(outgoingFade.x-.25f) < 1.e-5f && outgoingFade.y == 1.f,
        "LOD fade coverage is not complementary");
    Require(!first.BeginRuntimeLodTransition(1, .4f), "LOD fade pair was interrupted");
    first.AdvanceRuntimeLodTransition(.3f);
    Require(!first.GetPreviousRuntimeLodMesh() && first.GetGraphicsBuffer() == resident,
        "finished fade retained the outgoing draw or changed the GPU buffer");
    first.SetRuntimeVisibility(.5f);
    Require(first.GetRuntimeGeometryFade().y == .5f, "appearance fade coverage failed");
    first.SetRuntimeVisibility(1.f);
    first.SelectRuntimeLod(1);
    const auto revision = first.GetAuthoredGeometryRevision();
    Require(!first.SelectRuntimeLod(2) && first.GetGraphicsBuffer() == distant &&
        first.GetAuthoredGeometryRevision() == revision,
        "missing LOD changed the displayed mesh");
    Require(!first.StoreRuntimeLod(-1, std::make_unique<Engine::Components::Mesh>()),
        "negative LOD index accepted");
    for (int i = 0; i < 100; ++i)
    {
        const int levelIndex = i % 2;
        first.SelectRuntimeLod(levelIndex);
        Require(first.GetGraphicsBuffer() == (levelIndex ? distant : resident),
            "resident LOD switching recreated GPU geometry");
        Require(second.GetActiveRuntimeLod() == -1,
            "LOD state leaked to another object");
    }
    first.SelectRuntimeLod(0);
    Require(first.GetRuntimeLodGpuMemoryBytes() == resident->GetSize() + distant->GetSize(),
        "LOD memory accounting lost inactive geometry");
    Require(first.StoreRuntimeLod(2, std::make_unique<Engine::Components::Mesh>()), "empty LOD install failed");
    Require(first.SelectRuntimeLod(2) && first.ChooseRuntimeLod(12.5f, {8,13}, 1) == 2 &&
        first.ChooseRuntimeLod(11.9f, {8,13}, 1) == 1, "last LOD hysteresis failed");
    first.SelectRuntimeLod(0);
    Require(first.SetAuthoredVertices(triangle) && first.GetResidentRuntimeLodCount() == 0 &&
        first.GetActiveRuntimeLod() == -1, "authored geometry edit did not invalidate cached LODs");
    resident = first.GetGraphicsBuffer();
    first.ClearRuntimeLods();
    Require(first.GetResidentRuntimeLodCount() == 0 && first.GetGraphicsBuffer() == resident,
        "clearing LOD alternatives discarded displayed geometry");
    std::cout << "DX11: batched constant data and ranges, fallback, and resident geometry exchange passed.\n";
}

void CheckStressScene()
{
    Engine::Scene::Scene scene;
    Require(Engine::Serialization::SceneSerializer::Load(scene,
        "Engine/Core/Assets/Scenes/terrain/grass_flat_test.scene", nullptr), "stress scene load failed");
    size_t cells = 0, fields = 0, placements = 0;
    float minimumX = 1.e9f, maximumX = -1.e9f, minimumZ = 1.e9f, maximumZ = -1.e9f;
    GrassField* distant = nullptr;
    for (const auto& object : scene.GetObjects())
    {
        if (auto* terrain = object->GetComponent<Engine::Components::Terrain>())
        {
            ++cells;
            const auto origin = object->transform.GetWorldPosition();
            minimumX = std::min(minimumX, origin.x);
            minimumZ = std::min(minimumZ, origin.z);
            maximumX = std::max(maximumX, origin.x + terrain->cellsX * terrain->cellWidth);
            maximumZ = std::max(maximumZ, origin.z + terrain->cellsY * terrain->cellDepth);
            if (origin.x < -150 && origin.z < -150)
                for (auto* child : object->Children)
                    if (auto* field = child->GetComponent<GrassField>()) distant = field;
        }
        if (object->GetComponent<GrassField>()) ++fields;
        if (auto* layer = object->GetComponent<Engine::Components::TerrainObjectLayer>()) placements += layer->placements.size();
    }
    Require(cells == 441 && fields == 441 && placements == 648 * 441,
        "stress scene cell, grass field, or placement count is wrong");
    Require(minimumX == -168 && minimumZ == -168 && maximumX == 168 && maximumZ == 168,
        "stress scene is not centered or contains cell spacing gaps");
    auto* landscape = scene.FindObjectByName("Grass Terrain Landscape");
    auto* manager = landscape ? landscape->GetComponent<Engine::Components::TerrainManager>() : nullptr;
    Require(manager && manager->manageAuthoredCellColliders, "stress terrain collision manager missing");
    manager->Start();
    Require(manager->GetManagedColliderCellCount() == 441 && manager->GetActiveColliderCellCount() == 1,
        "stress scene must begin with one active terrain collider");
    auto& bullet = dynamic_cast<Engine::Physics::BulletPhysicsAdapter&>(scene.GetPhysics().GetAdapter());
    bullet.SetProfilingEnabled(true);
    double setup = 0;
    for (int frame = 0; frame < 10; ++frame)
    {
        manager->Update();
        scene.GetPhysics().Step(1./60.);
        if (frame > 0) setup += bullet.GetLastStepTimings().setupMs;
    }
    std::cout << "Stress terrain physics setup after initialization: " << setup/9 << " ms average; "
        << manager->GetActiveColliderCellCount() << " active terrain collider.\n";
    Require(distant != nullptr, "far corner cell missing");
    distant->Update();
    const auto& performance = distant->GetLastPerformance();
    Require(performance.placementsScanned == 0 && performance.tilesBuilt == 0 && performance.residentLods == 0,
        "distant grass performed generation work");
    std::cout << "Stress scene: " << cells << " terrain cells, " << placements
        << " grass placements, 336 x 336 metres; distant generation skipped.\n";
}

template<typename Renderer = Engine::Renderers::DX11GameRenderer>
void CheckFadeRendering()
{
    constexpr unsigned size = 128;
    Engine::Core::Window window(GetModuleHandleW(nullptr), L"Grass fade render check", size, size);
    Renderer renderer;
    std::cerr << "Fade renderer initialization" << std::endl;
    Require(renderer.Init(window.GetHWND(), size, size), "fade renderer failed");
    std::cerr << "Fade renderer initialized" << std::endl;
    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    std::cerr << "Fade scene initialized" << std::endl;
    scene.settings.skyboxEnabled = false;
    scene.settings.showGrid = false;
    auto* cameraOwner = scene.AddObject("Camera");
    cameraOwner->transform.position = {0,0,-3};
    auto* camera = cameraOwner->AddComponent<Engine::Components::Camera>();
    camera->useTransformRotation = false;
    camera->target = {0,0,0};
    // Reserve an earlier, off-camera draw slot, then hide it. Camera and
    // shadow masks must continue addressing the later persistent slots.
    auto* hiddenSlot=scene.AddObject("Retained off-camera slot");
    hiddenSlot->transform.position.x=1000;
    auto* hiddenMesh=hiddenSlot->AddComponent<Engine::Components::Mesh>();
    auto* hiddenMaterial=hiddenSlot->AddComponent<Engine::Components::Material>();
    hiddenMaterial->castsShadows=false;
    auto* owner = scene.AddObject("Fade triangle");
    auto* mesh = owner->AddComponent<Engine::Components::Mesh>();
    std::vector<Engine::Components::Mesh::Vertex> vertices(3);
    vertices[0].pos[0] = -1; vertices[0].pos[1] = -1;
    vertices[1].pos[0] = 1; vertices[1].pos[1] = -1;
    vertices[2].pos[1] = 1;
    for (auto& vertex : vertices)
    {
        vertex.normal[2] = -1;
        for (int channel = 0; channel < 4; ++channel) vertex.color[channel] = 1;
    }
    hiddenMesh->SetAuthoredVertices(vertices);
    hiddenMesh->CreateBuffer(renderer.GetGraphicsProvider()->GetBufferFactory());
    Require(mesh->SetAuthoredVertices(vertices), "fade triangle creation failed");
    mesh->CreateBuffer(renderer.GetGraphicsProvider()->GetBufferFactory());
    auto* material = owner->AddComponent<Engine::Components::Material>();
    material->unlit = true;
    material->doubleSided = true;
    material->diffuseColor = {1,1,1};
    material->castsShadows = false;
    std::vector<size_t> coverage;
    const auto requestCapture = [&]() {
        if constexpr (!std::is_same_v<Renderer, Engine::Renderers::DX11GameRenderer>)
            renderer.RequestSceneCapture();
    };
    const auto readCapture = [&](std::vector<uint8_t>& pixels) {
        if constexpr (std::is_same_v<Renderer, Engine::Renderers::DX11GameRenderer>)
            return renderer.CaptureFrameRGBA(pixels);
        else return renderer.ReadSceneCaptureRGBA(pixels);
    };
    for (float opacity : {0.f, .25f, .5f, .75f, 1.f})
    {
        mesh->SetRuntimeVisibility(opacity);
        // Refill both flip-model buffers before reading the presented image.
        for (int frame = 0; frame < 3; ++frame)
        {
            renderer.BeginFrame(); renderer.Clear(0,0,0,1);
            scene.PrepareRenderFrame();
            auto drawPacket = scene.TakeNativeRenderPacket();
            Engine::Graphics::GraphicsCommandPacket commands;
            scene.Render(&commands, 1.f, camera, false, size, size,
                false, drawPacket.get());
            auto context = renderer.CreateFrameGraphicsContext();
            commands.Replay(*context);
            requestCapture();
            renderer.EndFrame();
        }
        std::vector<uint8_t> pixels;
        Require(readCapture(pixels), "fade capture failed");
        size_t visible = 0;
        for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4)
            if (pixels[offset] > 100) ++visible;
        coverage.push_back(visible);
    }
    renderer.WaitIdle();
    Require(renderer.GetSubmittedFrameToken() > 0 &&
        renderer.IsFrameComplete(renderer.GetSubmittedFrameToken()),
        "renderer completion token did not finish after idle");
    std::cout << "Shader fade pixel coverage (0/25/50/75/100%): ";
    for (auto count : coverage) std::cout << count << ' ';
    std::cout << '\n';
    Require(coverage.front() == 0 && coverage.back() > 500, "fade endpoints rendered incorrectly");
    for (size_t index = 1; index < coverage.size(); ++index)
        Require(coverage[index] > coverage[index-1], "shader fade coverage is not progressive");

    hiddenSlot->enabled=false;
    using Mesh = Engine::Components::Mesh;
    auto* factory = renderer.GetGraphicsProvider()->GetBufferFactory();
    auto source = std::make_shared<Mesh>();
    auto combined = vertices;
    combined.insert(combined.end(), vertices.begin(), vertices.end());
    for (size_t i = 3; i < combined.size(); ++i)
    { combined[i].color[0] = 0; combined[i].color[2] = 0; }
    source->SetAuthoredVertices(combined);
    source->CreateBuffer(factory);
    auto instances = std::make_shared<Mesh::InstanceData>();
    // Skip record zero deliberately: verify nonzero firstInstance and firstVertex.
    for (float x : {-100.f, -.75f, .75f})
    {
        Mesh::InstanceTransform transform{};
        transform.rows[0] = {.45f, 0, 0, x};
        transform.rows[1] = {0, .45f, 0, 0};
        transform.rows[2] = {0, 0, .45f, 0};
        instances->transforms.push_back(transform);
    }
    for (uint32_t first : {1u, 2u})
    {
        Mesh::InstanceData::Slice slice;
        slice.firstInstance = first; slice.instanceCount = 1;
        slice.buffer = factory->CreateStaticShaderResource(sizeof(Mesh::InstanceTransform),
            instances->transforms.data() + first, sizeof(Mesh::InstanceTransform));
        instances->slices.push_back(std::move(slice));
    }
    mesh->SetInstancedGeometry(source, instances, {{0,3,1,1},{3,3,2,1}}, {-1.2f,-.45f,0}, {1.2f,.45f,0});
    instances->slices.clear();
    Require(mesh->IsReady(),"persistent instances require redundant native slice buffers");
    material->useVertexColors = true;
    bool captureShadow=false, captureNativePacket=false, separateShadowFrames=false;
    const auto capture = [&]() {
        for (int frame = 0; frame < 3; ++frame)
        {
            renderer.BeginFrame();
            scene.PrepareRenderFrame();
            auto drawPacket=captureNativePacket ? scene.TakeNativeRenderPacket():nullptr;
            auto context = renderer.CreateFrameGraphicsContext();
            if(captureShadow)
            {
                context->SetViewport({0,0,float(size),float(size),0,1});
                context->SetScissorRect({0,0,int32_t(size),int32_t(size)});
                scene.Render(context.get(),1.f,camera,false,size,size,true,drawPacket.get());
                if(separateShadowFrames)
                {
                    // Exercise both allocations with fresh frame attachment
                    // bindings; capacity testing must not depend on whether a
                    // backend restores its color target after a depth-only pass.
                    renderer.EndFrame(); renderer.BeginFrame();
                    context=renderer.CreateFrameGraphicsContext();
                }
                context->SetViewport({0,0,float(size),float(size),0,1});
                context->SetScissorRect({0,0,int32_t(size),int32_t(size)});
            }
            renderer.Clear(0,0,0,1);
            scene.Render(context.get(), 1.f, camera, false, size, size,false,drawPacket.get());
            requestCapture();
            renderer.EndFrame();
        }
        std::vector<uint8_t> pixels;
        Require(readCapture(pixels), "instance capture failed");
        renderer.WaitIdle();
        return pixels;
    };
    auto gpuInstances = capture();
    mesh->SetRuntimeVisibility(.5f);
    auto fadedInstances = capture();
    size_t fullCoverage = 0, fadedCoverage = 0;
    for (size_t i = 1; i < gpuInstances.size(); i += 4)
    {
        fullCoverage += gpuInstances[i] > 100;
        fadedCoverage += fadedInstances[i] > 100;
    }
    Require(fadedCoverage > fullCoverage * .3 && fadedCoverage < fullCoverage * .7,
        "instanced geometry does not preserve shader fades");
    mesh->SetRuntimeVisibility(1.f);
    auto expected = combined;
    for (size_t i = 0; i < expected.size(); ++i)
    {
        expected[i].pos[0] = expected[i].pos[0] * .45f + (i < 3 ? -.75f : .75f);
        expected[i].pos[1] *= .45f;
        expected[i].pos[2] *= .45f;
    }
    mesh->SetAuthoredVertices(expected);
    mesh->CreateBuffer(factory);
    auto cpuReference = capture();
    size_t differences = 0, lit = 0;
    for (size_t i = 0; i < gpuInstances.size(); ++i)
    {
        differences += std::abs(int(gpuInstances[i]) - int(cpuReference[i])) > 2;
        if (i % 4 != 3 && gpuInstances[i] > 100) ++lit;
    }
    std::cout << "Instance reference: " << lit << " lit channels, " << differences << " differing channels.\n";
    if (differences) for (const auto& image : {std::make_pair(".local/instances-gpu.ppm", &gpuInstances),
            std::make_pair(".local/instances-reference.ppm", &cpuReference)})
    {
        std::ofstream file(image.first, std::ios::binary);
        file << "P6\n" << size << ' ' << size << "\n255\n";
        for (size_t i = 0; i < image.second->size(); i += 4)
            file.write(reinterpret_cast<const char*>(image.second->data() + i), 3);
    }
    Require(lit > 500 && differences < 32, "GPU instance ranges/transforms differ from reference geometry");
    mesh->SetInstancedGeometry(source,instances,{{0,3,1,1},{3,3,2,1}},{-1.2f,-.45f,0},{1.2f,.45f,0});
    mesh->SetRuntimeVisibility(.6f);
    auto* second=scene.AddObject("Second instanced tile");
    second->transform.position={.1f,.5f,.1f};
    auto* secondMesh=second->AddComponent<Mesh>();
    secondMesh->SetInstancedGeometry(source,instances,{{0,3,1,1},{3,3,2,1}},{-1.2f,-.45f,0},{1.2f,.45f,0});
    secondMesh->SetRuntimeVisibility(.3f);
    auto* secondMaterial=second->AddComponent<Engine::Components::Material>();
    secondMaterial->useVertexColors=true;
    secondMaterial->unlit=true; secondMaterial->doubleSided=true;
    secondMaterial->castsShadows=false; secondMaterial->diffuseColor={1,1,1};
    scene.automaticInstancing=false;
    const auto individual=capture();
    scene.automaticInstancing=true;
    const auto batched=capture();
    size_t batchDifferences=0;
    for(size_t i=0;i<individual.size();++i)
        batchDifferences+=std::abs(int(individual[i])-int(batched[i]))>2;
    Require(batchDifferences<32,"cross-tile batching changed transforms, species, or independent fades");
    Require(scene.GetLastRenderCpuTimings().automaticBatches==2,"tiles were not merged into species batches");
    std::cout << "Cross-tile batch comparison: " << batchDifferences << " differing channels.\n";
    auto dense=std::make_shared<Mesh::InstanceData>();
    for(int i=0;i<64;++i)
    {
        Mesh::InstanceTransform transform{};
        transform.rows[0]={.08f,0,0,-.8f+float(i%8)*.22f};
        transform.rows[1]={0,.08f,0,-.8f+float(i/8)*.22f};
        transform.rows[2]={0,0,.08f,0}; dense->transforms.push_back(transform);
    }
    for(uint32_t first:{0u,32u})
    {
        Mesh::InstanceData::Slice slice; slice.firstInstance=first; slice.instanceCount=32;
        slice.buffer=factory->CreateStaticShaderResource(32*sizeof(Mesh::InstanceTransform),dense->transforms.data()+first,48);
        dense->slices.push_back(std::move(slice));
    }
    for(auto* instanced:{mesh,secondMesh})
        instanced->SetInstancedGeometry(source,dense,{{0,3,0,32},{3,3,32,32}},{-1,-1,-.2f},{1,1,.2f});
    scene.automaticInstancing=false; const auto denseReference=capture();
    scene.automaticInstancing=true; scene.gpuInstanceCulling=true; const auto denseGpu=capture();
    size_t denseDifferences=0;
    for(size_t i=0;i<denseGpu.size();++i) denseDifferences+=std::abs(int(denseGpu[i])-int(denseReference[i]))>2;
    Require(denseDifferences<32,"GPU grass compaction changed reference pixels");
    Require(scene.GetLastRenderCpuTimings().gpuBatches==2,"explicit grass did not use GPU indirect draws");
    Require(scene.GetLastRenderCpuTimings().changedUploadBytes==0,"unchanged grass placements were uploaded again");
    std::cout << "GPU grass compaction comparison: " << denseDifferences << " differing channels; unchanged upload zero.\n";
    for(auto* instanced:{mesh,secondMesh})
    {
        for(int lod=0;lod<3;++lod)
        {
            auto variant=std::make_unique<Mesh>();
            variant->SetInstancedGeometry(source,dense,{{0,3,0,32},{3,3,32,32}},{-1,-1,-.2f},{1,1,.2f});
            instanced->StoreRuntimeLod(lod,std::move(variant));
        }
        instanced->SelectRuntimeLod(0); instanced->gpuLodSelection=true;
        instanced->gpuLod1Distance=3.f; instanced->gpuLod2Distance=6.f;
    }
    for(float distance:{2.f,3.f,4.f,6.f,8.f})
    {
        cameraOwner->transform.position.z=-distance;
        scene.gpuInstanceCulling=false; const auto lodReference=capture();
        scene.gpuInstanceCulling=true; const auto lodGpu=capture();
        size_t lodDifferences=0;
        for(size_t i=0;i<lodGpu.size();++i) lodDifferences+=std::abs(int(lodGpu[i])-int(lodReference[i]))>2;
        Require(lodDifferences<32,"GPU grass LOD or complementary fade differs from fallback");
    }
    std::cout << "GPU grass LOD/fade intervals match fallback at five distances.\n";
    auto* sun=scene.AddObject("Shadow sun")->AddComponent<Engine::Components::Light>();
    sun->lightType=int(Engine::Components::Light::Type::Directional); sun->castsShadows=true;
    material->castsShadows=secondMaterial->castsShadows=true;
    std::vector<Mesh::Vertex> plane(6);
    const glm::vec2 coordinates[6]={{-2,-2},{2,-2},{2,2},{-2,-2},{2,2},{-2,2}};
    for(size_t i=0;i<6;++i)
    {
        plane[i].pos[0]=coordinates[i].x; plane[i].pos[1]=coordinates[i].y; plane[i].pos[2]=1;
        plane[i].normal[2]=-1; for(int channel=0;channel<4;++channel) plane[i].color[channel]=1;
    }
    auto* receiver=scene.AddObject("Shadow receiver"); auto* receiverMesh=receiver->AddComponent<Mesh>();
    receiverMesh->SetAuthoredVertices(plane); receiverMesh->CreateBuffer(factory);
    auto* receiverMaterial=receiver->AddComponent<Engine::Components::Material>();
    receiverMaterial->doubleSided=true; receiverMaterial->castsShadows=false;
    captureShadow=true; scene.gpuInstanceCulling=false; const auto shadowReference=capture();
    hiddenSlot->enabled=true;
    scene.gpuInstanceCulling=true; const auto shadowGpu=capture();
    size_t shadowDifferences=0;
    for(size_t i=0;i<shadowGpu.size();++i) shadowDifferences+=std::abs(int(shadowGpu[i])-int(shadowReference[i]))>2;
    Require(shadowDifferences<32,"GPU grass shadow compaction/fades changed receiver pixels");
    Require(scene.GetLastShadowCpuTiming().gpuGrassBatches>0,"shadow grass stayed on the CPU fallback");
    std::cout << "GPU grass shadow receiver comparison: " << shadowDifferences << " differing channels.\n";
    renderer.WaitIdle();
    std::cout << "GPU instancing matches CPU reference pixels; distinct species ranges and placement offsets verified.\n";

    // Existing frame/view buffers have only 512 slots. Grow persistent IDs
    // past 1024, then render one high-ID caster while old buffers are reused.
    // Count-based allocation would pass a handful of active draws and write
    // the later cascades beyond those old 512-slot shadow allocations.
    std::vector<Engine::Core::Object*> fillers;
    for(unsigned i=0;i<1025;++i)
    {
        auto* object=scene.AddObject("Sparse slot filler");
        object->transform.position.x=10000;
        auto* sparseMesh=object->AddComponent<Mesh>();
        sparseMesh->SetAuthoredVertices(vertices); sparseMesh->CreateBuffer(factory);
        auto* sparseMaterial=object->AddComponent<Engine::Components::Material>();
        sparseMaterial->unlit=true; sparseMaterial->doubleSided=true;
        sparseMaterial->diffuseColor={1,1,1}; sparseMaterial->castsShadows=true;
        fillers.push_back(object);
    }
    scene.PrepareRenderFrame();
    Require(scene.GetLastPrepareTimings().spatialActiveSlots>1024,"sparse slot fixture did not allocate high persistent IDs");
    for(auto* object:fillers) object->enabled=false;
    fillers.back()->enabled=true; fillers.back()->transform.position.x=0;
    owner->enabled=second->enabled=receiver->enabled=hiddenSlot->enabled=false;
    cameraOwner->transform.position={0,0,-3};
    Engine::Model::RealtimeShadowSettings sparseShadows;
    sparseShadows.directionalCascadeCount=4; sparseShadows.directionalResolution=512;
    scene.SetRealtimeShadowSettings(sparseShadows);
    // DX12 depth-only contexts leave attachment rebinding to their host.
    // DX11 capture reads flip-model buffers, so fill both with color frames.
    separateShadowFrames=std::is_same_v<Renderer,Engine::Renderers::DX12GameRenderer>;
    for(bool native:{false,true}) for(bool parallel:{false,true})
    {
        captureNativePacket=native;
        Engine::Threading::JobScheduler::Get().SetParallelForEnabled(parallel);
        for(unsigned reuse=0;reuse<3;++reuse)
        {
            const auto pixels=capture(); size_t visible=0;
            for(size_t i=0;i<pixels.size();i+=4) visible+=pixels[i]>100;
            std::cout<<"Sparse slots: native="<<native<<" parallel="<<parallel<<" reuse="<<reuse
                <<" visible="<<visible<<" active="<<scene.GetLastPrepareTimings().spatialActiveSlots
                <<" retained="<<scene.GetLastPrepareTimings().spatialRetainedSlots<<std::endl;
            Require(visible>500,"high persistent slot was not drawn after pass-buffer reuse");
            Require(scene.GetLastPrepareTimings().spatialActiveSlots<8,"sparse fixture still has a packed draw list");
            Require(scene.GetShadowDebugSnapshot().cascadeCount==4 && scene.GetShadowDebugSnapshot().submittedCasterDrawCount>0,
                "sparse high-ID caster did not exercise shadow cascades");
        }
    }
    Engine::Threading::JobScheduler::Get().SetParallelForEnabled(true);
    renderer.WaitIdle();
    std::cout<<"Sparse high-slot camera/shadow uploads passed across reused views, native packets and serial/parallel cascades.\n";

}

void TestComponentRegistry()
{
    struct Counter final : Engine::Core::Component { int updates=0; void Update() override { ++updates; } };
    Engine::Scene::Scene scene;
    auto* object=scene.AddObject("Registry");
    object->AddComponent<Engine::Components::Mesh>();
    scene.Update(1.f/60);
    auto* counter=object->AddComponent<Counter>();
    Require(scene.GetComponents<Counter>().size()==1,"component registry did not track additions");
    const auto revision=scene.GetComponentRevision<Counter>();
    const auto* registryData=scene.GetComponents<Counter>().data();
    auto* unrelated=scene.AddObject("Unrelated mesh");
    unrelated->AddComponent<Engine::Components::Mesh>();
    Require(scene.GetComponentRevision<Counter>()==revision && scene.GetComponents<Counter>().data()==registryData,
        "unrelated streaming invalidated a typed registry");
    scene.Update(1.f/60);
    Require(counter->updates==1,"new component on static object did not enter update list");
    object->enabled=false; scene.Update(1.f/60);
    Require(counter->updates==1,"disabled hierarchy updated cached components");
    object->enabled=true; scene.Update(1.f/60);
    Require(counter->updates==2,"reactivated hierarchy did not update");
    scene.RequestRemoveObject(object); scene.CommitPendingObjectChanges();
    Require(scene.GetComponents<Counter>().empty(),"component registry retained a deleted component");
    Engine::Rendering::RenderSpatialIndex spatial;
    std::vector<Engine::Rendering::RenderBounds> bounds{
        {{-.25f,-.25f,.1f},{.25f,.25f,.9f},true},{{4,-.25f,.1f},{5,.25f,.9f},true},{}};
    spatial.Update(bounds);
    auto visible=spatial.Query(glm::mat4(1));
    Require(visible[0] && !visible[1] && visible[2],"spatial culling lost bounded or conservative fallback objects");
    bounds[1]=bounds[0]; spatial.Update(bounds);
    Require(spatial.Query(glm::mat4(1))[1],"spatial hierarchy did not refresh moved bounds");
    Engine::Components::Mesh settings, loaded;
    settings.gpuLodSelection=true; settings.gpuLod1Distance=4; settings.gpuLod2Distance=12;
    loaded.Deserialize(settings.Serialize());
    Require(loaded.gpuLodSelection && loaded.gpuLod1Distance==4 && loaded.gpuLod2Distance==12,
        "GPU LOD settings failed serialization");
    std::cout<<"Component registry tracks add, removal, activation, and cached update callbacks.\n";
}

void TestBakedPlacements()
{
    using Layer=Engine::Components::TerrainObjectLayer;
    Layer source;
    for(int i=0;i<20;++i)
    {
        Layer::Placement placement; placement.asset=i%2 ? "a":"b";
        placement.position={float(i)-9,.5f,float(i%3)}; placement.normal={.2f,1,.1f}; placement.yawDegrees=float(i*17);
        source.placements.push_back(placement);
    }
    const auto saved=source.Serialize(); Layer loaded; loaded.Deserialize(saved);
    Require(loaded.HasLoadedPreparedData(),"saved placements did not load baked tile records");
    const auto& first=source.GetPreparedTiles(4); const auto& second=loaded.GetPreparedTiles(4);
    Require(first.size()==second.size(),"baked tile membership differs");
    for(const auto& [key,tile]:first) for(const auto& [asset,records]:tile.groups)
    {
        const auto& copy=second.at(key).groups.at(asset);
        Require(copy.size()==records.size(),"baked species records differ");
        for(size_t i=0;i<records.size();++i)
            Require(std::memcmp(&records[i].transform,&copy[i].transform,48)==0,"baked affine record changed");
    }
    const auto identity=loaded.placements.front().id;
    loaded.placements.front().position.x+=1; loaded.MarkConfigurationDirty(); loaded.GetPreparedTiles(4);
    Require(!loaded.HasLoadedPreparedData() && loaded.placements.front().id==identity,"placement edit lost identity or reused stale cache");
    auto invalid=saved; invalid.Set("bakedInstances",Layer::JsonValue("invalid"));
    loaded.Deserialize(invalid); Require(!loaded.HasLoadedPreparedData(),"corrupt cache was accepted");
    Require(loaded.GetPreparedTiles(4).size()==first.size(),"corrupt cache did not regenerate safely");
    std::cout<<"Baked tiles round-trip exact affines and IDs; edits and corrupt caches invalidate safely.\n";
}

void TestSceneLoadingIntoWarmCaches()
{
    using namespace Engine::Components;
    Engine::Scene::Scene source;
    auto* terrainOwner=source.AddObject("Loaded terrain");
    terrainOwner->AddComponent<Mesh>();
    auto* terrain=terrainOwner->AddComponent<Terrain>();
    terrain->cellsX=terrain->cellsY=16;
    terrain->cellWidth=terrain->cellDepth=1;
    terrainOwner->AddComponent<RigidBody>();
    auto* viewer=source.AddObject("Loaded viewer"); viewer->transform.position={2,0,2};
    auto* fieldOwner=source.AddObject("Loaded layer");
    fieldOwner->Parent=terrainOwner; terrainOwner->Children.push_back(fieldOwner);
    auto* layer=fieldOwner->AddComponent<TerrainObjectLayer>();
    TerrainObjectLayer::Placement placement;
    placement.asset="Assets/Mesh/Grass/meadow_short.obj"; placement.position={1,0,1};
    layer->placements.push_back(placement); layer->MarkConfigurationDirty();
    auto* field=fieldOwner->AddComponent<GrassField>();
    field->viewerObjectName="Loaded viewer"; field->fadeDuration=0;
    const auto saved=source.SaveToString();

    Engine::Scene::Scene loaded;
    // Normal editor startup queries these before another scene is opened.
    loaded.GetComponents<GrassField>(); loaded.GetComponents<SpatialUpdate>();
    loaded.GetComponents<TerrainObjectLayer>(); loaded.GetComponents<Mesh>(); loaded.GetComponents<RigidBody>();
    loaded.UpdateSpatialScripts(true);
    for(bool playing:{false,true})
    {
        Require(Engine::Serialization::SceneSerializer::LoadFromString(loaded,saved,nullptr),"fixture scene failed to load");
        Require(loaded.GetComponents<GrassField>().size()==1 && loaded.GetComponents<SpatialUpdate>().size()==1,
            "scene loading omitted grass from previously initialized component caches");
        Require(loaded.GetComponents<TerrainObjectLayer>().size()==1 && loaded.GetComponents<Mesh>().size()==1 && loaded.GetComponents<RigidBody>().size()==1,
            "scene loading omitted ordinary components from warm typed caches");
        field=loaded.FindObjectByName("Loaded layer")->GetComponent<GrassField>();
        if(playing) loaded.Start();
        for(unsigned frame=0;frame<24 && field->GetLastPerformance().residentLods==0;++frame)
        {
            loaded.ResetGrassPerformance();
            if(playing) loaded.Update(1.f/60.f);
            else { loaded.UpdateSpatialScripts(true); loaded.CommitPendingObjectChanges(); }
        }
        Require(field->GetLastPerformance().residentLods==3 && field->GetLastPerformance().activeTiles==1,
            "grass did not activate after scene switching or entering Play");
    }
    std::cout<<"Scene switching refreshes warm component caches and schedules grass in Edit and Play.\n";
}

void TestPersistentStorage()
{
    using Mesh=Engine::Components::Mesh;
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Persistent instance storage",64,64);
    Engine::Renderers::DX11GameRenderer renderer;
    Require(renderer.Init(window.GetHWND(),64,64),"pool renderer init failed");
    Engine::Rendering::PersistentInstancePool pool;
    auto source=std::make_shared<Mesh::InstanceData>(); source->transforms.resize(64);
    for(auto& transform:source->transforms) { transform={}; transform.rows[0].x=transform.rows[1].y=transform.rows[2].z=1; }
    for(unsigned frame=0;frame<12;++frame)
    {
        renderer.BeginFrame(); auto* provider=renderer.GetGraphicsProvider(); pool.BeginFrame(provider);
        if(frame==6) { source->transforms[19].rows[0].w=4; source->MarkDirty(); }
        if(frame==9)
        {
            auto replacement=std::make_shared<Mesh::InstanceData>(); replacement->transforms=source->transforms;
            replacement->transforms[23].rows[1].w=5; source=std::move(replacement);
        }
        const auto first=pool.Register(source,source->revision,source->transforms.data(),64,42);
        pool.Upload(provider);
        Require(pool.Get(first).identity.x==42,"persistent source identity changed");
        if(frame>=3 && frame<6) Require(pool.stats.newBytes+pool.stats.editedBytes+pool.stats.growthBytes==0,"resident pool reuploaded unchanged transforms");
        if(frame==6) Require(pool.stats.editedBytes==64 && pool.stats.growthBytes==0,"single placement edit uploaded more than one record");
        if(frame==9) Require(pool.stats.editedBytes==64 && pool.stats.newBytes+pool.stats.growthBytes==0,"saved tile replacement did not preserve its GPU allocation");
        renderer.EndFrame();
    }
    const auto maximum=pool.ResidentBytes()*4;
    for(unsigned frame=0;frame<90;++frame)
    {
        renderer.BeginFrame(); auto* provider=renderer.GetGraphicsProvider(); pool.BeginFrame(provider);
        auto streamed=std::make_shared<Mesh::InstanceData>(); streamed->transforms=source->transforms;
        pool.Register(streamed,1,streamed->transforms.data(),64); pool.Upload(provider);
        Require(pool.ResidentBytes()<=maximum,"streaming pool capacity grew without reuse");
        renderer.EndFrame();
    }
    renderer.WaitIdle();
    Require(renderer.GetSubmittedFrameToken() >= 102 &&
        renderer.IsFrameComplete(renderer.GetSubmittedFrameToken()),
        "DX11 completion queries did not retire submitted frames after idle");
    std::cout<<"Persistent storage: zero unchanged uploads, one 64-byte edited record, bounded streaming allocations.\n";
}

template<typename Renderer=Engine::Renderers::DX11GameRenderer>
void CheckAutomaticRendering()
{
    using Mesh=Engine::Components::Mesh;
    constexpr unsigned size=256;
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Automatic instancing check",size,size);
    Renderer renderer;
    Require(renderer.Init(window.GetHWND(),size,size),"automatic renderer init failed");
    Engine::Scene::Scene scene; scene.Init(renderer.GetGraphicsProvider());
    scene.settings.skyboxEnabled=false; scene.settings.showGrid=false;
    auto* cameraOwner=scene.AddObject("Camera"); cameraOwner->transform.position={0,0,-10};
    auto* camera=cameraOwner->AddComponent<Engine::Components::Camera>();
    camera->useTransformRotation=false; camera->target={0,0,0};
    std::vector<Mesh::Vertex> vertices(3);
    vertices[0].pos[0]=-.23f; vertices[0].pos[1]=-.23f;
    vertices[1].pos[0]=.23f; vertices[1].pos[1]=-.23f; vertices[2].pos[1]=.23f;
    for(auto& vertex:vertices) { vertex.normal[2]=-1; for(auto& color:vertex.color) color=1; }
    auto* factory=renderer.GetGraphicsProvider()->GetBufferFactory();
    std::vector<Mesh*> meshes;
    for(int i=0;i<64;++i)
    {
        auto* object=scene.AddObject("Repeated mesh");
        object->transform.position={(i%8-3.5f)*.65f,(i/8-3.5f)*.65f,0};
        auto* mesh=object->AddComponent<Mesh>();
        Require(i%2 ? mesh->SetIndexedGeometry(vertices,{0,1,2}) : mesh->SetAuthoredVertices(vertices),"repeated mesh failed"); mesh->CreateBuffer(factory);
        if(i) Require(mesh->GetGraphicsBuffer()==meshes.front()->GetGraphicsBuffer(),"duplicate geometry did not share GPU storage");
        meshes.push_back(mesh);
        auto* material=object->AddComponent<Engine::Components::Material>();
        material->unlit=true; material->doubleSided=true; material->castsShadows=false;
        material->diffuseColor={.2f+.1f*(i%7),.25f+.08f*(i%5),.4f+.07f*(i%3)};
    }
    const auto capture=[&]() {
        for(int frame=0;frame<3;++frame)
        {
            renderer.BeginFrame(); renderer.Clear(0,0,0,1); scene.PrepareRenderFrame();
            auto context=renderer.CreateFrameGraphicsContext(); context->SetScissorRect({0,0,int(size),int(size)}); scene.Render(context.get(),1.f,camera,false,size,size);
            if constexpr (!std::is_same_v<Renderer,Engine::Renderers::DX11GameRenderer>) renderer.RequestSceneCapture();
            renderer.EndFrame();
        }
        std::vector<uint8_t> pixels;
        if constexpr (std::is_same_v<Renderer,Engine::Renderers::DX11GameRenderer>)
            Require(renderer.CaptureFrameRGBA(pixels),"automatic capture failed");
        else Require(renderer.ReadSceneCaptureRGBA(pixels),"automatic capture failed");
        return pixels;
    };
    const auto compare=[&](const auto& reference,const auto& actual) {
        size_t differences=0,lit=0;
        for(size_t i=0;i<reference.size();++i)
        { differences+=std::abs(int(reference[i])-int(actual[i]))>2; if(i%4!=3 && actual[i]>20) ++lit; }
        std::cout<<"Automatic pixels: "<<lit<<" lit channels, "<<differences<<" differing channels.\n";
        Require(lit>100 && differences<32,"automatic instances differ from individual draws");
    };
    scene.automaticInstancing=false;
    auto reference=capture(); const auto referenceDraws=scene.GetLastOrdinaryDrawCount();
    scene.automaticInstancing=true; scene.gpuInstanceCulling=false;
    compare(reference,capture());
    Require(scene.GetLastOrdinaryDrawCount()<referenceDraws && scene.GetLastRenderCpuTimings().automaticInstances>=32,
        "repeated meshes did not batch");
    scene.gpuInstanceCulling=true;
    compare(reference,capture());
    Require(scene.GetLastRenderCpuTimings().gpuBatches>0,"large batch did not use GPU visibility and indirect drawing");
    const auto unchanged=capture();
    Require(scene.GetLastRenderCpuTimings().changedUploadBytes==0,"unchanged scene uploaded render or instance data again");
    cameraOwner->transform.position.x=-.7f; const auto leftReference=capture();
    cameraOwner->transform.position.x=.7f; const auto rightReference=capture();
    auto expectedViews=leftReference;
    for(unsigned y=0;y<size;++y) for(unsigned x=size/2;x<size;++x)
        for(unsigned channel=0;channel<4;++channel) expectedViews[(y*size+x)*4+channel]=rightReference[(y*size+x)*4+channel];
    for(int frame=0;frame<3;++frame)
    {
        renderer.BeginFrame(); renderer.Clear(0,0,0,1); scene.PrepareRenderFrame();
        auto context=renderer.CreateFrameGraphicsContext();
        cameraOwner->transform.position.x=-.7f; context->SetScissorRect({0,0,int(size/2),int(size)});
        scene.Render(context.get(),1.f,camera,false,size,size);
        cameraOwner->transform.position.x=.7f; context->SetScissorRect({int(size/2),0,int(size),int(size)});
        scene.Render(context.get(),1.f,camera,false,size,size);
        context->SetScissorRect({0,0,int(size),int(size)});
        if constexpr (!std::is_same_v<Renderer,Engine::Renderers::DX11GameRenderer>) renderer.RequestSceneCapture();
        renderer.EndFrame();
    }
    std::vector<uint8_t> twoViews;
    if constexpr (std::is_same_v<Renderer,Engine::Renderers::DX11GameRenderer>)
        Require(renderer.CaptureFrameRGBA(twoViews),"multi-view capture failed");
    else Require(renderer.ReadSceneCaptureRGBA(twoViews),"multi-view capture failed");
    compare(expectedViews,twoViews);
    cameraOwner->transform.position.x=0;
    const auto* original=meshes[1]->GetGraphicsBuffer();
    auto edited=vertices; edited[2].pos[1]+=.1f;
    Require(meshes[0]->UpdateAuthoredVertices(edited),"shared mesh edit failed");
    Require(meshes[0]->GetGraphicsBuffer()!=original && meshes[1]->GetGraphicsBuffer()==original,
        "editing shared geometry changed another instance");
    meshes[0]->SetAuthoredVertices(vertices); meshes[0]->CreateBuffer(factory);
    for(auto* mesh:meshes)
    {
        mesh->gpuLodSelection=true; mesh->gpuLod1Distance=8; mesh->gpuLod2Distance=12;
        mesh->useLightProbes=true;
        for(int lod=1;lod<3;++lod)
        {
            auto geometry=std::make_unique<Mesh>(); auto reduced=vertices;
            for(auto& vertex:reduced) { vertex.pos[0]*=lod==1?.8f:.6f; vertex.pos[1]*=lod==1?.8f:.6f; }
            geometry->SetAuthoredVertices(reduced); geometry->CreateBuffer(factory);
            Require(mesh->StoreRuntimeLod(lod,std::move(geometry)),"GPU LOD fixture failed");
        }
    }
    for(float distance:{5.f,10.f,20.f})
    {
        cameraOwner->transform.position.z=-distance;
        scene.automaticInstancing=false; reference=capture();
        scene.automaticInstancing=true; scene.gpuInstanceCulling=true;
        compare(reference,capture());
        Require(scene.GetLastRenderCpuTimings().gpuBatches>0,"GPU LOD selection did not dispatch");
        for(auto* mesh:meshes)
            Require(mesh->GetRuntimeLodMesh(1)->useLightProbes && mesh->GetRuntimeLodMesh(2)->useLightProbes,
                "GPU LOD variants lost the object's lighting settings");
    }
    renderer.WaitIdle();
    std::cout<<"Shared geometry, automatic batching, GPU visibility/LOD, dirty uploads, and edit isolation passed.\n";
}

void RenderAudit(const char* reportPath=".local/performance-logs/grass-automatic-instancing-render-audit.jsonl",bool matrix=false,bool storageAudit=false,bool compare=false,bool nativeAudit=false,unsigned width=1848,unsigned height=1069,bool reverseOrder=false)
{
    using Clock = std::chrono::steady_clock;
    Engine::Core::Window window(GetModuleHandleW(nullptr), L"Grass render audit", width, height);
    Engine::Renderers::DX11GameRenderer renderer;
    Require(renderer.Init(window.GetHWND(), width, height), "hardware renderer failed");
    if(nativeAudit) renderer.GetGraphicsProvider()->GetContextFactory()->SetNativeCallTimingEnabled(true);
    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    std::cout << "Loading 441-cell landscape; Debug loading can take about a minute." << std::endl;
    Require(scene.Load("Engine/Core/Assets/Scenes/terrain/grass_flat_test.scene"), "grass scene failed");
    auto* player = scene.FindObjectByName("Grass Test Capsule");
    auto* owner = scene.FindObjectByName("Grass Layer");
    auto* grass = owner ? owner->GetComponent<GrassField>() : nullptr;
    auto* camera = player ? player->GetComponent<Engine::Components::Camera>() : nullptr;
    Require(grass && camera, "grass/camera missing from scene");
    std::vector<GrassField*> fields;
    for (const auto& object : scene.GetObjects())
        if (auto* field = object->GetComponent<GrassField>()) fields.push_back(field);
    Require(fields.size() == 441, "render audit must exercise all terrain grass fields");
    if(storageAudit) for(auto* layer:scene.GetComponents<Engine::Components::TerrainObjectLayer>())
        Require(layer->HasLoadedPreparedData(),"landscape cell did not load its baked placement cache");
    scene.SetUpdateProfilingEnabled(true);
    const char* modes[]={"default","terrain-only","grass-no-shadows","full-grass"};
    if(compare) for(auto* field:fields) field->fadeDuration=0; // Identical coverage at each route frame.
    for(unsigned pass=0;pass<(matrix || compare ? 4u:storageAudit ? 2u:1u);++pass)
    {
    const unsigned mode=matrix ? pass:3u;
    const bool streaming=storageAudit && (compare ? pass>=2:pass>0);
    const bool parallel=!compare || ((pass%2==1) != reverseOrder);
    Engine::Threading::JobScheduler::Get().SetParallelForEnabled(parallel);
    if(compare)
    {
        // Each mode starts with the same generated-tile residency. Shared source
        // assets stay warm; the stationary warmup rebuilds identical active tiles.
        for(auto* field:fields) field->OnDestroy();
        scene.CommitPendingObjectChanges();
    }
    player->transform.position.z=0;
    const char* route=streaming ? "streaming":"resident";
    const std::string path=storageAudit ? std::string(*reportPath ? reportPath:".local/performance-logs/static-instance-")+route+(compare ? (parallel ? "-parallel":"-serial"):"")+".jsonl":matrix ? std::string(".local/performance-logs/landscape-matrix-")+modes[mode]+".jsonl":reportPath;
    std::cout << "Benchmark mode: " << modes[mode] << std::endl;
    std::ofstream report(path);
    unsigned measured = 0;
    uint64_t steadyBuilds = 0, steadyScans = 0, switches = 0;
    double snapshotMs = 0;
    size_t snapshotBytes = 0;
    double totalFrame = 0, totalShadow = 0, totalGrass = 0;
    bool sawShadows = false, warmed = false;
    for (int frame = 0; frame < 600; ++frame)
    {
        const auto started = Clock::now();
        // Stay within the authored tile radius while crossing LOD thresholds.
        player->transform.position.x = warmed ? 3.f * std::sin(measured * .04f):
            (matrix && frame>=31 ? 3.f*std::sin((frame-31)*.04f):0.f);
        if(streaming && warmed)
        {
            player->transform.position.x=64.f*std::sin(measured*.02f);
            player->transform.position.z=32.f*std::sin(measured*.04f);
        }
        scene.ResetGrassPerformance();
        if(!warmed || mode>=2) scene.UpdateSpatialScripts();
        const auto g = scene.GetGrassPerformance();
        scene.CommitPendingObjectChanges();
        if(matrix) for(const auto& object:scene.GetObjects())
            if(auto* mesh=object->GetComponent<Engine::Components::Mesh>())
            {
                const bool grassTile=object->name.find("Grass Tile (")==0;
                mesh->SetEditorVisible(mode!=0 && (!grassTile || mode>=2));
                if(grassTile) if(auto* material=object->GetComponent<Engine::Components::Material>())
                    material->castsShadows=mode==3 && material->castsShadows;
            }
        renderer.BeginFrame();
        renderer.Clear(.1f,.1f,.1f);
        scene.PrepareRenderFrame();
        auto context = renderer.CreateFrameGraphicsContext();
        scene.Render(context.get(), static_cast<float>(width) / height, camera, false, width, height, true);
        const auto submitted = scene.GetShadowDebugSnapshot().submittedCasterDrawCount;
        const auto eligibleCasters=scene.GetShadowDebugSnapshot().casterCount;
        scene.Render(context.get(), static_cast<float>(width) / height, camera, false, width, height);
        Require(submitted == scene.GetShadowDebugSnapshot().submittedCasterDrawCount,
            "main pass reset the shadow draw count");
        Require(eligibleCasters==scene.GetShadowDebugSnapshot().casterCount,"main pass reset eligible shadow casters");
        sawShadows |= submitted > 0;
        renderer.EndFrame();
        const double ms = std::chrono::duration<double,std::milli>(Clock::now()-started).count();
        const auto& shadow = scene.GetLastShadowCpuTiming();
        Require(shadow.explicitInstanceDraws+shadow.automaticDraws+shadow.ordinaryDraws==submitted,
            "shadow draw type counters do not match submitted draws");
        const auto& renderCpu=scene.GetLastRenderCpuTimings();
        const auto gpu=renderer.GetGraphicsProvider()->GetContextFactory()->GetFrameTimingTelemetry();
        const auto shadowSettings=scene.GetShadowDebugSnapshot();
        report << "{\"type\":\"frame_details\",\"playing\":true,\"benchmark_mode\":\"" << modes[mode]
            << "\",\"route\":\"" << route << "\",\"route_version\":\"static-instances-v1\",\"renderer\":\"dx11\""
            << ",\"parallel_preparation\":" << (parallel ? "true":"false") << ",\"fixed_fade_coverage\":" << (compare ? "true":"false")
            << ",\"warm_state_reset\":" << (compare ? "true":"false")
            << ",\"benchmark_scope\":\"render_and_grass_no_editor_or_physics\",\"measured\":" << (warmed ? "true":"false")
            << ",\"view_pixels\":" << width*height << ",\"frame\":" << frame << ",\"frame_ms\":" << ms
            << ",\"view_width\":" << width << ",\"view_height\":" << height
            << ",\"viewer_x\":" << player->transform.position.x << ",\"viewer_z\":" << player->transform.position.z
            << ",\"grass_ms\":" << g.totalMs << ",\"grass_built\":" << g.tilesBuilt
            << ",\"grass_scanned\":" << g.placementsScanned << ",\"grass_pending\":" << g.pending
            << ",\"grass_lod_switches\":" << g.lodSwitches << ",\"grass_gpu_bytes\":" << g.gpuBytes
            << ",\"grass_instances\":" << g.instances << ",\"grass_instance_upload_bytes\":" << g.instanceUploadBytes
            << ",\"shadow_cascades\":" << shadowSettings.cascadeCount
            << ",\"shadow_atlas_resolution\":" << shadowSettings.atlasResolution
            << ",\"shadow_submission_wall_ms\":" << shadow.submissionWallMs
            << ",\"shadow_submission_thread_cycles\":" << shadow.submissionThreadCycles
            << ",\"shadow_max_batch_ms\":" << shadow.maxBatchMs
            << ",\"gpu_valid\":" << (gpu.gpuTimingsValid ? "true":"false")
            << ",\"gpu_sample_id\":" << gpu.gpuSampleId
            << ",\"gpu_shadow_ms\":" << gpu.gpuMilliseconds[size_t(Engine::Graphics::GpuTimingStage::Shadow)]
            << ",\"gpu_terrain_ms\":" << gpu.gpuMilliseconds[size_t(Engine::Graphics::GpuTimingStage::Terrain)]
            << ",\"gpu_grass_ms\":" << gpu.gpuMilliseconds[size_t(Engine::Graphics::GpuTimingStage::Grass)]
            << ",\"gpu_opaque_ms\":" << gpu.gpuMilliseconds[size_t(Engine::Graphics::GpuTimingStage::Opaque)]
            << ",\"shadow_ms\":" << shadow.totalMs << ",\"shadow_draw_ms\":" << shadow.drawSubmissionMs
            << ",\"shadow_cb_ms\":" << shadow.constantBindingMs
            << ",\"shadow_geometry_ms\":" << shadow.geometryDrawMs
            << ",\"shadow_draws\":" << submitted
            << ",\"render_draws\":" << scene.GetLastOrdinaryDrawCount()
            << ",\"automatic_batches\":" << renderCpu.automaticBatches
            << ",\"automatic_instances\":" << renderCpu.automaticInstances
            << ",\"gpu_batches\":" << renderCpu.gpuBatches
            << ",\"render_material_ms\":" << renderCpu.materialMs
            << ",\"render_culling_ms\":" << renderCpu.cullingMs
            << ",\"render_sorting_ms\":" << renderCpu.sortingMs
            << ",\"render_batching_ms\":" << renderCpu.batchingMs
            << ",\"render_batch_snapshot_ms\":" << renderCpu.batchSnapshotMs
            << ",\"render_batch_build_ms\":" << renderCpu.batchBuildMs
            << ",\"render_batch_wait_ms\":" << renderCpu.batchWaitMs
            << ",\"render_batch_eligible_draws\":" << renderCpu.batchEligibleDraws
            << ",\"render_batch_cache_hits\":" << renderCpu.batchCacheHits
            << ",\"render_batch_worker_jobs\":" << renderCpu.batchWorkerJobs
            << ",\"render_batch_reused_keys\":" << renderCpu.batchReusedKeys
            << ",\"render_retained_ui_ms\":" << renderCpu.retainedUiMs
            << ",\"render_portal_ms\":" << renderCpu.portalMs
            << ",\"render_worker_active_ms\":" << renderCpu.workerActiveMs
            << ",\"render_main_job_ms\":" << renderCpu.mainJobMs
            << ",\"render_worker_wait_ms\":" << renderCpu.workerWaitMs
            << ",\"render_record_pack_ms\":" << renderCpu.recordPackingMs
            << ",\"render_record_wait_ms\":" << renderCpu.recordWaitMs
            << ",\"render_worker_queue_ms\":" << renderCpu.workerQueueMs
            << ",\"configured_workers\":" << renderCpu.configuredWorkers
            << ",\"prepare_bounds_reused\":" << scene.GetLastPrepareTimings().boundsReused
            << ",\"prepare_bounds_updated\":" << scene.GetLastPrepareTimings().boundsUpdated
            << ",\"prepare_worker_active_ms\":" << scene.GetLastPrepareTimings().workerMs
            << ",\"render_submit_ms\":" << renderCpu.submissionMs
            << ",\"render_upload_ms\":" << renderCpu.uploadMs
            << ",\"render_cpu_total_ms\":" << renderCpu.totalMs
            << ",\"render_startup_ms\":" << renderCpu.startupMs
            << ",\"render_terrain_prepare_ms\":" << renderCpu.terrainPreparationMs
            << ",\"render_occlusion_ms\":" << renderCpu.occlusionMs
            << ",\"render_other_ms\":" << renderCpu.otherMs;
        const auto& prep=scene.GetLastPrepareTimings();
        report<<",\"prepare_spatial_ms\":"<<prep.spatialMs
            <<",\"prepare_spatial_index_ms\":"<<prep.spatialIndexMs
            <<",\"prepare_spatial_compare_ms\":"<<prep.spatialCompareMs
            <<",\"prepare_spatial_refit_ms\":"<<prep.spatialRefitMs
            <<",\"prepare_spatial_build_ms\":"<<prep.spatialBuildMs
            <<",\"prepare_spatial_changed_bounds\":"<<prep.spatialChangedBounds
            <<",\"prepare_spatial_active_slots\":"<<prep.spatialActiveSlots
            <<",\"prepare_spatial_retained_slots\":"<<prep.spatialRetainedSlots
            <<",\"prepare_spatial_reused_bounds\":"<<prep.spatialReusedBounds
            <<",\"prepare_spatial_dirty_leaves\":"<<prep.spatialDirtyLeaves
            <<",\"prepare_spatial_refit_nodes\":"<<prep.spatialRefitNodes
            <<",\"prepare_spatial_rebuilds\":"<<prep.spatialRebuilds
            <<",\"prepare_spatial_quality_rebuilds\":"<<prep.spatialQualityRebuilds;
        report<<",\"prepare_transforms_ms\":"<<prep.transformsMs
            <<",\"prepare_transform_snapshot_ms\":"<<prep.transformSnapshotMs
            <<",\"prepare_transform_compute_ms\":"<<prep.transformComputeMs
            <<",\"prepare_transform_commit_ms\":"<<prep.transformCommitMs
            <<",\"prepare_transform_local_updated\":"<<prep.transformLocalUpdated
            <<",\"prepare_transform_world_updated\":"<<prep.transformWorldUpdated
            <<",\"prepare_transform_world_reused\":"<<prep.transformWorldReused
            <<",\"prepare_transform_levels\":"<<prep.transformLevels
            <<",\"prepare_transform_structure_fallback\":"<<prep.transformStructureFallback;
        renderCpu.instances.WriteJson(report); shadow.instances.WriteJson(report,"shadow_instance_");
        renderer.GetGraphicsProvider()->GetContextFactory()->GetNativeSubmissionTimings().WriteJson(report);
        report << "}\n";
        if(storageAudit) Require(renderCpu.instances.editedBytes==0 && shadow.instances.editedBytes==0,
            "camera movement edited static placement records");
        if (warmed)
        {
            ++measured;
            totalFrame += ms; totalShadow += shadow.totalMs; totalGrass += g.totalMs;
            steadyBuilds += g.tilesBuilt; steadyScans += g.placementsScanned; switches += g.lodSwitches;
            if (measured >= 240) break;
        }
        if (!warmed && frame > (matrix || storageAudit ? 300:30) && g.pending == 0 && g.tilesBuilt == 0)
        {
            const auto snapshotStarted = Clock::now();
            const auto snapshot = scene.SaveSnapshotToString();
            snapshotBytes = snapshot.size();
            snapshotMs = std::chrono::duration<double,std::milli>(Clock::now()-snapshotStarted).count();
            Require(scene.SaveToString().find("Grass Tile (") == std::string::npos,
                "runtime grass leaked into render-audit save");
            warmed = true;
        }
    }
    renderer.WaitIdle();
    if(mode==3) Require(sawShadows, "render audit never submitted shadow draws");
    Require(measured == 240, "grass did not finish warming");
    // Movement can enter previously unvisited cells. Those index their authored
    // placements once; the small fixture above verifies zero repeated scanning.
    Require(steadyScans <= fields.size() * 648, "render audit repeatedly indexed grass cells");
    std::cout << "Hardware grass audit: " << measured << " moving frames, CPU frame "
        << totalFrame/measured << " ms, shadow " << totalShadow/measured
        << " ms, grass " << totalGrass/measured << " ms; " << steadyBuilds
        << " new instance tiles, " << switches << " cached LOD switches.\n";
    std::cout << "Authored snapshot: " << snapshotBytes << " bytes in " << snapshotMs << " ms." << std::endl;
    }
    Engine::Threading::JobScheduler::Get().SetParallelForEnabled(true);
}

void TestRecordedOcclusionFeedback()
{
    struct Probe final : Engine::Graphics::IGraphicsContext
    {
        uint32_t frames = 0, decisions = 0, queries = 0, draws = 0, bindings = 0, computeBytes = 0;
        void SetComputeConstants(const void*, uint32_t size) override { computeBytes = size; }
        void SetPipeline(const Engine::Graphics::IPipelineState*) override { ++bindings; }
        void SetConstantBuffer(uint32_t, const Engine::Graphics::IGraphicsBuffer*, uint64_t) override {}
        void SetStructuredBuffer(uint32_t, const Engine::Graphics::IGraphicsBuffer*) override {}
        void SetVertexBuffer(uint32_t, const Engine::Graphics::IGraphicsBuffer*, uint32_t, uint64_t) override {}
        void SetIndexBuffer(const Engine::Graphics::IGraphicsBuffer*, uint32_t, uint64_t) override {}
        void SetViewport(const Viewport&) override {}
        void SetScissorRect(const ScissorRect&) override {}
        void Clear(float, float, float, float, float) override {}
        void DrawInstanced(uint32_t, uint32_t, uint32_t, uint32_t) override { ++draws; }
        void DrawIndexedInstanced(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override { ++draws; }
        void TransitionResource(void*, ResourceState, ResourceState) override {}
        void* GetNativeHandle() const override { return nullptr; }
        bool BeginOcclusionFrame(uint64_t, uint64_t) override { ++frames; return true; }
        bool IsOccluded(uint64_t id) override { ++decisions; return id == 7u; }
        void BeginOcclusionQuery(uint64_t) override { ++queries; }
    } probe;
    Engine::Graphics::GraphicsCommandPacket commands;
    Require(commands.BeginOcclusionFrame(3u, 5u), "recorder disabled occlusion queries");
    for (uint64_t id : {7u, 8u})
    {
        Require(!commands.IsOccluded(id), "recorder read backend occlusion state");
        commands.BeginOcclusionQuery(id);
        commands.SetPipeline(nullptr);
        commands.DrawInstanced(3, 1);
        commands.EndOcclusionQuery();
    }
    commands.Replay(probe);
    Require(probe.frames == 1 && probe.decisions == 2 &&
        probe.queries == 1 && probe.draws == 1 && probe.bindings == 2,
        "recorded occlusion did not skip only the hidden draw");
    Require(commands.GetReplayOcclusionCulledCount() == 1,
        "recorded occlusion count did not reflect replay");
    commands.Reset();
    commands.Replay(probe);
    Require(probe.frames == 1 && probe.draws == 1,
        "recycled packet replayed stale commands");
    Engine::Graphics::GraphicsCommandPacket constants;
    const uint32_t value = 42;
    constants.SetComputeConstants(&value, sizeof(value));
    constants.Replay(probe);
    Require(probe.computeBytes == sizeof(value), "compute constant capture failed");
    constants.Reset();
    constants.SetComputeConstants(nullptr, 0);
    constants.Replay(probe);
    Require(probe.computeBytes == 0, "recycled compute constants retained stale bytes");
}

void AuditStopRestore()
{
    using Clock=std::chrono::steady_clock;
    const auto ms=[](auto start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); };
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Stop restore audit",64,64);
    ShowWindow(window.GetHWND(),SW_HIDE);
    Engine::Renderers::DX11GameRenderer renderer;
    Require(renderer.Init(window.GetHWND(),64,64),"restore audit renderer failed");
    Engine::Scene::Scene scene; scene.Init(renderer.GetGraphicsProvider());
    scene.SetBeforeGpuResourceDestruction([&] { renderer.WaitIdle(); });
    std::cout<<"Loading full grass scene for stop/restore audit; waiting for load completion."<<std::endl;
    auto started=Clock::now();
    Require(scene.Load("Engine/Core/Assets/Scenes/terrain/grass_flat_test.scene"),"restore audit scene load failed");
    std::cout<<"Initial load: "<<ms(started)<<" ms, "<<scene.GetObjects().size()<<" objects."<<std::endl;
    started=Clock::now(); const auto baseline=scene.SaveSnapshotToString();
    std::cout<<"Pre-play snapshot: "<<ms(started)<<" ms, "<<baseline.size()<<" bytes."<<std::endl;
    started=Clock::now(); scene.Start(); scene.Update(1.f/60.f);
    std::cout<<"Start/update: "<<ms(started)<<" ms, "<<scene.GetObjects().size()<<" objects."<<std::endl;
    started=Clock::now(); const auto rollback=scene.SaveSnapshotToString();
    std::cout<<"Stop rollback snapshot: "<<ms(started)<<" ms, "<<rollback.size()<<" bytes."<<std::endl;
    started=Clock::now(); scene.FenceBackgroundWork();
    std::cout<<"Stop job fence: "<<ms(started)<<" ms."<<std::endl;
    started=Clock::now(); scene.ClearObjects();
    std::cout<<"Stop clear: "<<ms(started)<<" ms."<<std::endl;
    started=Clock::now();
    Require(Engine::Serialization::SceneSerializer::LoadFromString(scene,baseline,renderer.GetGraphicsProvider()),"restore audit deserialize failed");
    std::cout<<"Stop deserialize: "<<ms(started)<<" ms, "<<scene.GetObjects().size()<<" objects."<<std::endl;
    started=Clock::now(); scene.CommitPendingObjectChanges();
    std::cout<<"Stop publish: "<<ms(started)<<" ms."<<std::endl;
    renderer.WaitIdle();
}

int main(int argc, char** argv)
{
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* failure)->LONG {
        HMODULE module=nullptr;
        const auto address=failure->ExceptionRecord->ExceptionAddress;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address),&module);
        wchar_t name[MAX_PATH]{}; GetModuleFileNameW(module,name,MAX_PATH);
        std::wcerr << L"Native failure in " << name << L" offset " << std::hex << (uintptr_t(address)-uintptr_t(module)) << std::endl;
        return EXCEPTION_EXECUTE_HANDLER;
    });
    try
    {
        if(argc>1 && std::string(argv[1])=="--stop-restore-audit") AuditStopRestore();
        else if (argc > 1 && std::string(argv[1]) == "--landscape-matrix") RenderAudit("",true);
        else if(argc>1 && std::string(argv[1])=="--static-instance-audit") RenderAudit("",false,true);
        else if(argc>1 && std::string(argv[1])=="--parallel-stage-audit") RenderAudit(".local/performance-logs/parallel-stage-",false,true);
        else if(argc>1 && std::string(argv[1])=="--parallel-stage-comparison") RenderAudit(".local/performance-logs/parallel-comparison-",false,true,true);
        else if(argc>2 && std::string(argv[1])=="--current-scene-comparison") RenderAudit(argv[2],false,true,true);
        else if(argc>2 && std::string(argv[1])=="--current-scene-comparison-reverse") RenderAudit(argv[2],false,true,true,false,1848,1069,true);
        else if(argc>1 && std::string(argv[1])=="--native-submission-audit") RenderAudit(argc>2 ? argv[2] : ".local/performance-logs/native-submission-stage2.jsonl",false,false,false,true);
        else if(argc>1 && std::string(argv[1])=="--worker-batch-audit") RenderAudit(".local/performance-logs/parallel-stage7-",false,true);
        else if(argc>1 && std::string(argv[1])=="--persistent-spatial-audit") RenderAudit(".local/performance-logs/parallel-stage6-",false,true);
        else if(argc>1 && std::string(argv[1])=="--parallel-spatial-audit") RenderAudit(".local/performance-logs/parallel-stage5-",false,true);
        else if(argc>1 && std::string(argv[1])=="--parallel-slot-audit") RenderAudit(".local/performance-logs/parallel-slot-",false,true);
        else if(argc>1 && std::string(argv[1])=="--parallel-native-audit") RenderAudit(".local/performance-logs/parallel-native-stage7.jsonl",false,false,false,true);
        else if(argc>1 && std::string(argv[1])=="--parallel-gpu-stage-audit") RenderAudit(".local/performance-logs/parallel-gpu-stage7.jsonl");
        else if(argc>1 && std::string(argv[1])=="--parallel-gpu-halfres-audit") RenderAudit(".local/performance-logs/parallel-gpu-halfres-stage7.jsonl",false,false,false,false,924,535);
        else if(argc>1 && std::string(argv[1])=="--parallel-snapshot-audit") RenderAudit(".local/performance-logs/parallel-snapshot-stage9.jsonl");
        else if(argc>1 && std::string(argv[1])=="--snapshot-benchmark") BenchmarkSceneSnapshot();
        else if(argc>1 && std::string(argv[1])=="--static-render-cache-test") TestSceneTransformScheduling();
        else if(argc>1 && std::string(argv[1])=="--parallel-transform-audit") RenderAudit(".local/performance-logs/parallel-stage4-",false,true);
        else if(argc>1 && std::string(argv[1])=="--transform-benchmark") BenchmarkTransformHierarchy();
        else if(argc>1 && std::string(argv[1])=="--transform-regression") { TestTransformHierarchy(); TestSceneTransformScheduling(); }
        else if(argc>1 && std::string(argv[1])=="--component-update-regression") TestComponentUpdateScheduling();
        else if(argc>1 && std::string(argv[1])=="--physics-phase-regression") TestPhasedPhysicsStep();
        else if(argc>1 && std::string(argv[1])=="--native-regression") { TestRecordedOcclusionFeedback(); TestDx11ConstantsAndGeometry(); TestEditorFrameLifecycle(); }
        else if(argc>1 && std::string(argv[1])=="--lifecycle-regression") { TestSceneSnapshotCommit(); TestSimulationOwnerHandoff(); TestAsyncSceneOwnerCommit(); TestLifecycleWorkerFenceAndClock(); TestComponentLifecycleDelivery(); TestDeferredGraphicsPreparation(); TestScriptModuleJobFence(); TestSceneGpuDestructionBarrier(); }
        else if (argc > 1 && std::string(argv[1]) == "--render-audit") RenderAudit();
        else if(argc>1 && std::string(argv[1])=="--diagnostics-audit") RenderAudit(".local/performance-logs/grass-diagnostics-audit.jsonl");
        else if (argc > 1 && std::string(argv[1]) == "--fade-render-check") CheckFadeRendering();
        else if (argc > 1 && std::string(argv[1]) == "--instance-render-dx12") CheckFadeRendering<Engine::Renderers::DX12GameRenderer>();
#if defined(ENGINE_VULKAN_ENABLED)
        else if (argc > 1 && std::string(argv[1]) == "--instance-render-vulkan") CheckFadeRendering<Engine::Renderers::VulkanGameRenderer>();
#endif
        else if (argc > 1 && std::string(argv[1]) == "--scene-check") CheckStressScene();
        else if(argc>1 && std::string(argv[1])=="--automatic-render-check") CheckAutomaticRendering();
        else if(argc>1 && std::string(argv[1])=="--automatic-render-dx12") CheckAutomaticRendering<Engine::Renderers::DX12GameRenderer>();
#if defined(ENGINE_VULKAN_ENABLED)
        else if(argc>1 && std::string(argv[1])=="--automatic-render-vulkan") CheckAutomaticRendering<Engine::Renderers::VulkanGameRenderer>();
#endif
        else { TestEngineScheduling(); TestComponentUpdateScheduling(); TestPhasedPhysicsStep(); TestSceneSnapshotCommit(); TestSimulationOwnerHandoff(); TestNativeUiInteractionOwner(); TestAsyncSceneOwnerCommit(); TestLifecycleWorkerFenceAndClock(); TestSceneGpuDestructionBarrier(); TestMeshBufferRetirement(); TestRecordedOcclusionFeedback(); TestRenderBoundsCaching(); TestTransformHierarchy(); TestSceneTransformScheduling(); TestDirectorySnapshots(); TestSpatialScheduling(); TestLandscapePreparation(); TestGrass(); TestDx11ConstantsAndGeometry(); TestEditorFrameLifecycle(); TestComponentRegistry(); TestBakedPlacements(); TestSceneLoadingIntoWarmCaches(); TestPersistentStorage(); }
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
