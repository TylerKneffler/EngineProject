#include "Editor/Diagnostics/PerformanceProvenance.h"
#include "CanopyExperiment.h"
#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Terrain/Terrain.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
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
#include <future>
#include <d3d12sdklayers.h>
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
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> validation;
    if constexpr (std::is_same_v<Renderer,DX12GameRenderer>)
        if (std::getenv("ENGINE_OAK_DX12_VALIDATION"))
        {
            Check(SUCCEEDED(renderer.GetDevice()->QueryInterface(IID_PPV_ARGS(&validation))),
                "DX12 validation info queue unavailable");
            validation->ClearStoredMessages();
        }
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
    if constexpr (std::is_same_v<Renderer,DX12GameRenderer>)
    {
        struct Upload { std::vector<uint32_t> bytes; std::unique_ptr<Buffer> buffer; };
        std::vector<std::future<std::vector<Upload>>> workers;
        for (uint32_t worker=0; worker<4; ++worker)
            workers.push_back(std::async(std::launch::async, [factory,worker] {
                std::vector<Upload> uploads;
                for (uint32_t iteration=0; iteration<8; ++iteration)
                {
                    Upload upload;
                    upload.bytes.resize(3+worker*997+iteration*8191);
                    for (size_t i=0; i<upload.bytes.size(); ++i)
                        upload.bytes[i]=uint32_t(i*2654435761u)^worker^(iteration<<24);
                    upload.buffer=factory->CreateStaticVertexBuffer(upload.bytes.size()*4,upload.bytes.data());
                    uploads.push_back(std::move(upload));
                }
                return uploads;
            }));
        // Read every destination after all concurrent upload calls return. A
        // premature allocator reset or scratch reuse can corrupt earlier copies.
        std::vector<Upload> uploads;
        for (auto& worker:workers)
            for (auto& upload:worker.get()) uploads.push_back(std::move(upload));
        for (auto& upload:uploads)
        {
            auto readback=factory->CreateBuffer(Buffer::Usage::VertexBuffer,
                Buffer::AccessMode::Readback,upload.bytes.size()*4);
            renderer.BeginFrame(); auto context=renderer.CreateFrameGraphicsContext();
            Check(context->CopyBuffer(upload.buffer.get(),readback.get()),"concurrent static readback failed");
            renderer.EndFrame(); renderer.WaitIdle();
            Check(std::memcmp(readback->Map(),upload.bytes.data(),upload.bytes.size()*4)==0,
                "concurrent static uploads reused resources before completion");
            readback->Unmap();
        }
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
    if (validation)
        for (UINT64 i=0; i<validation->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes=0; validation->GetMessage(i,nullptr,&bytes);
            std::vector<unsigned char> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(validation->GetMessage(i,message,&bytes)) &&
                (message->Severity==D3D12_MESSAGE_SEVERITY_ERROR ||
                 message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION))
                throw std::runtime_error(std::string("DX12 static upload validation: ")+message->pDescription);
        }
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
    const bool materialOnly=std::getenv("ENGINE_FOREST_AUDIT_MATERIAL_ONLY")!=nullptr;
    const bool prepassOnly=std::getenv("ENGINE_FOREST_AUDIT_PREPASS_ONLY")!=nullptr;
    const auto envFlag = [](const char* name, bool fallback)
    {
        const char* value = std::getenv(name);
        if (!value || !*value) return fallback;
        const char c = value[0];
        return c != '0' && c != 'f' && c != 'F' && c != 'n' && c != 'N';
    };
    const bool terrainOcclusion = envFlag("ENGINE_VISIBILITY_TERRAIN_OCCLUSION", true);
    const bool solidOcclusion = envFlag("ENGINE_VISIBILITY_SOLID_OCCLUSION", false);
    Check(!prepassOnly || (!materialOnly && !foliageOnly && !cpuProfile),"prepass control requires an independent capture");
    const char* canopyDirectory=std::getenv("ENGINE_FOREST_AUDIT_CANOPY");
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
    if(const char* shadowRange=std::getenv("ENGINE_FOREST_AUDIT_LAYER_SHADOW_DISTANCE"))
        forest->shadowDistance=std::strtof(shadowRange,nullptr);
    const int authoredShadowLod=forest->shadowLod;
    const char* shadowLodStudy=std::getenv("ENGINE_FOREST_AUDIT_SHADOW_LOD_STUDY");
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
    bool requestCapture=false, vertexOnly=false;
    bool alphaPrepass=false;
    uint64_t prepassDraws=0;
    std::map<Material*,std::string> prepassShaders;
    Engine::Graphics::FrameTimingTelemetry shadowTelemetry;
    auto render=[&] {
        const auto advanceBegin=std::chrono::steady_clock::now();
        scene.AdvanceManualFrame(0);
        const auto afterAdvance=std::chrono::steady_clock::now();
        renderer.BeginFrame();
        const auto afterBegin=std::chrono::steady_clock::now();
        scene.PrepareRenderFrame();
        if(canopyDirectory)
            for(auto* mesh:scene.GetComponents<Mesh>())
                if(mesh->Owner->GetComponentInParent<TerrainObjectLayer>()==forest)
                    mesh->treeTimingGeometry=true;
        const auto afterPrepare=std::chrono::steady_clock::now();
        auto context=renderer.CreateFrameGraphicsContext();
        context->SetViewport({0,0,float(width),float(height),0,1});
        context->SetScissorRect({0,0,int(width),int(height)});
        if(shadows) {
            // Force the same complete cascade workload for material ablations;
            // rotating cached-cascade refresh would otherwise mix pass costs.
            if(materialOnly || canopyDirectory || prepassOnly) scene.InvalidateDirectionalShadowCache();
            scene.Render(context.get(),float(width)/height,camera,false,width,height,true);
            if constexpr (std::is_same_v<Renderer,DX12GameRenderer>) {
                renderer.EndFrame();
                shadowTelemetry=renderer.GetFrameTimingTelemetry();
                renderer.BeginFrame(); context=renderer.CreateFrameGraphicsContext();
                context->SetViewport({0,0,float(width),float(height),0,1});
                context->SetScissorRect({0,0,int(width),int(height)});
            }
        }
        renderer.Clear(.1f,.1f,.1f,1);
        prepassDraws=0;
        if(alphaPrepass) {
            for(auto& [material,shader]:prepassShaders)
                material->shader="Assets/Shaders/Diagnostics/FoliageAlphaMask.hlsl";
            scene.PrepareRenderFrame();
            scene.Render(context.get(),float(width)/height,camera,false,width,height);
            prepassDraws=scene.GetLastRenderCpuTimings().physicalDraws;
            for(auto& [material,shader]:prepassShaders) material->shader=shader;
            scene.PrepareRenderFrame();
        }
        if(vertexOnly) context->SetScissorRect({0,0,0,0});
        scene.SetDiagnosticOpaqueDepthEqual(alphaPrepass);
        scene.Render(context.get(),float(width)/height,camera,false,width,height);
        scene.SetDiagnosticOpaqueDepthEqual(false);
        if(requestCapture) {
            renderer.RequestSceneCapture();
        }
        const auto afterSceneRender=std::chrono::steady_clock::now();
        renderer.EndFrame();
        if constexpr (!std::is_same_v<Renderer,DX12GameRenderer>) shadowTelemetry=renderer.GetFrameTimingTelemetry();
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
    if(shadowLodStudy)
    {
        Check(shadows,"shadow LOD comparison requires shadows");
        width=primaryWidth; height=width*9/16;
        renderer.WaitIdle(); renderer.Resize(width,height);
        std::vector<int> variants={authoredShadowLod,-1};
        if(const char* requested=std::getenv("ENGINE_FOREST_AUDIT_SHADOW_LOD_ORDER"))
            if(requested[0]=='1') std::reverse(variants.begin(),variants.end());
        for(const int shadowLod:variants)
        {
            forest->shadowLod=shadowLod;
            forest->MarkConfigurationDirty();
            for(const std::string view:{"entrance","overlook","motion"})
            {
                const glm::dvec3 origin=view=="entrance" ? glm::dvec3(entrance):glm::dvec3(-5,27,-39);
                const glm::dvec3 aim=view=="entrance" ? glm::dvec3(target):glm::dvec3(2,1,5);
                constexpr int samples=120;
                const auto positionFrame=[&](int frame) {
                    const auto shift=view=="motion" ? glm::dvec3(3.5*std::sin(frame*6.283185307179586/samples),0,0):glm::dvec3(0);
                    viewer->transform.position=origin+shift; camera->target=aim+shift;
                };
                for(int i=0;i<samples;++i) { positionFrame(i); scene.InvalidateDirectionalShadowCache(); render(); }
                uint64_t lastGpu=renderer.GetFrameTimingTelemetry().gpuSampleId;
                uint64_t lastShadow=shadowTelemetry.gpuSampleId;
                for(int i=0;i<samples;++i)
                {
                    positionFrame(i); scene.InvalidateDirectionalShadowCache(); render();
                    const auto gpu=renderer.GetFrameTimingTelemetry();
                    const auto shadowIndex=static_cast<size_t>(Engine::Graphics::GpuTimingStage::Shadow);
                    const bool valid=gpu.gpuTimingsValid && gpu.gpuSampleId!=lastGpu;
                    const bool shadowValid=shadowTelemetry.gpuTimingsValid &&
                        shadowTelemetry.gpuSampleId!=lastShadow;
                    if(valid) lastGpu=gpu.gpuSampleId;
                    if(shadowValid) lastShadow=shadowTelemetry.gpuSampleId;
                    output<<"{"<<Engine::Editor::PerformanceProvenanceFields()<<",\"backend\":\""<<backend
                        <<"\",\"view\":\""<<view<<"\",\"shadow_lod\":"<<shadowLod
                        <<",\"shadow_distance\":"<<forest->shadowDistance<<",\"shadow_refresh\":\"moving-full-route\""
                        <<",\"frame\":"<<i<<",\"width\":"<<width<<",\"height\":"<<height
                        <<",\"x\":"<<viewer->transform.position.x<<",\"y\":"<<viewer->transform.position.y<<",\"z\":"<<viewer->transform.position.z
                        <<",\"gpu_sample_id\":"<<gpu.gpuSampleId<<",\"gpu_valid\":"<<(valid?"true":"false")
                        <<",\"shadow_valid\":"<<(shadowValid?"true":"false")
                        <<",\"shadow_workload_valid\":"<<(shadowTelemetry.gpuWorkloadValid?"true":"false")
                        <<",\"shadow_sample_id\":"<<shadowTelemetry.gpuSampleId
                        <<",\"gpu_shadow_ms\":"<<shadowTelemetry.gpuMilliseconds[shadowIndex]
                        <<",\"shadow_vs\":"<<shadowTelemetry.gpuWorkload[shadowIndex].vertices
                        <<",\"shadow_primitives\":"<<shadowTelemetry.gpuWorkload[shadowIndex].primitives
                        <<",\"shadow_ps\":"<<shadowTelemetry.gpuWorkload[shadowIndex].fragments;
                    gpu.WriteOpaqueJson(output); output<<"}\n";
                }
                output.flush();
                std::cout<<backend<<" shadowLod="<<shadowLod<<' '<<view<<" measured"<<std::endl;
            }
            if(envFlag("ENGINE_FOREST_AUDIT_SHADOW_LOD_CAPTURE",false))
            {
                for(const std::string view:{"entrance","overlook"})
                {
                    viewer->transform.position=view=="entrance"?entrance:glm::vec3(-5,27,-39);
                    camera->target=view=="entrance"?target:glm::vec3(2,1,5);
                    requestCapture=true; for(int i=0;i<3;++i) render(); requestCapture=false;
                    std::vector<uint8_t> pixels;
                    Check(renderer.ReadSceneCaptureRGBA(pixels) && pixels.size()==size_t(width)*height*4,
                        "shadow LOD image capture failed");
                    std::ofstream image(outputDirectory/(backend+"-shadow-lod-"+
                        std::to_string(shadowLod)+"-"+view+".ppm"),std::ios::binary);
                    image<<"P6\n"<<width<<' '<<height<<"\n255\n";
                    for(size_t i=0;i<pixels.size();i+=4)
                        image.write(reinterpret_cast<const char*>(pixels.data()+i),3);
                    Check(image.good(),"cannot write shadow LOD capture");
                }
            }
        }
        forest->shadowLod=authoredShadowLod;
        renderer.WaitIdle(); scene.ClearObjects(); renderer.WaitIdle(); return;
    }
    if(canopyDirectory)
    {
        Check(shadows,"canopy measurements require shadows");
        width=primaryWidth; height=width*9/16;
        renderer.WaitIdle(); renderer.Resize(width,height);
        std::vector<bool> variants={false,true};
        if(std::getenv("ENGINE_FOREST_AUDIT_REVERSE")) std::reverse(variants.begin(),variants.end());
        for(bool candidate:variants)
        {
            renderer.WaitIdle();
            Check(ConfigureCanopy(*forest,canopyDirectory,candidate,&std::cout)==4,"canopy study requires all four varieties");
            for(const std::string view:{"entrance","overlook","zoom","motion"})
            {
                const auto origin=view=="entrance" ? glm::dvec3(entrance):glm::dvec3(-5,27,-39);
                const auto aim=view=="entrance" ? glm::dvec3(target):glm::dvec3(2,1,5);
                camera->fov=view=="zoom" ? 30.f:60.f;
                constexpr int samples=180;
                const auto positionFrame=[&](int frame) {
                    const auto shift=view=="motion" ? glm::dvec3(3.5*std::sin(frame*6.283185307179586/samples),0,0):glm::dvec3(0);
                    viewer->transform.position=origin+shift; camera->target=aim+shift;
                };
                // A complete periodic warmup makes delayed asynchronous GPU
                // queries sample identical route coverage across variants.
                for(int i=0;i<samples;++i) { positionFrame(i); render(); }
                uint64_t lastGpu=renderer.GetFrameTimingTelemetry().gpuSampleId;
                uint64_t lastShadow=shadowTelemetry.gpuSampleId;
                for(int i=0;i<samples;++i)
                {
                    positionFrame(i); render();
                    const auto gpu=renderer.GetFrameTimingTelemetry();
                    const auto shadowIndex=static_cast<size_t>(Engine::Graphics::GpuTimingStage::Shadow);
                    const bool valid=gpu.gpuTimingsValid && gpu.gpuWorkloadValid && gpu.gpuSampleId!=lastGpu;
                    const bool shadowValid=shadowTelemetry.gpuTimingsValid && shadowTelemetry.gpuWorkloadValid &&
                        shadowTelemetry.gpuSampleId!=lastShadow;
                    if(valid) lastGpu=gpu.gpuSampleId;
                    if(shadowValid) lastShadow=shadowTelemetry.gpuSampleId;
                    output<<"{"<<Engine::Editor::PerformanceProvenanceFields()<<",\"backend\":\""<<backend
                        <<"\",\"view\":\""<<view<<"\",\"variant\":\""<<(candidate?"candidate":"baseline")
                        <<"\",\"frame\":"<<i<<",\"width\":"<<width<<",\"height\":"<<height
                        <<",\"distance0\":28,\"distance1\":90,\"fov\":"<<camera->fov
                        <<",\"x\":"<<viewer->transform.position.x<<",\"y\":"<<viewer->transform.position.y<<",\"z\":"<<viewer->transform.position.z
                        <<",\"gpu_sample_id\":"<<gpu.gpuSampleId<<",\"gpu_valid\":"<<(valid?"true":"false")
                        <<",\"shadow_valid\":"<<(shadowValid?"true":"false")
                        <<",\"shadow_sample_id\":"<<shadowTelemetry.gpuSampleId
                        <<",\"gpu_shadow_ms\":"<<shadowTelemetry.gpuMilliseconds[shadowIndex]
                        <<",\"shadow_vs\":"<<shadowTelemetry.gpuWorkload[shadowIndex].vertices
                        <<",\"shadow_primitives\":"<<shadowTelemetry.gpuWorkload[shadowIndex].primitives
                        <<",\"shadow_ps\":"<<shadowTelemetry.gpuWorkload[shadowIndex].fragments;
                    gpu.WriteOpaqueJson(output); output<<"}\n";
                }
                output.flush();
                std::cout<<backend<<' '<<(candidate?"candidate":"baseline")<<' '<<view<<" measured\n";
            }
        }
        renderer.WaitIdle(); scene.ClearObjects(); renderer.WaitIdle(); return;
    }
    std::vector<std::pair<Engine::Core::Object*,bool>> visibility;
    for(const auto& object:scene.GetObjects()) visibility.emplace_back(object.get(),object->enabled);
    struct Capture { std::filesystem::path path; unsigned width,height; std::vector<uint8_t> pixels; };
    std::vector<Capture> captures;
    std::map<Material*,std::string> originalShaders;
    struct Surface { bool unlit; float cutoff; };
    std::map<Material*, Surface> originalSurfaces;
    std::map<Material*, Engine::Model::TextureLodSettings> originalTextureLods;
    std::map<Material*, bool> originalTextureOverrides;
    for(auto* material:scene.GetComponents<Material>()) {
        originalShaders[material]=material->shader;
        originalSurfaces[material]={material->unlit, material->alphaCutoff};
        originalTextureLods[material]=material->textureLod;
        originalTextureOverrides[material]=material->overrideTextureLod;
    }
    auto measure=[&](const std::string& view,const std::string& mode,float lodDistance,unsigned pixelsWide) {
        vertexOnly=mode.find("-vertex-only")!=std::string::npos;
        alphaPrepass=mode=="leaves-alpha-prepass";
        prepassShaders.clear();
        const bool simplified=mode=="foliage" || mode=="trees-foliage";
        const bool fullScene=mode=="full" || mode=="foliage";
        for(auto& [material,shaders]:originalShaders) {
            material->shader=shaders;
            material->unlit=originalSurfaces[material].unlit;
            material->alphaCutoff=originalSurfaces[material].cutoff;
            material->textureLod=originalTextureLods[material];
            material->overrideTextureLod=originalTextureOverrides[material];
        }
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
                mesh->prefabLodDistances={mode=="leaves-lod1" ? 0.f : 28.f,lodDistance};
                mesh->MarkConfigurationDirty();
                if(simplified) if(auto* material=mesh->Owner->GetComponent<Material>())
                    if(material->doubleSided) material->shader="Assets/Shaders/Foliage.hlsl";
                bool enabled=mode!="terrain";
                // Source canopy parts use two-sided alpha-tested materials;
                // impostors are included with leaves for pixel-cost isolation.
                if(auto* material=mesh->Owner->GetComponent<Material>()) {
                    if(mode.rfind("material-",0)==0) {
                        material->overrideTextureLod=true;
                        // Independent controls: no mesh, alpha, wind or lighting replacement.
                        auto& lod=material->textureLod;
                        lod.enabled=true;
                        lod.nearDistance=20.f; lod.farDistance=80.f;
                        lod.mipBias=mode=="material-mips" ? 4.f : 0.f;
                        lod.simplifyShading=mode=="material-cheap";
                        lod.alphaMipBias=0.f; lod.disableAlphaAtFar=false;
                    }
                    if(mode.rfind("bark",0)==0) enabled=!material->doubleSided;
                    if(mode.rfind("leaves",0)==0) enabled=material->doubleSided;
                    if(material->doubleSided && mode=="leaves-unlit") material->unlit=true;
                    if(material->doubleSided && mode=="leaves-solid") material->alphaCutoff=0.f;
                    if(material->doubleSided && mode.rfind("leaves-alpha-mask",0)==0)
                        material->shader="Assets/Shaders/Diagnostics/FoliageAlphaMask.hlsl";
                    if(alphaPrepass && enabled) prepassShaders[material]=material->shader;
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
        size_t materialSlots=0;
        std::set<Material*> activeMaterials;
        std::set<std::string> shaderPaths;
        std::set<std::string> shaderStates;
        for(auto* mesh:scene.GetComponents<Mesh>())
            if(mesh->IsInstanced() && mesh->Owner->enabled)
                if(auto* material=mesh->Owner->GetComponent<Material>()) {
                    ++materialSlots; activeMaterials.insert(material);
                    shaderPaths.insert(material->shader.empty() ? "Object.hlsl" : material->shader);
                    shaderStates.insert(material->shader+"|"+material->alphaMode+"|"+
                        material->specularModel+"|"+std::to_string(material->doubleSided)+"|"+
                        std::to_string(material->unlit)+"|"+
                        std::to_string(bool(material->normalTexture))+"|"+
                        std::to_string(bool(material->metallicRoughnessTexture))+"|"+
                        std::to_string(bool(material->occlusionTexture)));
                }
        for(int i=0;i<32;++i) render();
        if(alphaPrepass) {
            const auto equalDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
            while(!scene.DiagnosticOpaqueDepthEqualReady()) {
                Check(std::chrono::steady_clock::now()<equalDeadline,"equal-depth pipeline activation timed out");
                render();
            }
            for(int i=0;i<32;++i) render();
        }
        if(mode=="leaves-alpha-mask") {
            // Prove the asynchronous custom pipeline is active before samples,
            // rather than timing the full-shader fallback during compilation.
            const auto readyDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
            for(;;) {
                requestCapture=true;
                for(int i=0;i<3;++i) render();
                requestCapture=false;
                std::vector<uint8_t> proof;
                bool read;
                read=renderer.ReadSceneCaptureRGBA(proof);
                size_t white=0,colored=0;
                for(size_t i=0;i+3<proof.size();i+=4) {
                    white+=proof[i]>=250 && proof[i+1]>=250 && proof[i+2]>=250;
                    colored+=std::max({proof[i],proof[i+1],proof[i+2]})-
                        std::min({proof[i],proof[i+1],proof[i+2]})>10;
                }
                for(auto* material:scene.GetComponents<Material>())
                    if(material->shader=="Assets/Shaders/Diagnostics/FoliageAlphaMask.hlsl" && !material->shaderError.empty())
                        throw std::runtime_error(material->shaderError);
                if(read && proof.size()==size_t(width)*height*4 && white>width*height/20) break;
                if(std::chrono::steady_clock::now()>=readyDeadline) {
                    std::cerr<<"alpha mask proof read="<<read<<" bytes="<<proof.size()<<" white="<<white<<" colored="<<colored<<std::endl;
                    if(proof.size()==size_t(width)*height*4) {
                        std::ofstream failed(outputDirectory/"failed-mask.ppm",std::ios::binary);
                        failed<<"P6\n"<<width<<' '<<height<<"\n255\n";
                        for(size_t i=0;i<proof.size();i+=4) failed.write(reinterpret_cast<const char*>(proof.data()+i),3);
                    }
                    Check(false,"alpha-mask shader activation timed out");
                }
            }
            for(int i=0;i<32;++i) render();
        }
        if(mode=="trees" || mode=="bark" || mode=="leaves")
            Check(scene.GetLastRenderCpuTimings().physicalDraws>0,"isolated tree pass is empty");
        uint64_t workloadSamples=0, isolatedVertices=0, isolatedFragments=0;
        uint64_t lastGpu=renderer.GetFrameTimingTelemetry().gpuSampleId;
        uint64_t lastShadow=shadowTelemetry.gpuSampleId;
        for(int i=0;i<(cpuProfile?120:36);++i) {
            const auto begin=std::chrono::steady_clock::now(); render();
            const double wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            const auto& p=scene.GetLastPrepareTimings(); const auto& r=scene.GetLastRenderCpuTimings();
            const auto gpu=renderer.GetFrameTimingTelemetry();
            const bool valid=gpu.gpuTimingsValid && gpu.gpuSampleId!=lastGpu;
            if(valid) lastGpu=gpu.gpuSampleId;
            const auto shadowIndex=static_cast<size_t>(Engine::Graphics::GpuTimingStage::Shadow);
            const bool shadowValid=shadows && shadowTelemetry.gpuTimingsValid && shadowTelemetry.gpuWorkloadValid &&
                shadowTelemetry.gpuSampleId!=lastShadow && shadowTelemetry.gpuMilliseconds[shadowIndex]>0;
            if(shadowValid) lastShadow=shadowTelemetry.gpuSampleId;
            if(valid && gpu.gpuWorkloadValid) {
                ++workloadSamples;
                const auto stage=mode.rfind("bark",0)==0 ? Engine::Graphics::GpuTimingStage::TreeBark : Engine::Graphics::GpuTimingStage::TreeLeaves;
                isolatedVertices+=gpu.gpuWorkload[static_cast<size_t>(stage)].vertices;
                isolatedFragments+=gpu.gpuWorkload[static_cast<size_t>(stage)].fragments;
            }
            output<<"{"<<Engine::Editor::PerformanceProvenanceFields()<<",\"backend\":\""<<backend<<"\",\"view\":\""<<view<<"\",\"mode\":\""<<mode
                <<"\",\"width\":"<<width<<",\"height\":"<<height<<",\"impostor_distance\":"<<lodDistance
                <<",\"shadows\":"<<(shadows?"true":"false")
                <<",\"scissor_mode\":\""<<(vertexOnly?"empty":"full")<<"\",\"route\":\"static-v2\",\"camera_position\":["<<viewer->transform.position.x<<','<<viewer->transform.position.y<<','<<viewer->transform.position.z<<']'
                <<",\"camera_target\":["<<camera->target.x<<','<<camera->target.y<<','<<camera->target.z<<']'
                <<",\"warmup_frames\":32,\"measurement_frames\":"<<(cpuProfile?120:36)
                <<",\"cpu_profile\":"<<(cpuProfile?"true":"false")
                <<",\"workload_counters_requested\":"<<(std::getenv("ENGINE_GPU_WORKLOAD_COUNTERS")?"true":"false")
                <<",\"terrain_occlusion_enabled\":"<<(terrainOcclusion?"true":"false")
                <<",\"solid_occlusion_enabled\":"<<(solidOcclusion?"true":"false")
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
                <<",\"submitted_instances\":"<<r.submittedInstances
                <<",\"submitted_triangles\":"<<r.submittedTriangles
                <<",\"terrain_occlusion_rejected\":"<<r.terrainOcclusionRejected
                <<",\"solid_occlusion_rejected\":"<<r.solidOcclusionRejected
                <<",\"prepass_draws\":"<<prepassDraws
                <<",\"total_color_draws\":"<<(r.physicalDraws+prepassDraws)
                <<",\"alpha_prepass\":"<<(alphaPrepass?"true":"false")
                <<",\"equal_depth_ready\":"<<(scene.DiagnosticOpaqueDepthEqualReady()?"true":"false")
                <<",\"full_shadow_refresh\":"<<((materialOnly || canopyDirectory || prepassOnly)?"true":"false")
                <<",\"instanced_material_slots\":"<<materialSlots
                <<",\"instanced_materials\":"<<activeMaterials.size()
                <<",\"shader_paths\":"<<shaderPaths.size()
                <<",\"material_shader_states\":"<<shaderStates.size()
                <<",\"shadow_valid\":"<<(shadowValid ? "true":"false")
                <<",\"shadow_sample_id\":"<<shadowTelemetry.gpuSampleId
                <<",\"gpu_shadow_ms\":"<<shadowTelemetry.gpuMilliseconds[shadowIndex]
                <<",\"gpu_shadow_vs_invocations\":"<<shadowTelemetry.gpuWorkload[shadowIndex].vertices
                <<",\"gpu_shadow_primitives\":"<<shadowTelemetry.gpuWorkload[shadowIndex].primitives
                <<",\"gpu_shadow_ps_invocations\":"<<shadowTelemetry.gpuWorkload[shadowIndex].fragments
                <<",\"gpu_sample_id\":"<<gpu.gpuSampleId<<",\"gpu_valid\":"<<(valid?"true":"false")<<",\"gpu_ms\":";
            double total=0; for(auto value:gpu.gpuMilliseconds) total+=value;
            output<<total; gpu.WriteOpaqueJson(output); output<<"}\n";
        }
        if (std::getenv("ENGINE_GPU_WORKLOAD_COUNTERS"))
        {
            Check(workloadSamples>0,"no completed pipeline workload samples");
            if(mode.rfind("bark",0)==0 || mode.rfind("leaves",0)==0)
                Check(isolatedVertices>0,"tree category failed to capture isolated geometry");
        }
        if(vertexOnly && std::getenv("ENGINE_GPU_WORKLOAD_COUNTERS")) {
            Check(isolatedVertices>0,"vertex-only control skipped vertex work");
            Check(isolatedFragments==0,"vertex-only control executed fragments");
        }
        output.flush();
        if(vertexOnly) { std::cout<<backend<<'-'<<mode<<" done"<<std::endl; return; }
        // Capture only after measurement. Readback intentionally stalls here.
        requestCapture=true;
        for(int i=0;i<3;++i) render();
        requestCapture=false;
        std::vector<uint8_t> pixels;
        bool captured;
        captured=renderer.ReadSceneCaptureRGBA(pixels);
        Check(captured && pixels.size()==size_t(width)*height*4,"capture failed");
        size_t colored=0;
        for(size_t i=0;i<pixels.size();i+=4)
            colored+=std::max({pixels[i],pixels[i+1],pixels[i+2]})-
                std::min({pixels[i],pixels[i+1],pixels[i+2]})>10;
        if(mode=="leaves-alpha-mask") {
            size_t white=0;
            for(size_t i=0;i<pixels.size();i+=4)
                white+=pixels[i]>=250 && pixels[i+1]>=250 && pixels[i+2]>=250;
            Check(white>width*height/20,"alpha-mask shader was inactive or capture was empty");
        } else Check(colored>width*height/20,"capture was blank or missed the forest color pass");
        const auto name=backend+"-"+view+"-"+mode+"-"+std::to_string(int(lodDistance))+"-"+std::to_string(width);
        // Defer file creation until all cases finish. Creating images in a
        // watched project tree can otherwise trigger shader hot reload and
        // contaminate the next case with pipeline compilation/publication.
        captures.push_back({outputDirectory/(name+".ppm"),width,height,std::move(pixels)});
        std::cout<<name<<" done"<<std::endl;
    };
    // Restore authored objects separately; derived prefab pointers are transient.
    visibility.erase(std::remove_if(visibility.begin(),visibility.end(),[](auto item){return item.first->runtimeOnly;}),visibility.end());
    if(prepassOnly) {
        viewer->transform.position=entrance; camera->target=target;
        for(unsigned resolution:{primaryWidth,primaryWidth/2}) {
            measure("entrance","leaves-alpha-mask",90,resolution);
            measure("entrance","leaves-alpha-mask-vertex-only",90,resolution);
            std::vector<std::string> modes={"leaves","leaves-alpha-prepass"};
            if(std::getenv("ENGINE_FOREST_AUDIT_REVERSE")) std::reverse(modes.begin(),modes.end());
            for(const auto& mode:modes) measure("entrance",mode,90,resolution);
        }
    }
    if(!prepassOnly && !foliageOnly && !materialOnly) {
    for(const std::string view:{"entrance","mid","overlook"}) {
        const char* requestedView=std::getenv("ENGINE_FOREST_AUDIT_VIEW");
        if(requestedView && view!=requestedView) continue;
        viewer->transform.position=view=="entrance"?entrance:view=="mid"?glm::vec3(-5,3,-65):glm::vec3(-5,27,-39);
        camera->target=view=="entrance"?target:glm::vec3(2,1,5);
        for(float distance:{90.f,55.f}) if(!cpuProfile || distance==90.f) measure(view,"full",distance,primaryWidth);
    }
    viewer->transform.position=entrance; camera->target=target;
    if(!cpuProfile && !std::getenv("ENGINE_FOREST_AUDIT_VIEW")) for(unsigned resolution:{primaryWidth,primaryWidth/2})
        for(const std::string mode:{"full","trees","terrain","bark","bark-vertex-only","leaves","leaves-vertex-only","leaves-unlit","leaves-solid","leaves-lod1"}) measure("entrance",mode,90,resolution);
    }
    if(!prepassOnly && !foliageOnly && !cpuProfile && !std::getenv("ENGINE_FOREST_AUDIT_VIEW")) {
        viewer->transform.position=entrance; camera->target=target;
        std::vector<std::string> modes={"material-reference","material-mips","material-cheap"};
        if(std::getenv("ENGINE_FOREST_AUDIT_REVERSE")) std::reverse(modes.begin(),modes.end());
        for(const auto& mode:modes)
            measure("entrance",mode,90,primaryWidth);
    }
    if(!prepassOnly && !cpuProfile && !materialOnly && !std::getenv("ENGINE_FOREST_AUDIT_VIEW")) for(const std::string view:{"entrance","overlook"}) {
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
        if (backend=="static-buffer" && std::getenv("ENGINE_OAK_DX12_VALIDATION"))
        {
            Microsoft::WRL::ComPtr<ID3D12Debug1> debug;
            if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            { std::cerr<<"DX12 validation unavailable\n"; return 77; }
            debug->EnableDebugLayer();
            debug->SetEnableGPUBasedValidation(TRUE);
        }
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
