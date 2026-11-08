#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Renderers/Vulkan/VulkanGameRenderer.h"
#include "Core/Renderers/Vulkan/VulkanGraphicsTexture.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <cmath>
#include "Core/Compoonents/Lighting/Light.h"
#include <array>

using Microsoft::WRL::ComPtr;
using namespace Engine::Renderers;
using namespace Engine::Components;
void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }

template<class Renderer> void Audit(const std::string& backend, const std::string& prefix, bool extendedOnly, bool shadowsOnly)
{
    constexpr unsigned size = 512;
    const bool materialStudy=std::getenv("ENGINE_MATERIAL_STUDY")!=nullptr;
    Engine::Core::Window window(GetModuleHandleW(nullptr), L"Texture sampling audit", size, size);
    ShowWindow(window.GetHWND(), SW_HIDE);
    Renderer renderer;
    Check(renderer.Init(window.GetHWND(), size, size), "renderer init failed");
    auto* provider = renderer.GetGraphicsProvider();
    // Initialize texture infrastructure before the DXGI budget baseline.
    uint8_t white[4] = {255,255,255,255};
    auto warm = provider->GetTextureFactory()->CreateTexture2D(1,1,white,1,
        Engine::Graphics::GraphicsTextureFormat::Rgba8,true);
    Check(bool(warm), "warmup texture failed");
    ComPtr<IDXGIAdapter3> adapter;
    if constexpr (std::is_same_v<Renderer, DX11GameRenderer>)
    {
        auto* view = static_cast<ID3D11ShaderResourceView*>(warm->GetNativeHandle());
        ComPtr<ID3D11Device> device; view->GetDevice(&device);
        ComPtr<IDXGIDevice> dxgi; Check(SUCCEEDED(device.As(&dxgi)), "DXGI device missing");
        ComPtr<IDXGIAdapter> base; dxgi->GetAdapter(&base); base.As(&adapter);
    }
    else if constexpr (std::is_same_v<Renderer, DX12GameRenderer>)
    {
        ComPtr<IDXGIFactory4> factory; CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        factory->EnumAdapterByLuid(renderer.GetDevice()->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
    }
    if (adapter)
    {
        DXGI_ADAPTER_DESC desc{}; adapter->GetDesc(&desc);
        std::wcout << L"Adapter: " << desc.Description << L'\n';
    }
    auto usage = [&] {
        uint64_t bytes = 0;
        if (adapter) for (auto segment : {DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL})
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            Check(SUCCEEDED(adapter->QueryVideoMemoryInfo(0,segment,&info)), "budget query failed");
            bytes += info.CurrentUsage;
        }
        return bytes;
    };
    renderer.WaitIdle();
    const auto before = usage();
    uint64_t allocation = 0;
    std::vector<std::shared_ptr<Texture>> textures;
    for (int i : {6})
    {
        auto texture = Texture::Acquire("Engine/Core/Assets/Models/Woodland/Oaks/Textures/Texture " +
            std::to_string(i) + " " + std::to_string(i) + ".png", i==3 || i==6);
        Check(texture->PrepareImmediate(provider), "oak texture upload failed");
        const auto* gpu = texture->GetGraphicsTexture();
        if constexpr (std::is_same_v<Renderer, DX12GameRenderer>)
        {
            auto* resource = static_cast<ID3D12Resource*>(gpu->GetNativeHandle());
            const auto desc = resource->GetDesc();
            allocation += renderer.GetDevice()->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
        }
#if defined(ENGINE_VULKAN_ENABLED)
        else if constexpr (std::is_same_v<Renderer, VulkanGameRenderer>)
            allocation += static_cast<const VulkanGraphicsTexture*>(gpu)->GetAllocationSizeBytes();
#endif
        textures.push_back(std::move(texture));
    }
    renderer.WaitIdle();
    const auto after = usage();
    std::ofstream report(prefix + ".json");
    report << "{\"backend\":\"" << backend << "\",\"allocation_bytes\":" << allocation
        << ",\"dxgi_usage_before\":" << before << ",\"dxgi_usage_after\":" << after;
    std::cout << "Dedicated allocation bytes: " << allocation << "; DXGI usage before/after: " << before << '/' << after << '\n';
    Engine::Scene::Scene scene;
    scene.Init(provider);
    scene.SetBeforeGpuResourceDestruction([&] { renderer.WaitIdle(); });
    scene.settings.skyboxEnabled = false; scene.settings.showGrid = false;
    auto* card = scene.AddObject("Oak atlas");
    std::vector<Mesh::Vertex> vertices(4);
    const float positions[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
    for (int i=0;i<4;++i)
    {
        vertices[i].pos[0]=positions[i][0]; vertices[i].pos[1]=positions[i][1];
        vertices[i].normal[2]=-1;
        if(materialStudy) { vertices[i].tangent[0]=1; vertices[i].tangent[3]=1; }
        vertices[i].uv[0]=(positions[i][0]+1)*.5f; vertices[i].uv[1]=(positions[i][1]+1)*.5f;
        for (auto& color : vertices[i].color) color=1;
    }
    auto* mesh = card->AddComponent<Mesh>();
    Check(mesh->SetIndexedGeometry(vertices,{0,1,2,0,2,3}), "geometry failed");
    mesh->CreateBuffer(provider->GetBufferFactory());
    auto* material = card->AddComponent<Material>();
    material->doubleSided=true; material->unlit=true; material->castsShadows=false;
    material->diffuseColor={1,1,1}; material->alphaMode="Mask"; material->alphaCutoff=.32f;
    material->SetBaseColorTexture("Assets/Models/Woodland/Oaks/Textures/Texture 6 6.png");
    material->PrepareGraphicsResources(provider);
    Light* materialSun=nullptr;
    if(materialStudy) {
        material->unlit=false; material->overrideTextureLod=true;
        material->SetNormalTexture("Assets/Models/Woodland/Oaks/Textures/Texture 4 4.png");
        material->SetMetallicRoughnessTexture("Assets/Models/Woodland/Oaks/Textures/Texture 5 5.png");
        material->SetOcclusionTexture("Assets/Models/Woodland/Oaks/Textures/Texture 5 5.png");
        material->roughnessFactor=.86f; material->metallicFactor=0;
        material->PrepareGraphicsResources(provider);
        for(const auto& texture:{material->normalTexture,material->metallicRoughnessTexture,material->occlusionTexture})
            Check(texture && texture->PrepareImmediate(provider),"material study map upload failed");
        scene.settings.ambientColor={.3f,.3f,.3f};
        materialSun=scene.AddObject("Material continuity sun")->AddComponent<Light>();
        materialSun->lightType=int(Light::Type::Directional);
        materialSun->intensity=2.f; materialSun->castsShadows=false;
    }
    const auto configurePolicy=[&](int candidate) {
        if(materialStudy) material->textureLod={true,3.f,8.f,0.f,candidate!=0};
        else scene.settings.textureLod={candidate!=0,3.f,8.f,4.f,false};
    };
    auto* viewer = scene.AddObject("Viewer");
    auto* camera = viewer->AddComponent<Camera>();
    camera->target={0,0,0}; camera->useTransformRotation=false;
    report << ",\"material_study\":"<<(materialStudy ? "true":"false")<<",\"coverage\":[";
    bool first=true;
    size_t referenceCoverage=0;
    std::vector<uint8_t> referenceMask;
    if (!extendedOnly) for (int foliage=0;foliage<(materialStudy ? 1:2);++foliage) for (int angle : {0,60,80})
    for (float distance : {2.9f,3.f,3.1f,5.4f,5.5f,5.6f,7.9f,8.f,8.1f}) for (int preset=0;preset<2;++preset)
    {
        const float radians=angle*3.14159265f/180.f;
        viewer->transform.position={distance*std::sin(radians),0,-distance*std::cos(radians)};
        material->shader=foliage ? "Assets/Shaders/Foliage.hlsl" : "";
        configurePolicy(preset);
        scene.settings.graphicsQuality.preset=0;
        std::vector<uint8_t> pixels;
        for (int frame=0;frame<4;++frame)
        {
            scene.AdvanceManualFrame(1.f/60);
            renderer.BeginFrame(); renderer.Clear(0,0,0,1);
            scene.PrepareRenderFrame();
            auto context=renderer.CreateFrameGraphicsContext();
            scene.Render(context.get(),1.f,camera,false,size,size);
            if constexpr (!std::is_same_v<Renderer, DX11GameRenderer>) renderer.RequestSceneCapture();
            renderer.EndFrame();
        }
        renderer.WaitIdle();
        if constexpr (std::is_same_v<Renderer, DX11GameRenderer>)
            Check(renderer.CaptureFrameRGBA(pixels), "capture failed");
        else Check(renderer.ReadSceneCaptureRGBA(pixels), "capture failed");
        Check(pixels.size()==size*size*4, "capture size wrong");
        // Unlit leaf color is nonzero; a black background isolates mask coverage.
        size_t covered=0;
        std::vector<uint8_t> currentMask;
        const auto stem=prefix+"-f"+std::to_string(foliage)+"-a"+std::to_string(angle)+"-d"+std::to_string(distance)+"-bias"+std::to_string(preset);
        std::ofstream mask(stem+".pgm",std::ios::binary);
        mask << "P5\n" << size << ' ' << size << "\n255\n";
        for (size_t i=0;i<pixels.size();i+=4)
        {
            const uint8_t value=(pixels[i]>5 || pixels[i+1]>5 || pixels[i+2]>5) ? 255:0;
            covered += value!=0; mask.put(char(value)); currentMask.push_back(value);
        }
        Check(covered>10, "leaf coverage empty");
        if (preset==0) { referenceCoverage=covered; referenceMask=currentMask; }
        else { Check(covered==referenceCoverage, "bias changed authored alpha coverage");
            Check(currentMask==referenceMask, "bias changed mask pixels"); }
        Check(material->shaderError.empty(), "material shader failed");
        std::ofstream rgb(stem+".ppm",std::ios::binary);
        rgb << "P6\n" << size << ' ' << size << "\n255\n";
        for (size_t i=0;i<pixels.size();i+=4) rgb.write(reinterpret_cast<const char*>(pixels.data()+i),3);
        if (!first) report << ','; first=false;
        report << "{\"foliage\":" << foliage << ",\"angle\":" << angle << ",\"distance\":" << distance << ",\"preset\":" << preset << ",\"pixels\":" << covered << '}';
        std::cout << "Coverage d=" << distance << " preset=" << preset << ": " << covered << '\n';
    }
    report << "]";
    if(extendedOnly) {
    auto save = [&](const std::string& stem, const std::vector<uint8_t>& pixels, bool maskOnly=false) {
        std::ofstream image(stem+(maskOnly ? ".pgm" : ".ppm"),std::ios::binary);
        image << (maskOnly ? "P5\n" : "P6\n") << size << ' ' << size << "\n255\n";
        for(size_t i=0;i<pixels.size();i+=4)
            if(maskOnly) image.put((pixels[i]>5 || pixels[i+1]>5 || pixels[i+2]>5) ? static_cast<char>(static_cast<unsigned char>(255)) : char(0));
            else image.write(reinterpret_cast<const char*>(pixels.data()+i),3);
    };
    auto capture = [&](bool shadow, bool receiverOnly) {
        renderer.WaitIdle();
        scene.AdvanceManualFrame(0.f);
        mesh->SetEditorVisible(true);
        renderer.BeginFrame();
        scene.PrepareRenderFrame();
        auto context=renderer.CreateFrameGraphicsContext();
        context->SetViewport({0,0,float(size),float(size),0,1});
        context->SetScissorRect({0,0,int(size),int(size)});
        if(shadow) {
            scene.InvalidateDirectionalShadowCache();
            scene.Render(context.get(),1.f,camera,false,size,size,true);
            renderer.EndFrame(); renderer.WaitIdle();
            mesh->SetEditorVisible(!receiverOnly);
            renderer.BeginFrame(); scene.PrepareRenderFrame();
            context=renderer.CreateFrameGraphicsContext();
            context->SetViewport({0,0,float(size),float(size),0,1});
            context->SetScissorRect({0,0,int(size),int(size)});
        }
        renderer.Clear(0,0,0,1);
        scene.Render(context.get(),1.f,camera,false,size,size);
        renderer.RequestSceneCapture(); renderer.EndFrame();
        // EndFrame submits asynchronously; mapping readback memory does not wait for its copy.
        renderer.WaitIdle();
        std::vector<uint8_t> pixels;
        Check(renderer.ReadSceneCaptureRGBA(pixels),"extended capture failed");
        Check(pixels.size()==size*size*4,"extended capture size wrong");
        mesh->SetEditorVisible(true);
        Check(material->shaderError.empty(),"extended material shader failed");
        return pixels;
    };
    const auto covered=[](const std::vector<uint8_t>& p,size_t i) {
        return p[i]>5 || p[i+1]>5 || p[i+2]>5;
    };
    report << ",\"motion\":[";
    first=true;
    mesh->foliageWindStrength=.5f;
    if(!shadowsOnly) for(int foliage=0;foliage<(materialStudy ? 1:2);++foliage) {
        material->shader=foliage ? "Assets/Shaders/Foliage.hlsl" : "";
        std::array<std::vector<uint8_t>,2> previous;
        // 120 continuous camera frames followed by 60 fixed-camera wind frames.
        for(int frame=0;frame<180;++frame) {
            const float phase=std::min(frame,119)/119.f;
            const float wave=.5f-.5f*std::cos(phase*6.2831853f);
            const float distance=frame<120 ? (materialStudy ? 1.2f+9.f*wave : 2.8f+5.4f*wave) : 5.5f;
            const float angle=frame<120 ? 60.f+20.f*wave : 80.f;
            const float radians=angle*3.14159265f/180.f;
            viewer->transform.position={distance*std::sin(radians),0,-distance*std::cos(radians)};
            scene.AdvanceManualFrame(1.f/60.f);
            if(frame%60==0) std::cout << "Motion shader=" << foliage << " frame=" << frame << std::endl;
            std::array<std::vector<uint8_t>,2> pair;
            for(int bias=0;bias<2;++bias) {
                configurePolicy(bias);
                pair[bias]=capture(false,false);
                size_t coverage=0,changed=0,unionPixels=0,common=0;
                uint64_t rgbDelta=0,pairedRgbDelta=0;
                for(size_t i=0;i<pair[bias].size();i+=4) {
                    const bool now=covered(pair[bias],i); coverage+=now;
                    if(bias==1 && now && covered(pair[0],i))
                        for(size_t c=0;c<3;++c) pairedRgbDelta+=std::abs(int(pair[1][i+c])-int(pair[0][i+c]));
                    if(!previous[bias].empty() && frame!=120) {
                        const bool old=covered(previous[bias],i);
                        changed+=old!=now; unionPixels+=old || now;
                        if(old && now) {
                            ++common;
                            for(size_t c=0;c<3;++c) rgbDelta+=std::abs(int(pair[bias][i+c])-int(previous[bias][i+c]));
                        }
                    }
                }
                Check(coverage>10,"continuous motion lost all foliage");
                if(!first) report << ','; first=false;
                report << "{\"foliage\":" << foliage << ",\"frame\":" << frame
                    << ",\"bias\":" << bias << ",\"distance\":" << distance << ",\"angle\":" << angle
                    << ",\"time\":" << scene.GetElapsedTime() << ",\"coverage\":" << coverage
                    << ",\"mask_xor\":" << changed << ",\"mask_union\":" << unionPixels
                    << ",\"common_pixels\":" << common << ",\"rgb_delta\":" << rgbDelta
                    << ",\"paired_rgb_delta\":" << pairedRgbDelta << '}';
                if(frame%6==0 || frame==119 || frame==179)
                    save(prefix+"-motion-f"+std::to_string(foliage)+"-t"+std::to_string(frame)+"-b"+std::to_string(bias),pair[bias]);
                previous[bias]=pair[bias];
            }
            for(size_t i=0;i<pair[0].size();i+=4)
                Check(covered(pair[0],i)==covered(pair[1],i),"color bias changed moving alpha coverage");
        }
    }
    // A parallel receiver behind the atlas; normal-incidence sunlight isolates its cutout footprint.
    // The optional diagonal stress case moves the receiver under a 45-degree sun.
    const bool diagonalShadow=std::getenv("ENGINE_TEXTURE_DIAGONAL_SHADOW")!=nullptr;
    const float shadowOffset=diagonalShadow ? 2.f:0.f;
    report << "],\"diagonal_shadow\":" << (diagonalShadow ? "true":"false") << ",\"shadows\":[";
    auto* receiver=scene.AddObject("Shadow receiver");
    if(materialSun) materialSun->Owner->enabled=false;
    receiver->transform.position={shadowOffset,0,2};
    receiver->transform.scale={3,3,1};
    auto* receiverMesh=receiver->AddComponent<Mesh>();
    Check(receiverMesh->SetIndexedGeometry(vertices,{0,2,1,0,3,2}),"receiver geometry failed");
    receiverMesh->CreateBuffer(provider->GetBufferFactory());
    auto* receiverMaterial=receiver->AddComponent<Material>();
    receiverMaterial->doubleSided=true; receiverMaterial->castsShadows=false;
    receiverMaterial->diffuseColor={.8f,.8f,.8f}; receiverMaterial->roughnessFactor=1;
    auto* sunObject=scene.AddObject("Audit sun");
    sunObject->transform.rotation.y=diagonalShadow ? 3.14159265f/4.f:0.f;
    auto* sun=sunObject->AddComponent<Light>();
    sun->lightType=int(Light::Type::Directional); sun->intensity=2.f;
    sun->color={1,1,1}; sun->castsShadows=true;
    sun->shadowFilterScale=0; sun->shadowDepthBias=.0002f; sun->shadowNormalBias=0;
    Engine::Model::RealtimeShadowSettings shadowSettings;
    shadowSettings.directionalCascadeCount=1; shadowSettings.directionalDistance=20;
    shadowSettings.directionalResolution=2048; shadowSettings.pcfRadius=0;
    scene.SetRealtimeShadowSettings(shadowSettings);
    scene.settings.ambientColor={.08f,.08f,.08f};
    camera->target={shadowOffset,0,2};
    material->castsShadows=true;
    // Authored independent foliage cards preserve double-sided caster state.
    mesh->prefabLodBand=0; mesh->prefabLodDistances={28.f,90.f};
    // Wind is deliberately excluded from shadow casters in the shared shader.
    mesh->foliageWindStrength=.5f;
    first=true;
    for(int foliage=0;foliage<(materialStudy ? 1:2);++foliage) for(int angle : {0,60,80})
    for(float distance : {3.f,5.5f,8.f}) {
        material->shader=foliage ? "Assets/Shaders/Foliage.hlsl" : "";
        const float radians=angle*3.14159265f/180.f;
        viewer->transform.position={shadowOffset+distance*std::sin(radians),0,2-distance*std::cos(radians)};
        std::vector<uint8_t> reference;
        for(int bias=0;bias<2;++bias) {
            configurePolicy(bias);
            material->alphaMode="Mask"; sun->castsShadows=false;
            const auto unshadowed=capture(true,true);
            sun->castsShadows=true;
            const auto masked=capture(true,true);
            scene.AdvanceManualFrame(1.f);
            const auto windShifted=capture(true,true);
            Check(masked==windShifted,"wind changed static-caster shadow receiver pixels");
            Check(scene.GetShadowDebugSnapshot().hasSelectedLight,"shadow light missing");
            Check(scene.GetShadowDebugSnapshot().submittedCasterDrawCount>0,"shadow caster missing");
            material->alphaMode="Opaque";
            const auto opaque=capture(true,true);
            material->alphaMode="Mask";
            size_t maskCount=0,opaqueCount=0,holes=0;
            std::vector<uint8_t> shadowMask(size*size);
            for(size_t i=0;i<masked.size();i+=4) {
                int maskedDelta=0,opaqueDelta=0;
                for(size_t c=0;c<3;++c) {
                    maskedDelta+=int(unshadowed[i+c])-int(masked[i+c]);
                    opaqueDelta+=int(unshadowed[i+c])-int(opaque[i+c]);
                }
                const bool m=maskedDelta>24, o=opaqueDelta>24;
                maskCount+=m; opaqueCount+=o; holes+=o && !m; shadowMask[i/4]=m ? 255:0;
            }
            if(maskCount<=10) {
                save(prefix+"-debug-off",unshadowed); save(prefix+"-debug-mask",masked); save(prefix+"-debug-opaque",opaque);
                std::cerr << "Shadow f=" << foliage << " a=" << angle << " d=" << distance << " bias=" << bias << " mask=" << maskCount << " opaque=" << opaqueCount << " holes=" << holes << std::endl;
                const auto dbg=scene.GetShadowDebugSnapshot();
                std::cerr << "Light " << dbg.directionToLight.x << "," << dbg.directionToLight.z << " radius " << dbg.cascadeRadii[0] << " split " << dbg.cascadeSplits.x << " caster " << dbg.submittedCasterDrawCount << " receiver " << dbg.dynamicReceiverCount << std::endl;
            }
            Check(maskCount>10,"rendered masked shadow empty");
            Check(holes>10 && opaqueCount>maskCount,"shadow cutout control failed to expose holes");
            if(bias==0) reference=shadowMask;
            else if(reference!=shadowMask) {
                size_t changed=0;
                for(size_t i=0;i<reference.size();++i) changed+=reference[i]!=shadowMask[i];
                save(prefix+"-bias-failure-off",unshadowed); save(prefix+"-bias-failure-mask",masked);
                std::cerr << "Shadow bias mismatch f=" << foliage << " angle=" << angle << " distance=" << distance << " changed=" << changed << " current=" << maskCount << std::endl;
                Check(false,"color bias changed rendered shadow coverage");
            }
            const auto stem=prefix+"-shadow-f"+std::to_string(foliage)+"-a"+std::to_string(angle)+"-d"+std::to_string(distance)+"-b"+std::to_string(bias);
            save(stem+"-off",unshadowed); save(stem+"-mask",masked); save(stem+"-opaque",opaque);
            std::ofstream maskFile(stem+".pgm",std::ios::binary);
            maskFile << "P5\n" << size << ' ' << size << "\n255\n";
            maskFile.write(reinterpret_cast<const char*>(shadowMask.data()),shadowMask.size());
            if(!first) report << ','; first=false;
            report << "{\"foliage\":" << foliage << ",\"angle\":" << angle << ",\"distance\":" << distance
                << ",\"bias\":" << bias << ",\"masked_pixels\":" << maskCount << ",\"opaque_pixels\":" << opaqueCount
                << ",\"cutout_holes\":" << holes << ",\"wind_changed_pixels\":0}";
        }
    }
    report << "]";
    }
    report << "}\n";
    renderer.WaitIdle();
}

int main(int argc, char** argv)
{
    try
    {
        Check(argc==3 || (argc==4 && (std::string(argv[3])=="--extended" || std::string(argv[3])=="--shadows")),"expected backend, output prefix, optional --extended or --shadows");
        std::filesystem::create_directories(std::filesystem::path(argv[2]).parent_path());
        const std::string backend=argv[1];
        if (backend=="dx11") Audit<DX11GameRenderer>(backend,argv[2],argc==4,argc==4 && std::string(argv[3])=="--shadows");
        else if (backend=="dx12") Audit<DX12GameRenderer>(backend,argv[2],argc==4,argc==4 && std::string(argv[3])=="--shadows");
#if defined(ENGINE_VULKAN_ENABLED)
        else if (backend=="vulkan") Audit<VulkanGameRenderer>(backend,argv[2],argc==4,argc==4 && std::string(argv[3])=="--shadows");
#endif
        else throw std::runtime_error("backend unavailable");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
