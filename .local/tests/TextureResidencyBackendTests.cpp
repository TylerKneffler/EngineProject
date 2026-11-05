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

using Microsoft::WRL::ComPtr;
using namespace Engine::Renderers;
using namespace Engine::Components;
void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }

template<class Renderer> void Audit(const std::string& backend, const std::string& prefix)
{
    constexpr unsigned size = 512;
    Engine::Core::Window window(GetModuleHandleW(nullptr), L"Texture residency audit", size, size);
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
    for (int i=1;i<=6;++i)
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
    auto* viewer = scene.AddObject("Viewer");
    auto* camera = viewer->AddComponent<Camera>();
    camera->target={0,0,0}; camera->useTransformRotation=false;
    report << ",\"coverage\":[";
    bool first=true;
    for (float distance : {3.f,8.f,20.f}) for (bool lod : {false,true})
    {
        viewer->transform.position={0,0,-distance};
        scene.settings.textureLod={lod,0.f,1.f,4.f,true};
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
        if constexpr (std::is_same_v<Renderer, DX11GameRenderer>)
            Check(renderer.CaptureFrameRGBA(pixels), "capture failed");
        else Check(renderer.ReadSceneCaptureRGBA(pixels), "capture failed");
        Check(pixels.size()==size*size*4, "capture size wrong");
        // Unlit leaf color is nonzero; a black background isolates mask coverage.
        size_t covered=0;
        const auto stem=prefix+"-d"+std::to_string(int(distance))+"-lod"+std::to_string(lod);
        std::ofstream mask(stem+".pgm",std::ios::binary);
        mask << "P5\n" << size << ' ' << size << "\n255\n";
        for (size_t i=0;i<pixels.size();i+=4)
        {
            const uint8_t value=(pixels[i]>5 || pixels[i+1]>5 || pixels[i+2]>5) ? 255:0;
            covered += value!=0; mask.put(char(value));
        }
        Check(covered>10, "leaf coverage empty");
        if (!first) report << ','; first=false;
        report << "{\"distance\":" << distance << ",\"lod\":" << lod << ",\"pixels\":" << covered << '}';
        std::cout << "Coverage d=" << distance << " lod=" << lod << ": " << covered << '\n';
    }
    report << "]}\n";
    renderer.WaitIdle();
}

int main(int argc, char** argv)
{
    try
    {
        Check(argc==3,"expected backend and output prefix");
        std::filesystem::create_directories(std::filesystem::path(argv[2]).parent_path());
        const std::string backend=argv[1];
        if (backend=="dx11") Audit<DX11GameRenderer>(backend,argv[2]);
        else if (backend=="dx12") Audit<DX12GameRenderer>(backend,argv[2]);
#if defined(ENGINE_VULKAN_ENABLED)
        else if (backend=="vulkan") Audit<VulkanGameRenderer>(backend,argv[2]);
#endif
        else throw std::runtime_error("backend unavailable");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
