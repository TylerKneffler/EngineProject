#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Obj/Terrain.h"
#include "Core/Compoonents/Obj/TerrainObjectLayer.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include <cstring>
#include <psapi.h>
#include <set>
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Renderers/Vulkan/VulkanGameRenderer.h"
#include "Core/Compoonents/Lighting/Light.h"
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace Engine::Renderers;
using namespace Engine::Components;
void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }

// Device-backed geometry audit, independent of rendering/texture warm-up.
template<class Renderer> void AuditOak(const std::string& backend)
{
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Tree allocation audit",64,64);
    ShowWindow(window.GetHWND(),SW_HIDE);
    Renderer renderer;
    Check(renderer.Init(window.GetHWND(),64,64),"audit renderer init failed");
    Engine::Scene::Scene scene;
    scene.SetBeforeGpuResourceDestruction([&] { renderer.WaitIdle(); });
    Check(Engine::Serialization::SceneSerializer::Load(scene,
        "Engine/Core/Assets/Scenes/terrain/oak_meadow.scene",nullptr),"audit scene load failed");
    const auto start=std::chrono::steady_clock::now();
    for (auto* layer:scene.GetComponents<TerrainObjectLayer>())
        layer->PreparePrefabInstances(renderer.GetGraphicsProvider());
    scene.CommitPendingObjectChanges();
    std::set<const Mesh*> sources;
    std::set<const Texture*> textures;
    size_t materialComponents=0;
    std::set<void*> native;
    uint64_t cpu=0,gpu=0,shadow=0,transforms=0,unsharedGeometry=0;
    size_t batches=0;
    for (auto* mesh:scene.GetComponents<Mesh>()) if (mesh->IsInstanced())
    {
        ++batches;
        if (auto* material=mesh->Owner->GetComponent<Material>())
        {
            ++materialComponents;
            for (const auto& texture : {material->baseColorTexture,material->normalTexture,
                material->metallicRoughnessTexture,material->occlusionTexture,
                material->emissiveTexture,material->heightTexture})
                if (texture) textures.insert(texture.get());
        }
        const auto* source=mesh->GetInstanceSource();
        Check(mesh->GetGraphicsBuffer()==source->GetGraphicsBuffer(),"batch allocated separate geometry");
        auto* buffer=source->GetGraphicsBuffer();
        Check(buffer && buffer->GetNativeHandle(),"native vertex allocation missing");
        transforms+=mesh->GetInstanceData()->transforms.capacity()*sizeof(Mesh::InstanceTransform);
        unsharedGeometry+=source->GetCpuMeshMemoryBytes()*mesh->GetInstanceData()->transforms.size();
        if (sources.insert(source).second) cpu+=source->GetCpuMeshMemoryBytes();
        if (native.insert(buffer->GetNativeHandle()).second)
        { gpu+=buffer->GetSize(); shadow+=buffer->GetUploadShadowSize(); }
    }
    Check(batches>sources.size() && !sources.empty(),"no spatial sharing audited");
    const auto snapshot=Engine::Serialization::SceneSerializer::SaveToString(scene);
    Check(snapshot.find("Shared prefab part:")==std::string::npos,"derived batches in snapshot");
    PROCESS_MEMORY_COUNTERS memory{}; memory.cb=sizeof(memory);
    Check(K32GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory)),"memory query failed");
    std::cout << "working_set_bytes=" << memory.WorkingSetSize << " peak_working_set_bytes="
        << memory.PeakWorkingSetSize << " materials=" << materialComponents << " textures=" << textures.size() << " ";
    std::cout << backend << " batches=" << batches << " sources=" << sources.size()
        << " native_vertex_allocations=" << native.size() << " source_cpu_bytes=" << cpu
        << " native_vertex_payload_bytes=" << gpu << " upload_shadow_bytes=" << shadow
        << " hypothetical_per_placement_cpu_bytes=" << unsharedGeometry
        << " transform_capacity_bytes=" << transforms << " snapshot_bytes=" << snapshot.size()
        << " prepare_ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()
        << std::endl;
    // Rebuild an independent layer with the same asset in separate spatial tiles.
    // CPU sources are layer-owned; native geometry must still reuse the content cache.
    TerrainObjectLayer::Placement probe;
    bool found=false;
    for (auto* layer:scene.GetComponents<TerrainObjectLayer>())
        for (const auto& placement:layer->placements)
            if (std::filesystem::path(placement.asset).extension()==".prefab")
            { probe=placement; found=true; break; }
    Check(found,"cross-layer probe asset missing");
    auto* second=scene.AddObject("Allocation audit second layer")->AddComponent<TerrainObjectLayer>();
    probe.position={0,0,0}; second->placements.push_back(probe);
    probe.position={128,0,128}; second->placements.push_back(probe);
    second->MarkConfigurationDirty();
    second->PreparePrefabInstances(renderer.GetGraphicsProvider());
    scene.CommitPendingObjectChanges();
    size_t crossLayerBatches=0;
    for (auto* mesh:scene.GetComponents<Mesh>()) if (mesh->IsInstanced() &&
        mesh->Owner->GetComponentInParent<TerrainObjectLayer>()==second)
    {
        ++crossLayerBatches;
        Check(native.count(mesh->GetGraphicsBuffer()->GetNativeHandle())==1,
            "identical geometry allocated a new native buffer across layers");
    }
    Check(crossLayerBatches>0,"cross-layer probe produced no batches");
    std::cout << "cross_layer_shared_batches=" << crossLayerBatches << std::endl;
    renderer.WaitIdle();
}

int main(int argc,char** argv)
{
    try
    {
        Check(argc==2,"expected dx11, dx12 or vulkan");
        const std::string backend=argv[1];
        if (backend=="dx11") AuditOak<DX11GameRenderer>(backend);
        else if (backend=="dx12") AuditOak<DX12GameRenderer>(backend);
#if defined(ENGINE_VULKAN_ENABLED)
        else if (backend=="vulkan") AuditOak<VulkanGameRenderer>(backend);
#endif
        else throw std::runtime_error("backend unavailable");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
