#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Renderers/Vulkan/VulkanGameRenderer.h"
#include <iostream>
#include <stdexcept>
using namespace Engine::Components;
using namespace Engine::Renderers;
static void Check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }

template<class Renderer> void Run()
{
    constexpr unsigned size=256;
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Screen LOD validation",size,size);
    ShowWindow(window.GetHWND(),SW_HIDE);
    Renderer renderer; Check(renderer.Init(window.GetHWND(),size,size),"renderer init");
    auto* provider=renderer.GetGraphicsProvider();
    Engine::Scene::Scene scene; scene.Init(provider);
    scene.SetBeforeGpuResourceDestruction([&] { renderer.WaitIdle(); });
    scene.settings.skyboxEnabled=false; scene.settings.showGrid=false;
    auto* object=scene.AddObject("Three resident LODs");
    auto* mesh=object->AddComponent<Mesh>();
    const auto geometry=[&](int level) {
        auto result=std::make_unique<Mesh>();
        std::vector<Mesh::Vertex> vertices(6);
        const float positions[6][2]={{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}};
        for(int i=0;i<6;++i)
        {
            vertices[i].pos[0]=positions[i][0]; vertices[i].pos[1]=positions[i][1]; vertices[i].normal[2]=-1;
            for(int channel=0;channel<3;++channel) vertices[i].color[channel]=channel==level ? 1.f:0.f;
        }
        Check(result->SetAuthoredVertices(std::move(vertices)),"geometry");
        result->CreateBuffer(provider->GetBufferFactory()); return result;
    };
    for(int level=0;level<3;++level) Check(mesh->StoreRuntimeLod(level,geometry(level)),"resident LOD");
    Check(mesh->SelectRuntimeLod(0),"initial LOD");
    mesh->screenLodSelection=true; mesh->screenLod1Pixels=100; mesh->screenLod2Pixels=30;
    mesh->gpuLod1Distance=1; mesh->gpuLod2Distance=2;
    auto* material=object->AddComponent<Material>();
    material->doubleSided=true; material->unlit=true; material->castsShadows=false;
    material->diffuseColor={1,1,1};
    material->useVertexColors=true;
    auto* viewer=scene.AddObject("Viewer"); auto* camera=viewer->AddComponent<Camera>();
    camera->target={0,0,0}; camera->useTransformRotation=false; camera->fov=60;
    const auto capture=[&](float distance,bool gpu,bool automatic,int expected,bool count=true) {
        camera->Owner->transform.position={0,0,-distance}; scene.gpuInstanceCulling=gpu; scene.automaticInstancing=automatic;
        std::vector<uint8_t> pixels;
        for(int frame=0;frame<4;++frame)
        {
            renderer.WaitIdle(); renderer.BeginFrame(); renderer.Clear(0,0,0,1);
            scene.PrepareRenderFrame(); auto context=renderer.CreateFrameGraphicsContext();
            scene.Render(context.get(),1,camera,false,size,size);
            renderer.RequestSceneCapture(); renderer.EndFrame();
        }
        Check(renderer.ReadSceneCaptureRGBA(pixels),"capture");
        const size_t center=(size*(size/2)+size/2)*4;
        std::cout << "distance=" << distance << " gpu=" << gpu << " automatic=" << automatic << " expected=" << expected
            << " rgb=" << int(pixels[center]) << ',' << int(pixels[center+1]) << ',' << int(pixels[center+2])
            << " choices=" << scene.GetScreenLodCounts()[0] << ',' << scene.GetScreenLodCounts()[1] << ',' << scene.GetScreenLodCounts()[2] << std::endl;
        Check(pixels[center+expected]>100 && pixels[center+(expected+1)%3]<30 &&
            pixels[center+(expected+2)%3]<30,"rendered geometry disagrees with selected LOD");
        if(count) Check(scene.GetScreenLodCounts()[expected]==1,"screen decision count");
    };
    for(bool gpu : {false,true}) for(bool automatic : {false,true})
    {
        capture(3,gpu,automatic,0); capture(8,gpu,automatic,1); capture(25,gpu,automatic,2);
        capture(4.5f,gpu,automatic,1); // retain coarse state within the hysteresis band
        capture(3.9f,gpu,automatic,0);
        camera->fov=30; capture(8,gpu,automatic,0); camera->fov=60;
    }
    capture(3,true,true,0);
    auto* primaryCamera=camera;
    camera=scene.AddObject("Independent view")->AddComponent<Camera>();
    camera->target={0,0,0}; camera->useTransformRotation=false; camera->fov=60;
    capture(4.5f,true,true,1);
    camera=primaryCamera;
    capture(4.5f,true,true,0); // identical projection, independent history
    mesh->distanceLodOverride=true;
    capture(3,true,true,2,false); // explicit authored distances win over projected pixels
    mesh->distanceLodOverride=false;
    // Shared instance geometry uses the same selector and still renders with
    // compute disabled. All levels deliberately share the placement array.
    auto data=std::make_shared<Mesh::InstanceData>();
    Mesh::InstanceTransform placement{};
    placement.rows[0]={1,0,0,0}; placement.rows[1]={0,1,0,0}; placement.rows[2]={0,0,1,0};
    data->transforms.push_back(placement);
    for(int level=0;level<3;++level)
    {
        auto instanced=std::make_unique<Mesh>();
        std::shared_ptr<Mesh> source=geometry(level);
        instanced->SetInstancedGeometry(source,data,{{0,6,0,1}}, {-1,-1,0},{1,1,0});
        Check(mesh->StoreRuntimeLod(level,std::move(instanced)),"instance resident LOD");
    }
    for(bool gpu : {false,true}) for(bool automatic : {false,true})
    { capture(3,gpu,automatic,0); capture(8,gpu,automatic,1); capture(25,gpu,automatic,2); }
    // Distinct unsaved instance sets both have stableId==0; their LOD history
    // and decision arrays must remain separate even when source buffers merge.
    auto secondData=std::make_shared<Mesh::InstanceData>();
    Check(data->stableId==0 && secondData->stableId==0 && data->lodIdentity!=secondData->lodIdentity,
        "transient instance identities collide");
    placement.rows[0]={.1f,0,0,.75f}; placement.rows[1]={0,.1f,0,0}; placement.rows[2]={0,0,.1f,-.1f};
    secondData->transforms.push_back(placement);
    auto* secondObject=scene.AddObject("Small independent placement set");
    auto* secondMesh=secondObject->AddComponent<Mesh>();
    secondMesh->screenLodSelection=true; secondMesh->screenLod1Pixels=100; secondMesh->screenLod2Pixels=30;
    for(int level=0;level<3;++level)
    {
        auto instanced=std::make_unique<Mesh>(); std::shared_ptr<Mesh> source=geometry(level);
        instanced->SetInstancedGeometry(source,secondData,{{0,6,0,1}}, {-1,-1,-1},{1,1,1});
        Check(secondMesh->StoreRuntimeLod(level,std::move(instanced)),"second instance resident LOD");
    }
    Check(secondMesh->SelectRuntimeLod(0),"second instance initial LOD");
    auto* secondMaterial=secondObject->AddComponent<Material>();
    secondMaterial->doubleSided=true; secondMaterial->unlit=true; secondMaterial->castsShadows=false;
    secondMaterial->useVertexColors=true; secondMaterial->diffuseColor={1,1,1};
    for(bool gpu : {false,true})
    {
        capture(3,gpu,true,0,false);
        Check(scene.GetScreenLodCounts()==std::array<uint64_t,3>{1,0,1},"placement sets share screen decisions");
    }
    renderer.WaitIdle(); scene.ClearObjects(); renderer.WaitIdle();
}
int main(int argc,char** argv)
{
    try {
        Check(argc==2,"expected backend"); const std::string backend=argv[1];
        if(backend=="dx11") Run<DX11GameRenderer>();
        else if(backend=="dx12") Run<DX12GameRenderer>();
        else if(backend=="vulkan") Run<VulkanGameRenderer>();
        else Check(false,"unknown backend");
        std::cout << "Screen LOD geometry, zoom, hysteresis, overrides and CPU/GPU paths passed.\n";
        return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
