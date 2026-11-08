#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Lighting/Light.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Renderers/Vulkan/VulkanGameRenderer.h"
#include "Core/Rendering/TextureResidency.h"
#include "TextureStreamingFixtures.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace Engine::Components;
using namespace Engine::Renderers;
void Check(bool ok,const char* text){if(!ok) throw std::runtime_error(text);}

template<class Renderer> void Run(const std::string& backend,const std::filesystem::path& directory)
{
    constexpr unsigned size=256;
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Mip streaming validation",size,size);
    ShowWindow(window.GetHWND(),SW_HIDE);
    Renderer renderer;Check(renderer.Init(window.GetHWND(),size,size),"renderer init");
    auto* provider=renderer.GetGraphicsProvider();
    Engine::Scene::Scene scene;scene.Init(provider);
    int drains=0;scene.SetBeforeGpuResourceDestruction([&]{++drains;renderer.WaitIdle();});
    scene.settings.skyboxEnabled=false;scene.settings.showGrid=false;
    scene.settings.textureLod.enabled=false;
    const auto pinnedPath=std::filesystem::absolute(directory/"pinned.tga");
    WriteStreamingFixture(pinnedPath,0,128);
    auto pinned=Texture::Acquire(pinnedPath.generic_string());pinned->RetainPixelsForCpu();
    Check(pinned->PrepareImmediate(provider),"pinned texture upload");
    const uint64_t fixedBytes=provider->GetTextureFactory()->EstimateAllocationBytes(
        pinned->GetWidth(),pinned->GetHeight(),pinned->GetMipLevels(),pinned->GetFormat());
    std::vector<std::shared_ptr<Texture>> textures;
    std::vector<Material*> materials;
    const glm::vec3 centers[3]={{0,0,0},{0,0,-6},{200,0,0}};
    for(int i=0;i<3;++i)
    {
        const auto path=std::filesystem::absolute(directory/("fixture"+std::to_string(i)+".tga"));
        WriteStreamingFixture(path,i);
        auto* object=scene.AddObject("Textured cutout");object->transform.position=centers[i];
        auto* mesh=object->AddComponent<Mesh>();
        std::vector<Mesh::Vertex> vertices(4);
        const float positions[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
        for(int j=0;j<4;++j)
        {
            vertices[j].pos[0]=positions[j][0];vertices[j].pos[1]=positions[j][1];vertices[j].normal[2]=-1;
            vertices[j].uv[0]=(positions[j][0]+1)*.5f;vertices[j].uv[1]=(positions[j][1]+1)*.5f;
            for(auto& color:vertices[j].color) color=1;
        }
        Check(mesh->SetIndexedGeometry(vertices,{0,1,2,0,2,3}),"card geometry");
        mesh->CreateBuffer(provider->GetBufferFactory());
        auto* material=object->AddComponent<Material>();material->unlit=true;material->doubleSided=true;
        material->castsShadows=false;material->alphaMode="Mask";material->alphaCutoff=.5f;
        material->SetBaseColorTexture(path.generic_string());
        auto texture=material->baseColorTexture;Check(texture->PrepareImmediate(provider),"reference texture upload");
        materials.push_back(material);
        textures.push_back(std::move(texture));
    }
    auto* viewer=scene.AddObject("Streaming camera");auto* camera=viewer->AddComponent<Camera>();
    camera->useTransformRotation=false;camera->farPlane=100;
    int pose=0;
    const auto setPose=[&](int value) {
        pose=value;viewer->transform.position=centers[value]+glm::vec3(0,0,value==1?3.f:-3.f);
        camera->target=centers[value];
    };
    std::ofstream trace(directory/(backend+".jsonl"));
    unsigned frame=0;
    unsigned diagnosticWaits=0;
    const char* phase="reference";
    const auto render=[&](bool capture=false) {
        const auto started=std::chrono::steady_clock::now();
        scene.AdvanceManualFrame(1.f/60);renderer.BeginFrame();
        scene.PrepareRenderFrame();auto context=renderer.CreateFrameGraphicsContext();
        if(std::string(phase)=="offscreen_shadow")
            scene.Render(context.get(),1.f,camera,false,size,size,true);
        // Vulkan Clear begins the main render pass; depth passes precede it.
        renderer.Clear(0,0,0,1);
        scene.Render(context.get(),1.f,camera,false,size,size);
        if(capture) if constexpr(!std::is_same_v<Renderer,DX11GameRenderer>) renderer.RequestSceneCapture();
        renderer.EndFrame();
        const auto stats=Texture::GetStreamingSnapshot();
        const auto timing=renderer.GetFrameTimingTelemetry();
        const double hostMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        trace<<"{\"frame\":"<<frame++<<",\"pose\":"<<pose<<",\"budget_bytes\":"<<stats.budgetBytes
            <<",\"phase\":\""<<phase<<"\",\"diagnostic_capture\":"<<(capture?"true":"false")
            <<",\"host_frame_ms\":"<<hostMs<<",\"gpu_sample_id\":"<<timing.gpuSampleId
            <<",\"gpu_opaque_ms\":"<<(timing.gpuTimingsValid?std::to_string(timing.OpaqueMilliseconds()):"null")
            <<",\"resident_bytes\":"<<stats.allocatedBytes<<",\"requested_bytes\":"<<stats.requestedBytes
            <<",\"pinned_bytes\":"<<stats.pinnedBytes
            <<",\"staging_bytes\":"<<stats.stagingBytes<<",\"retired_bytes\":"<<stats.retiredBytes
            <<",\"upload_bytes_total\":"<<stats.uploadBytes<<",\"evictions_total\":"<<stats.evictions
            <<",\"unmet\":"<<stats.unmet<<",\"mips\":[";
        for(size_t i=0;i<textures.size();++i) trace<<(i?",":"")<<textures[i]->GetResidentFirstMip();
        trace<<"],\"demand_mips\":[";
        for(size_t i=0;i<textures.size();++i) trace<<(i?",":"")<<textures[i]->GetDemandFirstMip();
        trace<<"]}\n";
    };
    const auto capture=[&] {
        render(true);std::vector<uint8_t> pixels;
        // EndFrame submits asynchronously. Map/readback alone is not a fence.
        // This diagnostic wait is outside render timing and uninterrupted routes.
        renderer.WaitIdle();++diagnosticWaits;
        if constexpr(std::is_same_v<Renderer,DX11GameRenderer>) Check(renderer.CaptureFrameRGBA(pixels),"capture");
        else Check(renderer.ReadSceneCaptureRGBA(pixels),"capture");
        Check(pixels.size()==size*size*4,"capture size");return pixels;
    };
    std::array<std::vector<uint8_t>,3> reference;
    for(int i=0;i<3;++i)
    {
        setPose(i);for(int f=0;f<4;++f) render();reference[i]=capture();
        WriteStreamingCapture(directory/("reference-pose"+std::to_string(i)),reference[i],size);
    }
    Engine::Rendering::TextureStreamingSettings settings;
    settings.uploadBytesPerFrame=2*1048576;settings.holdFrames=12;
    std::ofstream report(directory/(backend+"-summary.json"));report<<"{\"backend\":\""<<backend<<"\",\"cases\":[";
    bool first=true;
    for(uint64_t poolBudget:{8ull*1048576,2ull*1048576,512ull*1024})
    {
        const uint64_t budget=fixedBytes+poolBudget;
        settings.budgetBytes=budget;Texture::ConfigureStreaming(settings);
        for(auto& texture:textures) texture->Reload();
        setPose(0);
        phase="warmup";
        // Async publication and decode are exercised by ordinary material prep.
        for(int f=0;f<160;++f){render();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        const auto before=Texture::GetStreamingSnapshot();
        phase="rapid_turn";
        for(int f=0;f<120;++f)
        {
            setPose((f/2)%2);render();
            const auto stats=Texture::GetStreamingSnapshot();
            Check(stats.allocatedBytes<=budget,"rapid turn exceeded GPU texture budget");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for(int target:{2,0,1})
        {
            phase="teleport_backtrack";
            setPose(target);
            for(int f=0;f<140;++f)
            {
                render();Check(Texture::GetStreamingSnapshot().allocatedBytes<=budget,"teleport exceeded budget");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto pixels=capture();
            WriteStreamingCapture(directory/("budget"+std::to_string(poolBudget)+"-pose"+std::to_string(target)),pixels,size);
            size_t intersection=0,united=0,coverage=0,baseCoverage=0;
            for(size_t p=0;p<pixels.size();p+=4)
            {
                const bool a=reference[target][p]>5 || reference[target][p+1]>5 || reference[target][p+2]>5;
                const bool b=pixels[p]>5 || pixels[p+1]>5 || pixels[p+2]>5;
                intersection+=a&&b;united+=a||b;coverage+=b;baseCoverage+=a;
            }
            Check(united>100 && coverage>100,"streamed cutout disappeared");
            const double iou=double(intersection)/united,ratio=double(coverage)/baseCoverage;
            Check(iou>=.90 && ratio>=.95 && ratio<=1.05,"streamed cutout silhouette/coverage regression");
            const auto stats=Texture::GetStreamingSnapshot();
            Check(stats.requestedBytes>0,"renderer did not collect projected demand");
            Check(stats.pinnedBytes==fixedBytes,"pinned allocation not reserved");
            if(poolBudget==512ull*1024) Check(stats.unmet>0,"constrained case did not report unmet demand");
            if(!first) report<<',';first=false;
            report<<"{\"budget_bytes\":"<<budget<<",\"pose\":"<<target<<",\"iou\":"<<iou
                <<",\"coverage_ratio\":"<<ratio<<",\"resident_bytes\":"<<stats.allocatedBytes
                <<",\"requested_bytes\":"<<stats.requestedBytes<<",\"unmet\":"<<stats.unmet<<'}';
        }
        const auto after=Texture::GetStreamingSnapshot();
        Check(after.uploadBytes>before.uploadBytes,"turn/teleport route never uploaded a replacement");
        Check(after.evictions>before.evictions,"turn/teleport route never evicted");
        Check(after.allocatedBytes<=budget,"settled budget violation");
        // A separate diagnostic replay saves transient cutouts. Readback waits
        // are outside the uninterrupted route above and excluded from timing.
        phase="diagnostic_motion";
        for(int sample=0;sample<8;++sample)
        {
            const int target=sample%2;setPose(target);
            const auto pixels=capture();
            WriteStreamingCapture(directory/("motion"+std::to_string(poolBudget)+"-sample"+std::to_string(sample)+"-pose"+std::to_string(target)),pixels,size);
            size_t intersection=0,united=0,coverage=0,baseCoverage=0;
            for(size_t pixel=0;pixel<pixels.size();pixel+=4)
            {
                const bool a=reference[target][pixel]>5 || reference[target][pixel+1]>5 || reference[target][pixel+2]>5;
                const bool b=pixels[pixel]>5 || pixels[pixel+1]>5 || pixels[pixel+2]>5;
                intersection+=a&&b;united+=a||b;coverage+=b;baseCoverage+=a;
            }
            Check(united && baseCoverage,"empty motion reference");
            Check(double(intersection)/united>=.90 && double(coverage)/baseCoverage>=.95 && double(coverage)/baseCoverage<=1.05,
                "transient streamed cutout coverage regression");
        }
    }
    settings.budgetBytes=fixedBytes+8*1048576;Texture::ConfigureStreaming(settings);
    for(auto* material:materials) material->castsShadows=true;
    auto* sun=scene.AddObject("Streaming shadow sun")->AddComponent<Light>();
    sun->lightType=int(Light::Type::Directional);sun->castsShadows=true;sun->intensity=2;
    Engine::Model::RealtimeShadowSettings shadows;
    shadows.directionalCascadeCount=1;shadows.directionalDistance=20;shadows.directionalResolution=1024;
    scene.SetRealtimeShadowSettings(shadows);setPose(0);
    phase="offscreen_shadow";
    for(int f=0;f<80;++f){render();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    const auto shadow=scene.GetShadowDebugSnapshot();
    Check(shadow.hasSelectedLight && shadow.submittedCasterDrawCount>0,"streaming shadow casters missing");
    Check(textures[1]->GetDemandFirstMip()<3,"offscreen caster lost projected shadow demand");
    // More replacements than the DX12 heap capacity; poll instead of draining.
    uint8_t white[4]={255,255,255,255};
    for(int i=0;i<1200;++i)
    {
        auto image=provider->GetTextureFactory()->CreateTexture2D(1,1,white,1,Engine::Graphics::GraphicsTextureFormat::Rgba8);
        Check(image && image->GetNativeHandle(),"texture descriptor reuse exhausted");
        provider->GetTextureFactory()->PollUploads();
    }
    Check(drains==0,"runtime texture streaming drained the GPU");
    report<<"],\"motion_comparisons\":24,\"diagnostic_gpu_waits\":"<<diagnosticWaits
        <<",\"runtime_scene_drains\":"<<drains<<",\"descriptor_replacements\":1200,\"offscreen_caster_mip\":"
        <<textures[1]->GetDemandFirstMip()<<"}\n";
    renderer.WaitIdle();provider->GetTextureFactory()->PollUploads();
    std::cout<<backend<<": turns, teleports, budgets, captures and descriptor reuse passed\n";
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==3,"expected backend and output directory");
        std::filesystem::create_directories(argv[2]);const std::string backend=argv[1];
        if(backend=="dx11") Run<DX11GameRenderer>(backend,argv[2]);
        else if(backend=="dx12") Run<DX12GameRenderer>(backend,argv[2]);
#if defined(ENGINE_VULKAN_ENABLED)
        else if(backend=="vulkan") Run<VulkanGameRenderer>(backend,argv[2]);
#endif
        else throw std::runtime_error("unsupported backend");
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
