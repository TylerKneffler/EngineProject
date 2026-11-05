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


template<class Renderer> void Run(const std::string& backend)
{
    constexpr unsigned width=512,height=512;
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Tree LOD regression",width,height);
    ShowWindow(window.GetHWND(),SW_HIDE);
    Renderer renderer;
    Check(renderer.Init(window.GetHWND(),width,height),"renderer init failed");
    Engine::Scene::Scene scene; scene.Init(renderer.GetGraphicsProvider());
    scene.SetBeforeGpuResourceDestruction([&] { renderer.WaitIdle(); });
    scene.settings.skyboxEnabled=false;scene.settings.ambientColor={.04f,.04f,.04f};
    Engine::Model::RealtimeShadowSettings shadows;shadows.directionalDistance=60;shadows.directionalResolution=512;shadows.directionalCascadeCount=1;
    scene.SetRealtimeShadowSettings(shadows);
    auto* viewer=scene.AddObject("Viewer"); auto* camera=viewer->AddComponent<Camera>();
    camera->target={0,1,0};camera->useTransformRotation=false;
    auto instances=std::make_shared<Mesh::InstanceData>();
    Mesh::InstanceTransform transform{};
    transform.rows[0]={1,0,0,0};transform.rows[1]={0,1,0,0};transform.rows[2]={0,0,1,0};
    instances->transforms.push_back(transform);
    std::vector<Mesh::Vertex> vertices(3);
    vertices[0].pos[0]=-2;vertices[1].pos[0]=2;vertices[2].pos[1]=4;
    for (auto& vertex:vertices) { vertex.normal[2]=-1;vertex.tangent[0]=1;vertex.tangent[3]=1;for(auto& c:vertex.color)c=1; }
    auto source=std::make_shared<Mesh>();
    Check(source->SetAuthoredVertices(vertices),"fixture geometry invalid");
    source->CreateBuffer(renderer.GetGraphicsProvider()->GetBufferFactory());
    std::vector<Mesh*> bands;
    for(int lod=0;lod<3;++lod)
    {
        auto* object=scene.AddObject("LOD "+std::to_string(lod));
        auto* mesh=object->AddComponent<Mesh>(); bands.push_back(mesh);
        mesh->SetInstancedGeometry(source,instances,{{0,3,0,1}},{-2,0,0},{2,4,0});
        mesh->prefabLodBand=lod;mesh->prefabLodDistances={12,24};
        mesh->useBoundsForShadowCulling=true;
        auto* material=object->AddComponent<Material>();
        material->doubleSided=true;material->diffuseColor={0,0,0};material->diffuseColor[lod]=1;material->ambientColor=material->diffuseColor;
        material->emissiveColor=glm::vec3(0);material->emissiveColor[lod]=1;
    }
    auto* light=scene.AddObject("Sun")->AddComponent<Light>();
    light->lightType=int(Light::Type::Directional);light->castsShadows=true;
    light->Owner->transform.rotation={.8f,.8f,0};
    light->shadowDepthBias=.00001f;light->shadowNormalBias=0;light->intensity=1;
    auto* ground=scene.AddObject("Shadow receiver");
    auto* groundMesh=ground->AddComponent<Mesh>();
    std::vector<Mesh::Vertex> floor(6);
    const glm::vec3 points[]={{-50,-.1f,-50},{50,-.1f,50},{50,-.1f,-50},{-50,-.1f,-50},{-50,-.1f,50},{50,-.1f,50}};
    for(int i=0;i<6;++i)
    {
        for(int axis=0;axis<3;++axis) floor[i].pos[axis]=points[i][axis];
        floor[i].normal[1]=1;floor[i].tangent[0]=1;floor[i].tangent[3]=1;
        for(auto& color:floor[i].color) color=1;
    }
    Check(groundMesh->SetAuthoredVertices(floor),"receiver geometry invalid");
    groundMesh->CreateBuffer(renderer.GetGraphicsProvider()->GetBufferFactory());
    auto* groundMaterial=ground->AddComponent<Material>();
    groundMaterial->diffuseColor={.8f,.8f,.8f};groundMaterial->doubleSided=true;
    groundMaterial->castsShadows=false;light->color={1,1,1};
    const auto render=[&] {
        scene.AdvanceManualFrame(0);
        renderer.BeginFrame(); scene.PrepareRenderFrame();
        auto context=renderer.CreateFrameGraphicsContext();
        context->SetViewport({0,0,float(width),float(height),0,1});
        context->SetScissorRect({0,0,int(width),int(height)});
        scene.Render(context.get(),1,camera,false,width,height,true);
        if constexpr (std::is_same_v<Renderer,DX12GameRenderer>)
        {renderer.EndFrame();renderer.BeginFrame();context=renderer.CreateFrameGraphicsContext();}
        context->SetViewport({0,0,float(width),float(height),0,1});
        context->SetScissorRect({0,0,int(width),int(height)});
        renderer.Clear(0,0,0,1);scene.Render(context.get(),1,camera,false,width,height);
        if constexpr (!std::is_same_v<Renderer,DX11GameRenderer>) renderer.RequestSceneCapture();
        renderer.EndFrame();
    };
    for (const auto distance : {6.f,18.f,36.f,18.f,6.f,11.9f,12.1f,23.9f,24.1f})
    {
        viewer->transform.position={0,1,-distance};
        for(int frame=0;frame<6;++frame) render();
        std::vector<uint8_t> pixels;
        if constexpr (std::is_same_v<Renderer,DX11GameRenderer>) Check(renderer.CaptureFrameRGBA(pixels),"capture failed");
        else Check(renderer.ReadSceneCaptureRGBA(pixels),"capture failed");
        size_t colors[3]{};
        for(size_t pixel=0;pixel<pixels.size();pixel+=4)
            for(int channel=0;channel<3;++channel)
                if(pixels[pixel+channel]>40 && pixels[pixel+channel]>pixels[pixel+(channel+1)%3]+20 &&
                    pixels[pixel+channel]>pixels[pixel+(channel+2)%3]+20) ++colors[channel];
        const int expected=distance<12 ? 0:distance<24 ? 1:2;
        std::cout << backend << " distance=" << distance << " RGB pixels=" << colors[0] << ',' << colors[1] << ',' << colors[2] << std::endl;
        if (!colors[expected])
        {
            std::filesystem::create_directories(".local/tree-lod");
            std::ofstream out(".local/tree-lod/"+backend+"-failure.ppm",std::ios::binary);
            out << "P6\n512 512\n255\n";
            for(size_t i=0;i<pixels.size();i+=4) out.write(reinterpret_cast<const char*>(pixels.data()+i),3);
            for(auto* material:scene.GetComponents<Material>()) std::cout << "shader: " << material->shaderError << std::endl;
            std::cout << "GPU batches=" << scene.GetLastRenderCpuTimings().gpuBatches << std::endl;
        }
        Check(colors[expected]>10,"camera-selected LOD missing");
        if(distance==6 || distance==18 || distance==36)
            for(int channel=0;channel<3;++channel) if(channel!=expected)
                Check(colors[channel]==0,"wrong LOD visible away from fade");
        Check(scene.GetShadowDebugSnapshot().hasSelectedLight,"directional shadow light missing");
        if (distance<24) Check(scene.GetShadowDebugSnapshot().submittedCasterDrawCount>0,"directional LOD caster missing");
        Check(scene.GetLastRenderCpuTimings().gpuBatches>0,"GPU placement selection not dispatched");
        for(auto* mesh:bands) Check(mesh->GetInstanceData()==instances && mesh->GetGraphicsBuffer()==source->GetGraphicsBuffer(),"camera movement replaced resident data");
    }
    viewer->transform.position={0,8,-18};camera->target={0,1,0};camera->useTransformRotation=false;
    const auto shadowCapture=[&](bool enabled) {
        light->castsShadows=enabled;
        for(int frame=0;frame<6;++frame) render();
        std::vector<uint8_t> pixels;
        if constexpr (std::is_same_v<Renderer,DX11GameRenderer>) Check(renderer.CaptureFrameRGBA(pixels),"shadow capture failed");
        else Check(renderer.ReadSceneCaptureRGBA(pixels),"shadow capture failed");
        return pixels;
    };
    const auto unshadowed=shadowCapture(false),shadowed=shadowCapture(true);
    size_t darker=0;
    for(size_t i=0;i<shadowed.size();i+=4)
        darker+=int(unshadowed[i])+int(unshadowed[i+1])+int(unshadowed[i+2])>
            int(shadowed[i])+int(shadowed[i+1])+int(shadowed[i+2])+20;
    std::filesystem::create_directories(".local/tree-lod");
    for (int mode=0;mode<2;++mode)
    {
        const auto& pixels=mode ? shadowed:unshadowed;
        std::ofstream out(".local/tree-lod/"+backend+(mode ? "-shadow-on.ppm":"-shadow-off.ppm"),std::ios::binary);
        out<<"P6\n512 512\n255\n";
        for(size_t i=0;i<pixels.size();i+=4) out.write(reinterpret_cast<const char*>(pixels.data()+i),3);
    }
    std::cout<<backend<<" directional shadow darkened pixels="<<darker<<std::endl;
    Check(darker>10,"resident LOD produced no visible directional shadow");
    // Two visible placements in one tile must choose different levels. Moving
    // the camera must change the first placement without rebuilding either one.
    auto second=transform;second.rows[0].w=10;second.rows[2].w=20;
    instances->transforms.push_back(second);instances->MarkDirty();
    for(auto* mesh:bands) mesh->SetInstancedGeometry(source,instances,{{0,3,0,2}},{-2,0,0},{12,4,20});
    camera->target={0,1,0};
    for(float distance:{6.f,18.f})
    {
        viewer->transform.position={0,1,-distance};
        for(int frame=0;frame<6;++frame) render();
        std::vector<uint8_t> pixels;
        if constexpr (std::is_same_v<Renderer,DX11GameRenderer>) Check(renderer.CaptureFrameRGBA(pixels),"mixed capture failed");
        else Check(renderer.ReadSceneCaptureRGBA(pixels),"mixed capture failed");
        size_t colors[3]{};
        for(size_t i=0;i<pixels.size();i+=4) for(int c=0;c<3;++c)
            if(pixels[i+c]>40 && pixels[i+c]>pixels[i+(c+1)%3]+20 && pixels[i+c]>pixels[i+(c+2)%3]+20) ++colors[c];
        Check(colors[distance<12 ? 0:1]>10 && colors[2]>10,"same-tile placements did not select independent LODs");
        Check(scene.GetLastRenderCpuTimings().grassPlacementUploadBytes==0,"camera movement reuploaded placement records");
    }
    std::cout<<backend<<" independent same-tile LODs and resident placement reuse passed."<<std::endl;
    renderer.WaitIdle();
}
int main(int argc,char** argv)
{
    try
    {
        Check(argc==2,"expected backend");const std::string backend=argv[1];
        if(backend=="dx11") Run<DX11GameRenderer>(backend);
        else if(backend=="dx12") Run<DX12GameRenderer>(backend);
#if defined(ENGINE_VULKAN_ENABLED)
        else if(backend=="vulkan") Run<VulkanGameRenderer>(backend);
#endif
        else throw std::runtime_error("backend unavailable");
        return 0;
    }
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}


