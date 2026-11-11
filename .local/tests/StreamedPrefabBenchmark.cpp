#include "Core/Scene/Scene.h"
#ifdef _WIN32
#include "Core/Window.h"
#include "Core/Renderers/DX12/DX12GameRenderer.h"
#endif
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Serialization/Json.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Rendering/SharedMeshBuffers.h"
#include "Core/Serialization/SceneSerializer.h"
#include <chrono>
#include <thread>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace Engine::Components;
using namespace Engine::Graphics;
struct Buffer : IGraphicsBuffer {
 Usage usage; AccessMode access; uint32_t stride; std::vector<unsigned char> bytes;
 Usage GetUsage() const override{return usage;} AccessMode GetAccessMode() const override{return access;}
 uint64_t GetSize() const override{return bytes.size();} uint32_t GetElementStride() const override{return stride;}
 void* Map() override{return bytes.data();} void Unmap() override{} void* GetNativeHandle() const override{return const_cast<Buffer*>(this);}
};
struct Provider : IGraphicsProvider, IGraphicsBufferFactory {
 unsigned allocations=0; IShaderCompiler* GetShaderCompiler() override{return nullptr;}
 IGraphicsBufferFactory* GetBufferFactory() override{return this;}
 IPipelineStateFactory* GetPipelineStateFactory() override{return nullptr;}
 IGraphicsContextFactory* GetContextFactory() override{return nullptr;}
 std::unique_ptr<IGraphicsBuffer> CreateBuffer(IGraphicsBuffer::Usage u, IGraphicsBuffer::AccessMode a,uint64_t size,const void* data,uint32_t stride) override {
  auto b=std::make_unique<Buffer>();b->usage=u;b->access=a;b->stride=stride;b->bytes.resize(size); if(data)std::memcpy(b->bytes.data(),data,size);++allocations;return b; }
};
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(int argc,char**argv){try{
 bool realGpu=false, oak=false, dense=false;
 for(int i=1;i<argc;++i){const std::string arg=argv[i];if(arg=="--dx12")realGpu=true;else if(arg=="--oak")oak=true;else if(arg=="--dense")dense=true;else throw std::runtime_error("unknown benchmark option: "+arg);}
 oak=oak || realGpu;
 const int placementsPerAsset=dense ? 512 : 1;
 std::filesystem::create_directories(".local/tests-generated/prefab-stream-benchmark");
 std::vector<std::string> assets;
 if(oak) for(const char* name:{"Big_oak_tree_001","Medium_oak_tree_001","Small_oak_tree_001"}) assets.push_back(std::string("Assets/Prefabs/Woodland/Oaks/")+name+"_LOD0.prefab");
 else {
  const std::string mesh=".local/tests-generated/prefab-stream-benchmark/fixture.mesh";
  std::vector<Mesh::Vertex> vertices(3); vertices[1].pos[0]=1;vertices[2].pos[1]=1;
  for(auto& v:vertices){v.normal[2]=1; for(auto& c:v.color)c=1;}
  require(Mesh::SaveNativeFile(mesh,vertices),"could not save mesh fixture");
  for(int i=0;i<3;++i){std::string p=".local/tests-generated/prefab-stream-benchmark/fixture"+std::to_string(i)+".prefab";
   std::ofstream out(p);out<<"<Prefab xmlns:components=\"urn:engine-components\" version=\"1\" type=\"prefab\"><object name=\"tree\"><components><components:Mesh Mesh.file=\""<<mesh<<"\"/></components><children/></object></Prefab>";assets.push_back(p);}
 }
 if(!oak) {
  Engine::Scene::Scene normalScene;auto* normal=Engine::Serialization::SceneSerializer::InstantiatePrefab(normalScene,assets[0],nullptr);
  require(normal && !normal->PrefabSourceSnapshot.empty(),"normal prefab lost edit baseline");
  Engine::Scene::Scene prototypeScene;const auto document=Engine::Serialization::SceneSerializer::ReadPrefabFile(assets[0]);
  auto* prototype=Engine::Serialization::SceneSerializer::InstantiatePrefabFromValue(prototypeScene,assets[0],document,nullptr,false);
  require(prototype && prototype->PrefabSourceSnapshot.empty() && !prototype->GetComponentsInChildren<Mesh>().empty(),"read-only prototype failed");
 }
 {
  Provider p;std::vector<unsigned char> bytes(66,0x5a);
  for(size_t n=1;n<=63;++n){auto prepared=Engine::Rendering::PrepareMeshBuffer(bytes.data()+1,n);
   auto a=Engine::Rendering::AcquireMeshBuffer(&p,IGraphicsBuffer::Usage::VertexBuffer,bytes.data()+1,n,1,&prepared);
   auto b=Engine::Rendering::AcquireMeshBuffer(&p,IGraphicsBuffer::Usage::VertexBuffer,bytes.data()+1,n,1);
   require(a && a==b,"prepared buffer did not share exact bytes");
   bytes[n]^=1;auto changed=Engine::Rendering::PrepareMeshBuffer(bytes.data()+1,n);
   auto c=Engine::Rendering::AcquireMeshBuffer(&p,IGraphicsBuffer::Usage::VertexBuffer,bytes.data()+1,n,1,&changed);
   require(c && c!=a && std::memcmp(static_cast<Buffer*>(c.get())->bytes.data(),bytes.data()+1,n)==0,"prepared buffer corrupted data");bytes[n]^=1;
  }
 }
 Provider provider;
#ifdef _WIN32
 std::unique_ptr<Engine::Core::Window> window;
 std::unique_ptr<Engine::Renderers::DX12GameRenderer> renderer;
 if(realGpu){window=std::make_unique<Engine::Core::Window>(GetModuleHandleW(nullptr),L"Streamed prefab benchmark",640,360);
  renderer=std::make_unique<Engine::Renderers::DX12GameRenderer>();require(renderer->Init(window->GetHWND(),640,360),"DX12 renderer initialization failed");}
#else
 require(!realGpu,"DX12 benchmark requires Windows");
#endif
 IGraphicsProvider* activeProvider=&provider;
#ifdef _WIN32
 if(renderer)activeProvider=renderer->GetGraphicsProvider();
#endif
 Engine::Scene::Scene scene;
#ifdef _WIN32
 if(renderer){scene.Init(activeProvider);scene.SetBeforeGpuResourceDestruction([&]{renderer->WaitIdle();});}
#endif
 auto makeLayer=[&](const char* name){auto* o=scene.AddObject(name);auto* l=o->AddComponent<TerrainObjectLayer>();
  for(int i=0;i<3;++i){for(int n=0;n<placementsPerAsset;++n){TerrainObjectLayer::Placement p;p.asset=assets[i];p.position={float(i*3+(n%32)*3),0,float((n/32)*3)};p.id=i*placementsPerAsset+n+1;l->placements.push_back(p);}
   if(oak){auto lod1=assets[i],lod2=assets[i];const auto marker=assets[i].find("_LOD0.prefab");require(marker!=std::string::npos,"oak LOD path missing");lod1.replace(marker,5,"_LOD1");lod2.replace(marker,5,"_LOD2");l->objectLods[assets[i]]={{assets[i],lod1,lod2},{},{28.f,90.f}};}}
  l->MarkConfigurationDirty();return l;};
 auto run=[&](TerrainObjectLayer* layer,const char* phase){auto wallStart=std::chrono::steady_clock::now();double total=0,maximum=0,maxPrepare=0,maxCommit=0,maxSignature=0;int steps=0;
  for(int i=0;i<10000;++i){auto start=std::chrono::steady_clock::now();layer->PreparePrefabInstances(activeProvider);auto prepared=std::chrono::steady_clock::now();scene.CommitPendingObjectChanges();auto committed=std::chrono::steady_clock::now();double ms=std::chrono::duration<double,std::milli>(committed-start).count();maxPrepare=std::max(maxPrepare,std::chrono::duration<double,std::milli>(prepared-start).count());maxCommit=std::max(maxCommit,std::chrono::duration<double,std::milli>(committed-prepared).count());maxSignature=std::max(maxSignature,layer->GetLastPrefabPrepareTimings().placementSignatureMs);total+=ms;maximum=std::max(maximum,ms);++steps;if(!layer->IsPrefabPreparationPending())break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  require(!layer->IsPrefabPreparationPending(),"prefab preparation timed out");
  std::cout<<phase<<" wall_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wallStart).count()<<" total_ms="<<total<<" max_step_ms="<<maximum<<" max_prepare_ms="<<maxPrepare<<" max_commit_ms="<<maxCommit<<" max_signature_ms="<<maxSignature<<" steps="<<steps<<(realGpu ? " buffers=dx12" : " mock_allocations="+std::to_string(provider.allocations))<<'\n';};
 auto* cold=makeLayer("cold");run(cold,"cold");
 auto meshes=scene.GetComponents<Mesh>();size_t coldCount=0;for(auto* m:meshes)if(m->Owner->Parent==cold->Owner && m->IsInstanced()){require(m->Owner->IsEnabledInHierarchy(),"completed cold batch remains hidden");++coldCount;}require(coldCount>=3,"cold build lost prefab instances");
 unsigned allocated=provider.allocations;auto* warm=makeLayer("warm");run(warm,"warm");
 size_t warmCount=0,shared=0;meshes=scene.GetComponents<Mesh>();for(auto* a:meshes)if(a->Owner->Parent==warm->Owner && a->IsInstanced()) {require(a->Owner->IsEnabledInHierarchy(),"completed warm batch remains hidden");++warmCount;for(auto* b:meshes)if(b->Owner->Parent==cold->Owner && b->GetInstanceSource()==a->GetInstanceSource()){
  auto* am=a->Owner->GetComponent<Material>();auto* bm=b->Owner->GetComponent<Material>();require(bool(am)==bool(bm),"shared geometry lost material");
  if(am)require(Engine::Serialization::JsonWriteCompact(am->Serialize())==Engine::Serialization::JsonWriteCompact(bm->Serialize()),"cached material changed values");++shared;break;}}
 require(warmCount==coldCount && shared==warmCount,"warm cell failed to reuse geometry");if(!realGpu)require(provider.allocations==allocated,"warm cell allocated duplicate geometry");
 warm->placements[0].position.x=17;warm->MarkConfigurationDirty();warm->PreparePrefabInstances(activeProvider);scene.CommitPendingObjectChanges();
 require(warm->IsPrefabPreparationPending(),"edit should be built incrementally");
 bool oldVisible=false;for(auto* m:scene.GetComponents<Mesh>())if(m->Owner->Parent==warm->Owner && m->IsInstanced() && m->Owner->IsEnabledInHierarchy())oldVisible=true;
 require(oldVisible,"edit hid the previous batches before replacements were ready");
 run(warm,"edit");bool found=false;for(auto* m:scene.GetComponents<Mesh>())if(m->Owner->Parent==warm->Owner && m->IsInstanced())for(auto& t:m->GetInstanceData()->transforms)if(t.rows[0].w==17)found=true;require(found,"edited placement not rebuilt");
 auto* interrupted=makeLayer("interrupted");
 interrupted->PreparePrefabInstances(activeProvider);scene.CommitPendingObjectChanges();
 size_t firstCount=0;for(auto* m:scene.GetComponents<Mesh>())if(m->Owner->Parent==interrupted->Owner && m->IsInstanced())++firstCount;
 require(interrupted->IsPrefabPreparationPending(),"expected asynchronous or incremental preparation");
 interrupted->placements[0].position.x=23;interrupted->MarkConfigurationDirty();run(interrupted,"interrupted_edit");
 bool interruptedUpdated=false;for(auto* m:scene.GetComponents<Mesh>())if(m->Owner->Parent==interrupted->Owner && m->IsInstanced())for(auto& t:m->GetInstanceData()->transforms)if(t.rows[0].w==23)interruptedUpdated=true;
 require(interruptedUpdated,"edit during preparation was lost");
 scene.RemoveObject(interrupted->Owner);scene.CommitPendingObjectChanges();
 if(!oak){
  const std::string pendingPath=".local/tests-generated/prefab-stream-benchmark/pending.prefab";
  std::filesystem::copy_file(assets[0],pendingPath,std::filesystem::copy_options::overwrite_existing);
  auto* pending=scene.AddObject("pending unload")->AddComponent<TerrainObjectLayer>();TerrainObjectLayer::Placement p;p.asset=pendingPath;pending->placements.push_back(p);pending->MarkConfigurationDirty();
  pending->PreparePrefabInstances(activeProvider);require(pending->IsPrefabPreparationPending(),"cold prefab was not scheduled asynchronously");
  auto* pendingOwner=pending->Owner;scene.RemoveObject(pendingOwner);scene.CommitPendingObjectChanges();std::this_thread::sleep_for(std::chrono::milliseconds(20));
  for(auto* m:scene.GetComponents<Mesh>())require(m->Owner->Parent!=pendingOwner,"unloaded pending prefab published geometry");
  const std::string changingPath=".local/tests-generated/prefab-stream-benchmark/changing.prefab";
  std::filesystem::copy_file(assets[0],changingPath,std::filesystem::copy_options::overwrite_existing);
  auto* changing=scene.AddObject("changing prefab")->AddComponent<TerrainObjectLayer>();p.asset=changingPath;changing->placements.push_back(p);changing->MarkConfigurationDirty();
  changing->PreparePrefabInstances(activeProvider);require(changing->IsPrefabPreparationPending(),"changed prefab was not queued");
  {std::ifstream in(changingPath);std::string xml((std::istreambuf_iterator<char>(in)),{});auto at=xml.find("name=\"tree\"");require(at!=std::string::npos,"fixture name missing");xml.replace(at,11,"name=\"updated\"");std::ofstream out(changingPath,std::ios::trunc);out<<xml;}
  std::filesystem::last_write_time(changingPath,std::filesystem::file_time_type::clock::now()+std::chrono::seconds(2));
  run(changing,"source_edit_pending");bool newSource=false;for(auto* m:scene.GetComponents<Mesh>())if(m->Owner->Parent==changing->Owner && m->Owner->name.find("updated")!=std::string::npos)newSource=true;
  require(newSource,"source edit during preparation published stale prefab");
 }
 auto* canceled=makeLayer("canceled");canceled->PreparePrefabInstances(activeProvider);scene.CommitPendingObjectChanges();
 auto* canceledOwner=canceled->Owner;scene.RemoveObject(canceledOwner);scene.CommitPendingObjectChanges();
 for(auto* m:scene.GetComponents<Mesh>())require(m->Owner->Parent!=canceledOwner,"unloading partial build left batches behind");
 scene.RemoveObject(cold->Owner);scene.CommitPendingObjectChanges();auto* reload=makeLayer("reload");run(reload,"reload");size_t reloadCount=0;for(auto* m:scene.GetComponents<Mesh>())if(m->Owner->Parent==reload->Owner && m->IsInstanced()){++reloadCount;require(m->GetInstanceData()->transforms.size()==static_cast<size_t>(placementsPerAsset),"reload lost instance data");}
 require(reloadCount==coldCount,"reload lost prefab batches");
#ifdef _WIN32
 if(renderer)renderer->WaitIdle();
#endif
 std::cout<<"PASS\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
