#include "Core/Scene/Scene.h"
#include "Core/Rendering/RenderJobs.h"
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
#include <chrono>
#include <cmath>
#include "Core/Window.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Renderers/Vulkan/VulkanGameRenderer.h"
#include <type_traits>
#include "Core/Compoonents/Camera/Camera.h"

void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

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
            auto context = renderer.CreateFrameGraphicsContext();
            scene.Render(context.get(), 1.f, camera, false, size, size);
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
    std::cout << "Shader fade pixel coverage (0/25/50/75/100%): ";
    for (auto count : coverage) std::cout << count << ' ';
    std::cout << '\n';
    Require(coverage.front() == 0 && coverage.back() > 500, "fade endpoints rendered incorrectly");
    for (size_t index = 1; index < coverage.size(); ++index)
        Require(coverage[index] > coverage[index-1], "shader fade coverage is not progressive");

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
    bool captureShadow=false;
    const auto capture = [&]() {
        for (int frame = 0; frame < 3; ++frame)
        {
            renderer.BeginFrame();
            scene.PrepareRenderFrame();
            auto context = renderer.CreateFrameGraphicsContext();
            if(captureShadow)
            {
                context->SetViewport({0,0,float(size),float(size),0,1});
                context->SetScissorRect({0,0,int32_t(size),int32_t(size)});
                scene.Render(context.get(),1.f,camera,false,size,size,true);
                context->SetViewport({0,0,float(size),float(size),0,1});
                context->SetScissorRect({0,0,int32_t(size),int32_t(size)});
            }
            renderer.Clear(0,0,0,1);
            scene.Render(context.get(), 1.f, camera, false, size, size);
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
    scene.gpuInstanceCulling=true; const auto shadowGpu=capture();
    size_t shadowDifferences=0;
    for(size_t i=0;i<shadowGpu.size();++i) shadowDifferences+=std::abs(int(shadowGpu[i])-int(shadowReference[i]))>2;
    Require(shadowDifferences<32,"GPU grass shadow compaction/fades changed receiver pixels");
    Require(scene.GetLastShadowCpuTiming().gpuGrassBatches>0,"shadow grass stayed on the CPU fallback");
    std::cout << "GPU grass shadow receiver comparison: " << shadowDifferences << " differing channels.\n";
    renderer.WaitIdle();
    std::cout << "GPU instancing matches CPU reference pixels; distinct species ranges and placement offsets verified.\n";

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

void RenderAudit(const char* reportPath=".local/performance-logs/grass-automatic-instancing-render-audit.jsonl",bool matrix=false,bool storageAudit=false)
{
    using Clock = std::chrono::steady_clock;
    constexpr unsigned width = 1848, height = 1069;
    Engine::Core::Window window(GetModuleHandleW(nullptr), L"Grass render audit", width, height);
    Engine::Renderers::DX11GameRenderer renderer;
    Require(renderer.Init(window.GetHWND(), width, height), "hardware renderer failed");
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
    for(unsigned pass=0;pass<(matrix ? 4u:storageAudit ? 2u:1u);++pass)
    {
    const unsigned mode=matrix ? pass:3u;
    const char* route=storageAudit ? (pass ? "streaming":"resident"):"resident";
    const std::string path=storageAudit ? std::string(".local/performance-logs/static-instance-")+route+".jsonl":matrix ? std::string(".local/performance-logs/landscape-matrix-")+modes[mode]+".jsonl":reportPath;
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
        if(storageAudit && pass && warmed)
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
            << "\",\"route\":\"" << route << "\",\"route_version\":\"static-instances-v1\",\"renderer\":\"dx11"
            << "\",\"benchmark_scope\":\"render_and_grass_no_editor_or_physics\",\"measured\":" << (warmed ? "true":"false")
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
            << ",\"render_retained_ui_ms\":" << renderCpu.retainedUiMs
            << ",\"render_portal_ms\":" << renderCpu.portalMs
            << ",\"render_worker_active_ms\":" << renderCpu.workerActiveMs
            << ",\"render_main_job_ms\":" << renderCpu.mainJobMs
            << ",\"render_worker_wait_ms\":" << renderCpu.workerWaitMs
            << ",\"render_record_pack_ms\":" << renderCpu.recordPackingMs
            << ",\"render_record_wait_ms\":" << renderCpu.recordWaitMs
            << ",\"render_submit_ms\":" << renderCpu.submissionMs
            << ",\"render_upload_ms\":" << renderCpu.uploadMs
            << ",\"render_cpu_total_ms\":" << renderCpu.totalMs
            << ",\"render_startup_ms\":" << renderCpu.startupMs
            << ",\"render_terrain_prepare_ms\":" << renderCpu.terrainPreparationMs
            << ",\"render_occlusion_ms\":" << renderCpu.occlusionMs
            << ",\"render_other_ms\":" << renderCpu.otherMs;
        renderCpu.instances.WriteJson(report); shadow.instances.WriteJson(report,"shadow_instance_");
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
        if (argc > 1 && std::string(argv[1]) == "--landscape-matrix") RenderAudit("",true);
        else if(argc>1 && std::string(argv[1])=="--static-instance-audit") RenderAudit("",false,true);
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
        else { TestSpatialScheduling(); TestLandscapePreparation(); TestGrass(); TestDx11ConstantsAndGeometry(); TestComponentRegistry(); TestBakedPlacements(); TestSceneLoadingIntoWarmCaches(); TestPersistentStorage(); }
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
