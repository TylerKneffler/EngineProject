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
#include <set>
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Renderers/Vulkan/VulkanGameRenderer.h"
#include "Core/Assets/Scripts/Grass/GrassField.h"
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

void CheckSharedPrefabLayer()
{
    using namespace Engine::Graphics;
    struct Buffer : IGraphicsBuffer
    {
        Usage usage; AccessMode access; uint32_t stride; std::vector<uint8_t> bytes;
        Usage GetUsage() const override { return usage; }
        AccessMode GetAccessMode() const override { return access; }
        uint64_t GetSize() const override { return bytes.size(); }
        uint32_t GetElementStride() const override { return stride; }
        void* Map() override { return bytes.data(); }
        void Unmap() override {}
        void* GetNativeHandle() const override { return const_cast<Buffer*>(this); }
    };
    struct Provider : IGraphicsProvider, IGraphicsBufferFactory
    {
        unsigned allocations=0;
        IShaderCompiler* GetShaderCompiler() override { return nullptr; }
        IGraphicsBufferFactory* GetBufferFactory() override { return this; }
        IPipelineStateFactory* GetPipelineStateFactory() override { return nullptr; }
        IGraphicsContextFactory* GetContextFactory() override { return nullptr; }
        std::unique_ptr<IGraphicsBuffer> CreateBuffer(IGraphicsBuffer::Usage usage,
            IGraphicsBuffer::AccessMode access,uint64_t bytes,const void* data,uint32_t stride) override
        {
            auto result=std::make_unique<Buffer>(); result->usage=usage; result->access=access;
            result->stride=stride; result->bytes.resize(bytes);
            if (data) std::memcpy(result->bytes.data(),data,bytes);
            ++allocations; return result;
        }
    } provider;
    std::filesystem::create_directories(".local/tests-generated");
    const std::string meshPath=".local/tests-generated/shared-layer.mesh";
    const std::string prefabPath=".local/tests-generated/shared-layer.prefab";
    std::vector<Mesh::Vertex> vertices(3);
    vertices[1].pos[0]=1; vertices[2].pos[1]=1;
    for (auto& vertex:vertices) { vertex.normal[2]=1; for (auto& color:vertex.color) color=1; }
    if (!std::filesystem::exists(meshPath))
        Check(Mesh::SaveNativeFile(meshPath,vertices),"fixture mesh save failed");
    auto writePrefab=[&](float radius,const std::string& destination) {
        std::ofstream out(destination);
        out << "<Prefab xmlns:components=\"urn:engine-components\" version=\"1\" type=\"prefab\">"
            "<object name=\"Shared test tree\"><components><components:Mesh Mesh.file=\"" << meshPath << "\"/>";
        if (radius>0) out << "<components:PrimitiveObjectCollider PrimitiveObjectCollider.shape=\"Capsule\" "
            "PrimitiveObjectCollider.radius=\"" << radius << "\" PrimitiveObjectCollider.height=\"3\"/>";
        out << "</components><children/></object></Prefab>";
    };
    writePrefab(0,prefabPath);
    std::cerr << "Shared prefab fixture written\n";
    Engine::Scene::Scene scene;
    auto* owner=scene.AddObject("Painted objects"); owner->transform.position={-180,0,-180};
    auto* layer=owner->AddComponent<TerrainObjectLayer>();
    for (int i=0;i<2;++i)
    {
        TerrainObjectLayer::Placement p; p.asset=prefabPath; p.position={180.f+2*i,0,180};
        p.scale=i+1.f; layer->placements.push_back(p);
    }
    layer->MarkConfigurationDirty(); layer->PreparePrefabInstances(&provider); scene.CommitPendingObjectChanges();
    std::cerr << "Shared prefab first preparation complete\n";
    const auto meshes=scene.GetComponents<Mesh>();
    Check(meshes.size()==1 && meshes.front()->IsInstanced(),"prefab geometry was not shared");
    Check(meshes.front()->GetInstanceData()->transforms.size()==2,"placement instances missing");
    Check(provider.allocations==1,"source vertex buffer duplicated per placement");
    Check(glm::length(meshes.front()->Owner->transform.GetWorldPosition()-glm::vec3(-180,0,-180))<.001f,
        "shared batch did not inherit the layer transform");
    Check(scene.GetComponents<PrimitiveObjectCollider>().empty() && scene.GetComponents<RigidBody>().empty(),
        "colliders synthesized for collider-free prefab");
    Check(Engine::Serialization::SceneSerializer::SaveToString(scene).find("Shared prefab part:")==std::string::npos,
        "derived instances leaked into authored scene serialization");
    const std::string lodPrefabPath=".local/tests-generated/shared-layer-lod.prefab";
    writePrefab(0,lodPrefabPath);
    TerrainObjectLayer::Placement lodPlacement;
    lodPlacement.asset=std::filesystem::absolute(lodPrefabPath).generic_string();
    lodPlacement.position={184,0,180}; layer->placements.push_back(lodPlacement);
    layer->objectLods[prefabPath]={{prefabPath,lodPrefabPath},{28}};
    layer->MarkConfigurationDirty();
    for (float radius : {.8f,1.25f,0.f})
    {
        std::cerr << "Checking prefab collider radius " << radius << '\n';
        writePrefab(radius,prefabPath);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        layer->PreparePrefabInstances(&provider); scene.CommitPendingObjectChanges();
        std::cerr << "Prefab edit prepared\n";
        const auto colliders=scene.GetComponents<PrimitiveObjectCollider>();
        Check(colliders.size()==(radius>0 ? 3u:0u),"prefab collider edit did not affect all placements and LODs");
        Check(scene.GetComponents<RigidBody>().size()==colliders.size(),"static prefab bodies missing");
        for (auto* collider:colliders) Check(std::abs(collider->radius-radius)<.0001f,"shared collider radius stale");
    }
    Engine::Scene::Scene forest;
    Check(Engine::Serialization::SceneSerializer::Load(forest,
        "Engine/Core/Assets/Scenes/terrain/oak_meadow.scene",nullptr),"headless oak load failed");
    const auto forestLayers=forest.GetComponents<TerrainObjectLayer>();
    size_t placements=0;
    for (auto* objects:forestLayers)
    {
        if (objects->layerName=="Oak forest") placements=objects->placements.size();
        objects->PreparePrefabInstances(&provider);
    }
    forest.CommitPendingObjectChanges();
    Check(placements==2048,"headless forest lost authored placements");
    std::set<const Mesh*> sources;
    size_t instanceParts=0,batches=0;
    std::set<const Mesh::InstanceData*> placementArrays;
    for (auto* mesh:forest.GetComponents<Mesh>()) if (mesh->IsInstanced())
    {
        ++batches; sources.insert(mesh->GetInstanceSource());
        if (placementArrays.insert(mesh->GetInstanceData().get()).second)
            instanceParts+=mesh->GetInstanceData()->transforms.size();
        for (const auto& placement:mesh->GetInstanceData()->transforms)
        {
            const auto point=mesh->Owner->transform.GetWorldMatrix()*glm::vec4(
                placement.rows[0].w,placement.rows[1].w,placement.rows[2].w,1);
            Check(std::abs(point.x)<180 && std::abs(point.z)<180,"forest placement shifted outside terrain");
        }
    }
    Check(instanceParts==2048 && sources.size()==85 && batches>sources.size(),
        "forest did not share bark/leaf sources across spatial tiles");
    for (auto* collider:forest.GetComponents<PrimitiveObjectCollider>())
        Check(collider->Owner->name=="Meadow Explorer","unexpected tree collider in forest");
    for (auto* body:forest.GetComponents<RigidBody>())
    {
        auto* objects=body->Owner->GetComponentInParent<TerrainObjectLayer>();
        Check(!objects || objects->layerName!="Oak forest","forest still has per-tree rigid bodies");
    }
    std::cout << "Forest: " << placements << " placements; " << sources.size()
        << " shared source meshes; " << batches << " spatial batches; no tree colliders.\n";
    std::cout << "Shared geometry, transformed placements, serialization, collider addition/edit/removal passed.\n";
}

template<class Renderer> void CaptureOak(const std::string& backend)
{
    constexpr unsigned width=960,height=540;
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Actual oak meadow validation",width,height);
    ShowWindow(window.GetHWND(),SW_HIDE);
    Renderer renderer;
    Check(renderer.Init(window.GetHWND(),width,height),"oak renderer init failed");
    Engine::Scene::Scene scene; scene.Init(renderer.GetGraphicsProvider());
    scene.SetBeforeGpuResourceDestruction([&] { renderer.WaitIdle(); });
    Check(scene.Load("Engine/Core/Assets/Scenes/terrain/oak_meadow.scene"),"oak scene load failed");
    auto* viewer=scene.FindObjectByName("Meadow Explorer");
    Check(viewer!=nullptr,"authored oak viewer missing");
    auto* camera=viewer->GetComponent<Camera>();
    Check(camera!=nullptr,"authored oak camera missing");
    const auto fields=scene.GetComponents<GrassField>();
    Check(!fields.empty(),"oak grass scripts missing");
    for (auto* field:fields) { field->useEditorCamera=false; field->Start(); }
    const auto terrains=scene.GetComponents<Terrain>();
    Check(!terrains.empty(),"oak terrain missing");
    auto* terrain=terrains.front();
    std::filesystem::create_directories(".local/tree-lod-oak-validation");
    std::ofstream report(".local/tree-lod-oak-validation/"+backend+".log");
    report << "Source: Engine/Core/Assets/Scenes/terrain/oak_meadow.scene\n"
        << "Actual scene; authored entrance camera and established overlook camera; "
        << width << 'x' << height << "\n";
    bool refreshGrass=true;
    auto render=[&] {
        scene.AdvanceManualFrame(0);
        scene.ResetGrassPerformance();
        if (refreshGrass) for (auto* field:fields) field->RefreshEditorPreview();
        renderer.BeginFrame(); scene.PrepareRenderFrame();
        auto context=renderer.CreateFrameGraphicsContext();
        context->SetViewport({0,0,float(width),float(height),0,1});
        context->SetScissorRect({0,0,int(width),int(height)});
        scene.Render(context.get(),float(width)/height,camera,false,width,height,true);
        if constexpr (std::is_same_v<Renderer,DX12GameRenderer>)
        {
            renderer.EndFrame(); renderer.BeginFrame(); context=renderer.CreateFrameGraphicsContext();
        }
        context->SetViewport({0,0,float(width),float(height),0,1});
        context->SetScissorRect({0,0,int(width),int(height)});
        renderer.Clear(.1f,.1f,.1f,1);
        scene.Render(context.get(),float(width)/height,camera,false,width,height);
        if constexpr (!std::is_same_v<Renderer,DX11GameRenderer>) renderer.RequestSceneCapture();
        renderer.EndFrame();
    };
    auto capture=[&](const std::string& name,int frames) {
        for (int i=0;i<frames;++i) render();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
        auto texturesReady=[&] {
            bool ready=true;
            for (auto* material:scene.GetComponents<Material>())
                for (const auto& texture : {material->baseColorTexture,material->normalTexture,
                    material->metallicRoughnessTexture,material->occlusionTexture,
                    material->emissiveTexture,material->heightTexture})
                    if (texture && !texture->Prepare(renderer.GetGraphicsProvider())) ready=false;
            return ready;
        };
        while (!texturesReady())
        {
            Check(std::chrono::steady_clock::now()<deadline,"oak material textures did not become GPU-ready");
            render();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Refill flip-model buffers and refresh material bindings after uploads.
        for (int i=0;i<4;++i) render();
        std::vector<uint8_t> pixels;
        if constexpr (std::is_same_v<Renderer,DX11GameRenderer>)
            Check(renderer.CaptureFrameRGBA(pixels),"oak capture failed");
        else Check(renderer.ReadSceneCaptureRGBA(pixels),"oak capture failed");
        Check(pixels.size()==width*height*4,"oak capture size wrong");
        std::ofstream image(".local/tree-lod-oak-validation/"+backend+"-"+name+".ppm",std::ios::binary);
        image << "P6\n" << width << ' ' << height << "\n255\n";
        for (size_t i=0;i<pixels.size();i+=4) image.write(reinterpret_cast<const char*>(pixels.data()+i),3);
        for (auto* material:scene.GetComponents<Material>())
            Check(material->shaderError.empty(),"oak material shader error");
        const auto& shadow=scene.GetShadowDebugSnapshot();
        report << name << ": selectedShadow=" << shadow.hasSelectedLight << " casters="
            << shadow.submittedCasterDrawCount << " grassFields=" << fields.size() << std::endl;
        return pixels;
    };
    for (const std::string view : {"entrance","overlook"})
    {
        if (view=="overlook") { viewer->transform.position={-5,27,-39}; camera->target={2,1,5}; }
        capture(view,view=="entrance" ? 45:12);
        std::vector<std::pair<Material*,bool>> savedMaterials;
        for (auto* material:scene.GetComponents<Material>())
        { savedMaterials.emplace_back(material,material->castsShadows); material->castsShadows=true; }
        struct SavedLight { Light* light; bool casts; float resolution; };
        std::vector<SavedLight> savedLights;
        for (auto* light:scene.GetComponents<Light>())
        { savedLights.push_back({light,light->castsShadows,light->shadowResolutionScale}); light->castsShadows=false; }
        const auto noShadows=capture(view+"-diagnostic-shadows-off",4);
        for (const auto& saved:savedLights)
            if (saved.light->GetLightType()==Light::Type::Directional)
            { saved.light->castsShadows=true; saved.light->shadowResolutionScale=1; }
        const auto normal=capture(view+"-diagnostic-shadows-on",8);
        Check(scene.GetShadowDebugSnapshot().hasSelectedLight,"oak diagnostic directional light was not shadowed");
        Check(scene.GetShadowDebugSnapshot().submittedCasterDrawCount>0,"oak diagnostic shadow casters missing");
        size_t darker=0;
        for (size_t i=0;i<normal.size();i+=4)
            darker+=int(noShadows[i])+int(noShadows[i+1])+int(noShadows[i+2])>
                int(normal[i])+int(normal[i+1])+int(normal[i+2])+15;
        report << view << ": directional shadow darkened pixels=" << darker << std::endl;
        Check(darker>100,"oak directional shadows had no visible effect");
        for (const auto& saved:savedLights)
        { saved.light->castsShadows=saved.casts; saved.light->shadowResolutionScale=saved.resolution; }
        for (const auto& saved:savedMaterials) saved.first->castsShadows=saved.second;
        // Override the authored material sidedness to prove terrain still culls
        // its underside, using the actual scene with other objects hidden.
        std::vector<Engine::Core::Object*> hidden;
        refreshGrass=false;
        for (const auto& object:scene.GetObjects())
            if (object.get()!=terrain->Owner && object.get()!=viewer && object->enabled)
            { hidden.push_back(object.get()); object->enabled=false; }
        const auto savedPosition=viewer->transform.position;
        const auto savedTarget=camera->target;
        viewer->transform.position={0,-25,0}; camera->target={0,0,.1f};
        auto* terrainMaterial=terrain->Owner->GetComponent<Material>();
        const bool savedSidedness=terrainMaterial && terrainMaterial->doubleSided;
        if (terrainMaterial) terrainMaterial->doubleSided=true;
        terrain->Owner->enabled=false;
        const auto background=capture(view+"-diagnostic-background",4);
        terrain->Owner->enabled=true;
        const auto underside=capture(view+"-diagnostic-terrain-underside",4);
        size_t undersideDifferences=0;
        for (size_t i=0;i<underside.size();++i)
            undersideDifferences+=std::abs(int(underside[i])-int(background[i]))>2;
        report << view << ": isolated underside differing channels=" << undersideDifferences << std::endl;
        Check(undersideDifferences==0,"actual oak terrain underside rendered");
        if (terrainMaterial) terrainMaterial->doubleSided=savedSidedness;
        for (auto* object:hidden) object->enabled=true;
        viewer->transform.position=savedPosition; camera->target=savedTarget;
        refreshGrass=true;
    }
    renderer.WaitIdle();
    std::cout << backend << ": actual oak meadow entrance/overlook captures and shadow comparisons passed.\n";
}

int main(int argc,char** argv)
{
    try
    {
        Check(argc==2,"expected renderer backend");
        const std::string backend=argv[1];
        if (backend=="shared-prefabs") CheckSharedPrefabLayer();
        else if (backend=="dx11") CaptureOak<DX11GameRenderer>(backend);
        else if (backend=="dx12") CaptureOak<DX12GameRenderer>(backend);
#if defined(ENGINE_VULKAN_ENABLED)
        else if (backend=="vulkan") CaptureOak<VulkanGameRenderer>(backend);
#endif
        else throw std::runtime_error("backend unavailable");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

