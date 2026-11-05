#include "Core/Scene/Scene.h"
#include "Core/Window.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#include "Core/Renderers/Vulkan/VulkanGameRenderer.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
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

size_t CheckWinding(const Mesh* mesh)
{
    size_t surfaces=0;
    const auto& vertices=mesh->GetVertices(); const auto& indices=mesh->GetIndices();
    for (size_t i=0;i+2<indices.size();i+=3)
    {
        const auto& a=vertices.at(indices[i]); const auto& b=vertices.at(indices[i+1]);
        const auto& c=vertices.at(indices[i+2]);
        const double up=(double(b.pos[2])-a.pos[2])*(double(c.pos[0])-a.pos[0])-
            (double(b.pos[0])-a.pos[0])*(double(c.pos[2])-a.pos[2]);
        Check(up>=-1.e-7,"downward-facing terrain triangle");
        if (up>1.e-7) for (const auto* vertex : {&a,&b,&c})
        {
            const auto world=mesh->Owner->transform.GetWorldMatrixD()*glm::dvec4(
                vertex->pos[0],vertex->pos[1],vertex->pos[2],1);
            Check(std::abs(world.y-(2.+.005*world.x+.003*world.z))<1.e-4,
                "streamed/far surface deviated from shared seam height");
        }
        surfaces+=up>1.e-7; // Vertical skirts are outside the surface-winding assertion.
    }
    Check(surfaces>0,"terrain mesh has no surface triangles");
    return surfaces;
}

template<class Renderer> void Run(const std::string& backend)
{
    constexpr unsigned size=384;
    Engine::Core::Window window(GetModuleHandleW(nullptr),L"Terrain culling regression",size,size);
    ShowWindow(window.GetHWND(),SW_HIDE);
    Renderer renderer;
    Check(renderer.Init(window.GetHWND(),size,size),"renderer init failed");
    auto* provider=renderer.GetGraphicsProvider();
    Engine::Scene::Scene scene; scene.Init(provider);
    scene.SetBeforeGpuResourceDestruction([&] { renderer.WaitIdle(); });
    scene.settings.skyboxEnabled=false; scene.settings.showGrid=false;
    scene.settings.ambientColor={.04f,.04f,.04f};
    auto* manager=scene.AddObject("Streamed terrain")->AddComponent<TerrainManager>();
    manager->cellsX=manager->cellsY=8; manager->cellWidth=manager->cellDepth=1;
    manager->loadedRadius=1; manager->farRadius=1; manager->batchFarCells=false;
    manager->maxLoadsPerUpdate=64; manager->maxUploadsPerUpdate=64;
    manager->maxLodChangesPerUpdate=64; manager->maxFarRebuildMilliseconds=1000;
    manager->farVegetationImpostors=false;
    const auto cacheDirectory=".local/terrain-culling/cache-"+backend;
    std::filesystem::create_directories(cacheDirectory);
    manager->SetStaticCache(cacheDirectory,0x43554c4c);
    manager->SetProceduralBuilder([](TerrainManager::Cell cell,Terrain& terrain) {
        terrain.Resize(8,8,1,1);
        for (int z=0;z<=8;++z) for (int x=0;x<=8;++x)
            terrain.heights[z*9+x]=2.f+.005f*(cell.first*8+x)+.003f*(cell.second*8+z);
        terrain.Rebuild();
    });
    constexpr int extent=40, side=extent*2+2;
    for (int z=-extent;z<=extent;++z) for (int x=-extent;x<=extent;++x)
        Check(manager->AddProceduralCell(x,z),"procedural cell registration failed");
    std::vector<float> heights(side*side);
    for (int z=0;z<side;++z) for (int x=0;x<side;++x)
        heights[z*side+x]=2.f+.005f*((x-extent)*8)+.003f*((z-extent)*8);
    TerrainManager::FarTerrainPalette palette{};
    for (auto& color:palette.farColors) color={1,1,1};
    for (auto& color:palette.nearColors) color={1,1,1};
    manager->ConfigureFarTerrain(extent,std::move(heights),{},"",0x43554c4c,palette);
    manager->PrepareGraphicsResources(provider);
    auto* viewer=scene.AddObject("Viewer"); auto* camera=viewer->AddComponent<Camera>();
    camera->useTransformRotation=false; camera->orthographic=true;
    camera->orthographicSize=320; camera->farPlane=1000; camera->up={0,0,1};
    camera->target={4,2,4}; viewer->transform.position={4,42,4};
    auto terrainMaterials=[&] {
        std::vector<Material*> result;
        for (auto* terrain:scene.GetComponents<Terrain>())
        {
            auto* material=terrain->Owner->GetComponent<Material>();
            if (!material) material=terrain->Owner->AddComponent<Material>();
            result.push_back(material);
        }
        for (auto* mesh:manager->GetFarTileMeshes())
            if (auto* material=mesh->Owner->GetComponent<Material>()) result.push_back(material);
        return result;
    };
    auto capture=[&](bool shadows) {
        for (int frame=0;frame<4;++frame)
        {
            scene.AdvanceManualFrame(1.f/60);
            renderer.BeginFrame(); scene.PrepareRenderFrame();
            auto context=renderer.CreateFrameGraphicsContext();
            if (shadows)
            {
                context->SetViewport({0,0,float(size),float(size),0,1});
                context->SetScissorRect({0,0,int32_t(size),int32_t(size)});
                scene.Render(context.get(),1.f,camera,false,size,size,true);
                if constexpr (std::is_same_v<Renderer,DX12GameRenderer>)
                {
                    renderer.EndFrame(); renderer.BeginFrame(); context=renderer.CreateFrameGraphicsContext();
                }
            }
            context->SetViewport({0,0,float(size),float(size),0,1});
            context->SetScissorRect({0,0,int32_t(size),int32_t(size)});
            renderer.Clear(0,0,0,1);
            scene.Render(context.get(),1.f,camera,false,size,size);
            if constexpr (!std::is_same_v<Renderer,DX11GameRenderer>) renderer.RequestSceneCapture();
            renderer.EndFrame();
        }
        std::vector<uint8_t> pixels;
        if constexpr (std::is_same_v<Renderer,DX11GameRenderer>)
            Check(renderer.CaptureFrameRGBA(pixels),"capture failed");
        else Check(renderer.ReadSceneCaptureRGBA(pixels),"capture failed");
        Check(pixels.size()==size*size*4,"invalid capture size");
        return pixels;
    };
    std::filesystem::create_directories(".local/terrain-culling");
    std::ofstream report(".local/terrain-culling/"+backend+".log");
    // Cross a cell in both axes, then return so unloaded/cached geometry is reused.
    for (auto center : {TerrainManager::Cell{0,0},{1,0},{1,1},{0,0}})
    {
        bool settled=false;
        int stableIterations=0;
        for (int iteration=0;iteration<500;++iteration)
        {
            manager->UpdateStreaming(center.first,center.second);
            manager->UpdateFarTerrain(center.first,center.second,camera,size,1);
            scene.CommitPendingObjectChanges();
            // Runtime-generated components defer their GPU uploads to scene preparation.
            scene.PrepareRenderFrame();
            if (manager->GetLoadedCellCount()==9 && manager->AreDesiredFarTilesBuilt() &&
                manager->GetVisibleFarTileCount()>0 &&
                manager->CountVisibleCoverageGaps(center.first,center.second,5)==0)
            {
                // Re-evaluate far visibility after near GPU publication so a
                // temporary far underlay cannot hide the near/far seam.
                if (++stableIterations>=3) { settled=true; break; }
            }
            else stableIterations=0;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Check(settled,"streamed/far terrain did not settle");
        const auto gaps=manager->CountVisibleCoverageGaps(center.first,center.second,5);
        if (gaps)
        {
            std::cerr << "gaps=" << gaps << " near=" << manager->GetLoadedCellCount()
                << " far=" << manager->GetVisibleFarTileCount() << '\n';
            for (auto level:manager->GetFarLevelUsage()) std::cerr << "stride=" << level.stride
                << " visible=" << level.visibleTiles << '\n';
            for (int z=-2;z<=2;++z) for (int x=-2;x<=2;++x)
                if (manager->CountVisibleCoverageGaps(x,z,0)) std::cerr << "gap " << x << ',' << z << '\n';
        }
        Check(gaps==0,"far coverage gap");
        size_t triangles=0;
        for (auto* terrain:scene.GetComponents<Terrain>())
            triangles+=CheckWinding(terrain->Owner->GetComponent<Mesh>());
        for (auto* mesh:manager->GetFarTileMeshes()) triangles+=CheckWinding(mesh);
        bool coarse=false;
        for (auto level:manager->GetFarLevelUsage()) coarse |= level.visibleTiles && level.stride>1;
        Check(coarse,"fixture did not exercise coarse far LODs");
        for (auto* material:terrainMaterials())
        {
            material->unlit=true; material->diffuseColor={0,1,0};
            material->doubleSided=true; material->useVertexColors=false;
        }
        viewer->transform.position={4,42,4}; camera->target={4,2,4};
        size_t holes=0;
        for (float span : {48.f,320.f})
        {
            camera->orthographicSize=span;
            const auto top=capture(false);
            const auto beforeHoles=holes;
            for (unsigned y=32;y<size-32;++y) for (unsigned x=32;x<size-32;++x)
            {
                const auto i=(y*size+x)*4;
                // Terrain's baked palette can override the material tint.
                // Every fixture palette is nonblack; the background is black.
                holes+=!(top[i]>20 || top[i+1]>20 || top[i+2]>20);
            }
            report << "span=" << span << " holes=" << holes-beforeHoles << std::endl;
            if (holes>beforeHoles)
            {
                std::ofstream image(".local/terrain-culling/"+backend+"-seam-failure.ppm",std::ios::binary);
                image << "P6\n" << size << ' ' << size << "\n255\n";
                for (size_t i=0;i<top.size();i+=4) image.write(reinterpret_cast<const char*>(top.data()+i),3);
            }
        }
        Check(holes==0,"visible crack or culled surface across streamed/far seams");
        viewer->transform.position={4,-38,4};
        const auto bottom=capture(false);
        size_t underside=0;
        for (size_t i=0;i<bottom.size();i+=4)
            underside+=bottom[i]>20 || bottom[i+1]>20 || bottom[i+2]>20;
        Check(underside==0,"terrain underside rendered with double-sided material");
        report << "center " << center.first << ',' << center.second << ": " << triangles
            << " upward triangles; holes=" << holes << "; underside=" << underside << '\n';
    }
    Check(manager->GetCacheHitCount()>0,"return route did not restore cached terrain cells");
    report << "streamed cache hits=" << manager->GetCacheHitCount() << std::endl;
    // A lit receiver below the terrain sees shadows from its upward faces,
    // while the same terrain's underside must remain invisible to this camera.
    auto* receiver=scene.AddObject("Below-terrain shadow receiver");
    auto* receiverMesh=receiver->AddComponent<Mesh>();
    std::vector<Mesh::Vertex> receiverVertices(4);
    const float corners[4][2]={{-64,-64},{64,-64},{64,64},{-64,64}};
    for (int i=0;i<4;++i)
    {
        receiverVertices[i].pos[0]=corners[i][0]; receiverVertices[i].pos[1]=-2;
        receiverVertices[i].pos[2]=corners[i][1]; receiverVertices[i].normal[1]=1;
        for (auto& color:receiverVertices[i].color) color=1;
    }
    Check(receiverMesh->SetIndexedGeometry(receiverVertices,{0,2,1,0,3,2}),"receiver geometry failed");
    receiverMesh->CreateBuffer(provider->GetBufferFactory());
    auto* receiverMaterial=receiver->AddComponent<Material>();
    receiverMaterial->diffuseColor={1,1,1}; receiverMaterial->castsShadows=false;
    receiverMaterial->doubleSided=true;
    auto* sun=scene.AddObject("Directional sun")->AddComponent<Light>();
    sun->lightType=int(Light::Type::Directional); sun->castsShadows=true;
    sun->Owner->transform.rotation={1.3f,0,0};
    sun->shadowDepthBias=.00001f; sun->shadowNormalBias=0;
    Engine::Model::RealtimeShadowSettings settings;
    settings.directionalCascadeCount=2; settings.directionalResolution=1024;
    scene.SetRealtimeShadowSettings(settings);
    camera->orthographicSize=24; camera->farPlane=100;
    viewer->transform.position={4,0,4}; camera->target={4,-2,4};
    auto saveShadow=[&](const std::vector<uint8_t>& pixels,const std::string& name) {
        std::ofstream image(".local/terrain-culling/"+backend+"-shadow-"+name+".ppm",std::ios::binary);
        image << "P6\n" << size << ' ' << size << "\n255\n";
        for (size_t i=0;i<pixels.size();i+=4) image.write(reinterpret_cast<const char*>(pixels.data()+i),3);
    };
    auto materials=terrainMaterials();
    materials.erase(std::remove(materials.begin(),materials.end(),receiverMaterial),materials.end());
    for (auto* material:materials) material->castsShadows=false;
    const auto colorOnly=capture(false);
    saveShadow(colorOnly,"color-only");
    const auto unshadowed=capture(true);
    const auto& shadowDebug=scene.GetShadowDebugSnapshot();
    report << "lights=" << scene.GetLastRealtimeLightCount() << " selected=" << shadowDebug.hasSelectedLight
        << " light direction=" << shadowDebug.directionToLight.x << ',' << shadowDebug.directionToLight.y
        << ',' << shadowDebug.directionToLight.z << std::endl;
    saveShadow(unshadowed,"reference");
    std::vector<uint8_t> shadowed;
    size_t darker=0;
    for (int mode : {0,1,2})
    {
        for (auto* material:materials)
        {
            const bool nearCell=material->Owner->GetComponent<Terrain>()!=nullptr;
            material->castsShadows=mode==2 || (mode==0 ? nearCell : !nearCell);
        }
        shadowed=capture(true); darker=0;
        saveShadow(shadowed,std::to_string(mode));
        for (size_t i=0;i<shadowed.size();i+=4)
            darker+=int(unshadowed[i])+int(unshadowed[i+1])+int(unshadowed[i+2]) >
                int(shadowed[i])+int(shadowed[i+1])+int(shadowed[i+2])+15;
        report << "directional shadow mode=" << mode << " darkened pixels=" << darker << std::endl;
        Check(scene.GetShadowDebugSnapshot().submittedCasterDrawCount>0,"terrain shadow casters not submitted");
        Check(darker>1000,"near/far upward terrain did not cast directional shadows onto lower receiver");
    }
    const auto repeated=capture(true);
    size_t changed=0;
    for (size_t i=0;i<shadowed.size();++i) changed+=std::abs(int(shadowed[i])-int(repeated[i]))>2;
    Check(changed<32,"directional terrain shadows flickered across reused frames");
    renderer.WaitIdle();
    std::cout << backend << ": streamed/far seams, top-only culling and directional shadows passed ("
        << darker << " shadow pixels).\n";
}

int main(int argc,char** argv)
{
    try
    {
        Check(argc==2,"expected renderer backend");
        const std::string backend=argv[1];
        if (backend=="dx11") Run<DX11GameRenderer>(backend);
        else if (backend=="dx12") Run<DX12GameRenderer>(backend);
#if defined(ENGINE_VULKAN_ENABLED)
        else if (backend=="vulkan") Run<VulkanGameRenderer>(backend);
#endif
        else throw std::runtime_error("backend unavailable");
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

