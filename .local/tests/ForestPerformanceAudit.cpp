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
#include <cstdlib>
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


template<class Renderer> void CheckStaticGeometry(const std::string& backend)
{
    using Buffer=Engine::Graphics::IGraphicsBuffer;
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"DX12 static geometry",64,64);
    ShowWindow(window.GetHWND(),SW_HIDE);
    Renderer renderer; Check(renderer.Init(window.GetHWND(),64,64),"renderer init failed");
    auto* factory=renderer.GetGraphicsProvider()->GetBufferFactory();
    for(size_t count:{size_t(3),size_t(262144)}) {
        std::vector<uint32_t> expected(count);
        for(size_t i=0;i<count;++i) expected[i]=uint32_t(i*2654435761u)^0x15423211u;
        auto buffer=factory->CreateStaticVertexBuffer(expected.size()*4,expected.data());
        Check(buffer && buffer->GetAccessMode()==Buffer::AccessMode::Default,"static geometry remained CPU upload memory");
        if constexpr (std::is_same_v<Renderer,DX12GameRenderer>) {
            auto* native=static_cast<D3D12GraphicsBuffer*>(buffer.get());
            D3D12_HEAP_PROPERTIES heap{}; D3D12_HEAP_FLAGS flags{};
            Check(SUCCEEDED(native->GetResource()->GetHeapProperties(&heap,&flags)) && heap.Type==D3D12_HEAP_TYPE_DEFAULT,
                "static geometry is not in a default heap");
        }
#if defined(ENGINE_VULKAN_ENABLED)
        else Check(static_cast<VulkanGraphicsBuffer*>(buffer.get())->GetMappedData()==nullptr,"static geometry remained host mapped");
#endif
        auto readback=factory->CreateBuffer(Buffer::Usage::VertexBuffer,Buffer::AccessMode::Readback,expected.size()*4);
        renderer.BeginFrame(); auto context=renderer.CreateFrameGraphicsContext();
        Check(context->CopyBuffer(buffer.get(),readback.get()),"static geometry readback copy failed");
        renderer.EndFrame(); renderer.WaitIdle();
        Check(std::memcmp(readback->Map(),expected.data(),expected.size()*4)==0,"static geometry initialization corrupted");
        readback->Unmap();
        if constexpr (std::is_same_v<Renderer,DX12GameRenderer>)
            Check(static_cast<D3D12GraphicsBuffer*>(buffer.get())->GetResourceState()==D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                "readback did not restore vertex state");
    }
    Mesh editable; std::vector<Mesh::Vertex> vertices(3);
    vertices[1].pos[0]=1; vertices[2].pos[1]=1;
    for(auto& vertex:vertices) { vertex.normal[2]=1; vertex.tangent[0]=1;vertex.tangent[3]=1;for(auto& color:vertex.color)color=1; }
    Check(editable.SetAuthoredVertices(vertices),"editable fixture invalid"); editable.CreateBuffer(factory);
    Check(editable.GetGraphicsBuffer()->GetAccessMode()==Buffer::AccessMode::Default,"shared mesh did not use static storage");
    vertices[1].pos[0]=2;
    Check(editable.SetDeformedVertices(vertices),"static mesh could not become writable");
    Check(editable.GetGraphicsBuffer()->GetAccessMode()==Buffer::AccessMode::Upload,"deformed mesh did not use writable storage");
    renderer.WaitIdle();
    std::cout<<backend<<" static heap, full byte readback, resource state and editable-mesh fallback passed."<<std::endl;
}

// Controlled, static-camera diagnostic. No readback occurs in measured frames.
template<class Renderer> void Benchmark(const std::string& backend,bool foliageOnly)
{
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Forest performance audit",960,540);
    ShowWindow(window.GetHWND(),SW_HIDE);
    Renderer renderer;
    Check(renderer.Init(window.GetHWND(),960,540),"renderer init failed");
    Engine::Scene::Scene scene; scene.Init(renderer.GetGraphicsProvider());
    const bool cpuProfile=std::getenv("ENGINE_FOREST_AUDIT_CPU_PROFILE")!=nullptr;
    scene.SetUpdateProfilingEnabled(cpuProfile);
    scene.SetBeforeGpuResourceDestruction([&] { renderer.WaitIdle(); });
    Check(scene.Load("Engine/Core/Assets/Scenes/terrain/oak_meadow.scene"),"forest load failed");
    auto* viewer=scene.FindObjectByName("Meadow Explorer"); Check(viewer,"viewer missing");
    auto* camera=viewer->GetComponent<Camera>(); Check(camera,"camera missing");
    const auto entrance=viewer->transform.position;
    const auto target=camera->target;
    const auto fields=scene.GetComponents<GrassField>();
    for(auto* field:fields) { field->useEditorCamera=false; field->Start(); }
    const bool shadows=std::getenv("ENGINE_FOREST_AUDIT_SHADOWS")!=nullptr;
    if(!shadows) for(auto* light:scene.GetComponents<Light>()) light->castsShadows=false;
    auto layers=scene.GetComponents<TerrainObjectLayer>();
    TerrainObjectLayer* forest=nullptr;
    for(auto* layer:layers) if(layer->layerName=="Oak forest") forest=layer;
    Check(forest,"forest layer missing");
    // The forest is authored under the terrain. Detach it while preserving
    // its world transform, so hiding terrain does not hide the trees too.
    Check(scene.MoveObject(forest->Owner,nullptr,Engine::Scene::Scene::ObjectPlacement::AsChild),"forest detach failed");
    const auto* requestedOutput=std::getenv("ENGINE_FOREST_AUDIT_OUTPUT");
    const std::filesystem::path outputDirectory=requestedOutput ? requestedOutput:".local/forest-performance";
    const auto* requestedWidth=std::getenv("ENGINE_FOREST_AUDIT_WIDTH");
    const unsigned primaryWidth=requestedWidth ? unsigned(std::strtoul(requestedWidth,nullptr,10)):960u;
    Check(primaryWidth>=320 && primaryWidth<=3840 && primaryWidth%16==0,"invalid diagnostic width");
    std::filesystem::create_directories(outputDirectory);
    std::ofstream output(outputDirectory/(backend+".jsonl"),foliageOnly ? std::ios::app:std::ios::out);
    Check(output.good(),"cannot open diagnostic output");
    unsigned width=960,height=540;
    double advanceMs=0,beginMs=0,prepareMs=0,sceneRenderMs=0,endMs=0;
    const auto elapsed=[](auto first,auto last) {return std::chrono::duration<double,std::milli>(last-first).count();};
    bool requestCapture=false;
    auto render=[&] {
        const auto advanceBegin=std::chrono::steady_clock::now();
        scene.AdvanceManualFrame(0);
        const auto afterAdvance=std::chrono::steady_clock::now();
        renderer.BeginFrame();
        const auto afterBegin=std::chrono::steady_clock::now();
        scene.PrepareRenderFrame();
        const auto afterPrepare=std::chrono::steady_clock::now();
        auto context=renderer.CreateFrameGraphicsContext();
        context->SetViewport({0,0,float(width),float(height),0,1});
        context->SetScissorRect({0,0,int(width),int(height)});
        if(shadows) {
            scene.Render(context.get(),float(width)/height,camera,false,width,height,true);
            if constexpr (std::is_same_v<Renderer,DX12GameRenderer>) {
                renderer.EndFrame(); renderer.BeginFrame(); context=renderer.CreateFrameGraphicsContext();
                context->SetViewport({0,0,float(width),float(height),0,1});
                context->SetScissorRect({0,0,int(width),int(height)});
            }
        }
        renderer.Clear(.1f,.1f,.1f,1);
        scene.Render(context.get(),float(width)/height,camera,false,width,height);
        if(requestCapture) {
            if constexpr (!std::is_same_v<Renderer,DX11GameRenderer>) renderer.RequestSceneCapture();
        }
        const auto afterSceneRender=std::chrono::steady_clock::now();
        renderer.EndFrame();
        const auto afterEnd=std::chrono::steady_clock::now();
        advanceMs=elapsed(advanceBegin,afterAdvance); beginMs=elapsed(afterAdvance,afterBegin);
        prepareMs=elapsed(afterBegin,afterPrepare); sceneRenderMs=elapsed(afterPrepare,afterSceneRender);
        endMs=elapsed(afterSceneRender,afterEnd);
    };
    // Finish source and texture uploads before any measured comparisons.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    for(int warm=0;;++warm) {
        for(auto* field:fields) field->RefreshEditorPreview();
        render(); bool ready=true;
        for(auto* material:scene.GetComponents<Material>())
            for(const auto& texture:{material->baseColorTexture,material->normalTexture,
                material->metallicRoughnessTexture,material->occlusionTexture,material->emissiveTexture,material->heightTexture})
                if(texture && !texture->Prepare(renderer.GetGraphicsProvider())) ready=false;
        if(ready && warm>=30) break;
        Check(std::chrono::steady_clock::now()<deadline,"texture warmup timed out");
    }
    std::vector<std::pair<Engine::Core::Object*,bool>> visibility;
    for(const auto& object:scene.GetObjects()) visibility.emplace_back(object.get(),object->enabled);
    struct Capture { std::filesystem::path path; unsigned width,height; std::vector<uint8_t> pixels; };
    std::vector<Capture> captures;
    std::map<Material*,std::vector<MaterialShader>> originalShaders;
    for(auto* material:scene.GetComponents<Material>()) originalShaders[material]=material->shaders;
    auto measure=[&](const std::string& view,const std::string& mode,float lodDistance,unsigned pixelsWide) {
        const bool simplified=mode=="foliage" || mode=="trees-foliage";
        const bool fullScene=mode=="full" || mode=="foliage";
        for(auto& [material,shaders]:originalShaders) material->shaders=shaders;
        width=pixelsWide; height=width*9/16;
        renderer.WaitIdle(); renderer.Resize(width,height);
        // Override the resident draw bands without rebuilding imported source
        // geometry. This diagnostic never edits the authored scene or layer.
        for(auto [object,enabled]:visibility) object->enabled=enabled;
        scene.PrepareRenderFrame(); scene.CommitPendingObjectChanges();
        for(auto* mesh:scene.GetComponents<Mesh>()) if(mesh->IsInstanced()) {
            mesh->Owner->enabled=true;
            auto* ownerLayer=mesh->Owner->GetComponentInParent<TerrainObjectLayer>();
            if(ownerLayer==forest) {
                mesh->prefabLodDistances={28.f,lodDistance};
                mesh->MarkConfigurationDirty();
                if(simplified) if(auto* material=mesh->Owner->GetComponent<Material>())
                    if(material->doubleSided) material->shaders={{"Assets/Shaders/Foliage.hlsl",true,"Auto"}};
                bool enabled=mode!="terrain";
                // Source canopy parts use two-sided alpha-tested materials;
                // impostors are included with leaves for pixel-cost isolation.
                if(auto* material=mesh->Owner->GetComponent<Material>()) {
                    if(mode=="bark") enabled=!material->doubleSided;
                    if(mode=="leaves") enabled=material->doubleSided;
                }
                mesh->Owner->enabled=enabled;
            }
        }
        if(!fullScene) {
            for(auto* terrain:scene.GetComponents<Terrain>()) terrain->Owner->enabled=mode=="terrain";
            for(auto* field:fields) field->Owner->enabled=false;
            // Grass preview meshes are independent derived objects.
            for(auto* mesh:scene.GetComponents<Mesh>()) if(mesh->IsInstanced()) {
                auto* layer=mesh->Owner->GetComponentInParent<TerrainObjectLayer>();
                if(layer!=forest) mesh->Owner->enabled=false;
            }
        }
        for(int i=0;i<12;++i) render();
        if(mode=="trees" || mode=="bark" || mode=="leaves")
            Check(scene.GetLastRenderCpuTimings().physicalDraws>0,"isolated tree pass is empty");
        uint64_t lastGpu=renderer.GetFrameTimingTelemetry().gpuSampleId;
        for(int i=0;i<(cpuProfile?120:36);++i) {
            const auto begin=std::chrono::steady_clock::now(); render();
            const double wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            const auto& p=scene.GetLastPrepareTimings(); const auto& r=scene.GetLastRenderCpuTimings();
            const auto gpu=renderer.GetFrameTimingTelemetry();
            const bool valid=gpu.gpuTimingsValid && gpu.gpuSampleId!=lastGpu;
            if(valid) lastGpu=gpu.gpuSampleId;
            output<<"{\"backend\":\""<<backend<<"\",\"view\":\""<<view<<"\",\"mode\":\""<<mode
                <<"\",\"width\":"<<width<<",\"height\":"<<height<<",\"impostor_distance\":"<<lodDistance
                <<",\"shadows\":"<<(shadows?"true":"false")
                <<",\"frame_ms\":"<<wall
                <<",\"advance_ms\":"<<advanceMs<<",\"begin_frame_ms\":"<<beginMs
                <<",\"prepare_total_ms\":"<<prepareMs<<",\"scene_render_wall_ms\":"<<sceneRenderMs
                <<",\"end_frame_ms\":"<<endMs<<",\"prefab_ms\":"<<p.prefabInstancesMs
                <<",\"assets_ms\":"<<p.assetPublicationMs<<",\"graphics_ms\":"<<p.graphicsPreparationMs
                <<",\"commit_ms\":"<<p.objectCommitMs<<",\"native_ms\":"<<p.nativeBuffersMs
                <<",\"items_ms\":"<<p.itemsMs<<",\"render_cpu_ms\":"<<r.totalMs
                <<",\"prepare_lookup_ms\":"<<p.lookupMs
                <<",\"prepare_transforms_ms\":"<<p.transformsMs
                <<",\"prepare_build_ms\":"<<p.itemBuildMs
                <<",\"prepare_validation_ms\":"<<p.itemValidationMs
                <<",\"prepare_textures_ms\":"<<p.itemTexturesMs
                <<",\"prepare_shaders_ms\":"<<p.itemShadersMs
                <<",\"prepare_capacity_ms\":"<<p.capacityMs
                <<",\"prepare_spatial_ms\":"<<p.spatialMs
                <<",\"render_material_ms\":"<<r.materialMs
                <<",\"render_batching_ms\":"<<r.batchingMs
                <<",\"render_batch_snapshot_ms\":"<<r.batchSnapshotMs
                <<",\"render_batch_wait_ms\":"<<r.batchWaitMs
                <<",\"render_instance_tile_ms\":"<<r.instanceTilePrepareMs
                <<",\"render_culling_ms\":"<<r.cullingMs
                <<",\"render_sorting_ms\":"<<r.sortingMs
                <<",\"render_upload_ms\":"<<r.uploadMs
                <<",\"render_submission_ms\":"<<r.submissionMs
                <<",\"render_record_pack_ms\":"<<r.recordPackingMs
                <<",\"render_worker_ms\":"<<r.workerActiveMs
                <<",\"render_worker_wait_ms\":"<<r.workerWaitMs
                <<",\"render_other_ms\":"<<r.otherMs
                <<",\"render_portal_ms\":"<<r.portalMs
                <<",\"candidate_triangles\":"<<r.candidateTriangles<<",\"draws\":"<<r.physicalDraws
                <<",\"gpu_valid\":"<<(valid?"true":"false")<<",\"gpu_ms\":";
            double total=0; for(auto value:gpu.gpuMilliseconds) total+=value;
            output<<total<<"}\n";
        }
        output.flush();
        // Capture only after measurement. Readback intentionally stalls here.
        requestCapture=true;
        for(int i=0;i<3;++i) render();
        requestCapture=false;
        std::vector<uint8_t> pixels;
        bool captured;
        if constexpr (std::is_same_v<Renderer,DX11GameRenderer>) captured=renderer.CaptureFrameRGBA(pixels);
        else captured=renderer.ReadSceneCaptureRGBA(pixels);
        Check(captured && pixels.size()==size_t(width)*height*4,"capture failed");
        size_t colored=0;
        for(size_t i=0;i<pixels.size();i+=4)
            colored+=std::max({pixels[i],pixels[i+1],pixels[i+2]})-
                std::min({pixels[i],pixels[i+1],pixels[i+2]})>10;
        Check(colored>width*height/20,"capture was blank or missed the forest color pass");
        const auto name=backend+"-"+view+"-"+mode+"-"+std::to_string(int(lodDistance))+"-"+std::to_string(width);
        // Defer file creation until all cases finish. Creating images in a
        // watched project tree can otherwise trigger shader hot reload and
        // contaminate the next case with pipeline compilation/publication.
        captures.push_back({outputDirectory/(name+".ppm"),width,height,std::move(pixels)});
        std::cout<<name<<" done"<<std::endl;
    };
    // Restore authored objects separately; derived prefab pointers are transient.
    visibility.erase(std::remove_if(visibility.begin(),visibility.end(),[](auto item){return item.first->runtimeOnly;}),visibility.end());
    if(!foliageOnly) {
    for(const std::string view:{"entrance","mid","overlook"}) {
        viewer->transform.position=view=="entrance"?entrance:view=="mid"?glm::vec3(-5,3,-65):glm::vec3(-5,27,-39);
        camera->target=view=="entrance"?target:glm::vec3(2,1,5);
        for(float distance:{90.f,55.f}) if(!cpuProfile || distance==90.f) measure(view,"full",distance,primaryWidth);
    }
    viewer->transform.position=entrance; camera->target=target;
    if(!cpuProfile) for(unsigned resolution:{primaryWidth,primaryWidth/2})
        for(const std::string mode:{"full","trees","terrain","bark","leaves"}) measure("entrance",mode,90,resolution);
    }
    if(!cpuProfile) for(const std::string view:{"entrance","overlook"}) {
        viewer->transform.position=view=="entrance"?entrance:glm::vec3(-5,27,-39);
        camera->target=view=="entrance"?target:glm::vec3(2,1,5);
        for(const std::string mode:{"foliage","trees-foliage"}) measure(view,mode,55,primaryWidth);
    }
    renderer.WaitIdle();
    for(const auto& capture:captures) {
        std::ofstream image(capture.path,std::ios::binary);
        image<<"P6\n"<<capture.width<<' '<<capture.height<<"\n255\n";
        for(size_t i=0;i<capture.pixels.size();i+=4)
            image.write(reinterpret_cast<const char*>(capture.pixels.data()+i),3);
        Check(image.good(),"cannot write diagnostic image");
    }
}
int main(int argc,char** argv) {
    try {
        Check(argc==2 || (argc==3 && std::string(argv[2])=="foliage-only"),"expected backend [foliage-only]");
        const bool foliageOnly=argc==3; std::string backend=argv[1];
        if(backend=="static-buffer") CheckStaticGeometry<DX12GameRenderer>("dx12");
#if defined(ENGINE_VULKAN_ENABLED)
        else if(backend=="static-buffer-vulkan") CheckStaticGeometry<VulkanGameRenderer>("vulkan");
#endif
        else if(backend=="dx11") Benchmark<DX11GameRenderer>(backend,foliageOnly);
        else if(backend=="dx12") Benchmark<DX12GameRenderer>(backend,foliageOnly);
#if defined(ENGINE_VULKAN_ENABLED)
        else if(backend=="vulkan") Benchmark<VulkanGameRenderer>(backend,foliageOnly);
#endif
        else throw std::runtime_error("unknown backend");
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
}
