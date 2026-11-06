#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace Engine::Components;
using namespace Engine::Graphics;
void Check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
struct Buffer : IGraphicsBuffer {
    Usage usage; AccessMode access; uint32_t stride; std::vector<uint8_t> bytes;
    Usage GetUsage() const override { return usage; }
    AccessMode GetAccessMode() const override { return access; }
    uint64_t GetSize() const override { return bytes.size(); }
    uint32_t GetElementStride() const override { return stride; }
    void* Map() override { return bytes.data(); }
    void Unmap() override {}
    void* GetNativeHandle() const override { return const_cast<Buffer*>(this); }
};
struct Factory : IGraphicsBufferFactory {
    std::unique_ptr<IGraphicsBuffer> CreateBuffer(IGraphicsBuffer::Usage usage,
        IGraphicsBuffer::AccessMode access,uint64_t size,const void* data,uint32_t stride) override {
        auto result=std::make_unique<Buffer>(); result->usage=usage;result->access=access;
        result->stride=stride;result->bytes.resize(size);
        if(data) std::memcpy(result->bytes.data(),data,size);
        return result;
    }
};
struct ScriptedMesh : Mesh {
    int* destroyed=nullptr; std::thread::id owner;
    void OnDestroy() override {
        Check(std::this_thread::get_id()==owner,"destroy callback moved off the scene owner");
        ++*destroyed;
    }
};
int main() {
    try {
        Factory factory; int waits=0,destroyed=0;
        Engine::Scene::Scene scene;
        scene.SetBeforeGpuResourceDestruction([&] { ++waits; });
        scene.SetDeferredGraphicsRetirement(true);
        auto create=[&](const char* name,bool runtime=true) {
            auto* object=scene.AddObject(name);object->runtimeOnly=runtime;
            auto* mesh=object->AddComponent<Mesh>();object->AddComponent<Material>();
            std::vector<Mesh::Vertex> vertices(3);vertices[1].pos[0]=1;vertices[2].pos[1]=1;
            Check(mesh->SetAuthoredVertices(vertices),"fixture geometry rejected");
            mesh->CreateBuffer(&factory);return object;
        };
        auto* parent=scene.AddObject("parent");
        auto* tile=create("streamed tile");
        scene.MoveObject(tile,parent,Engine::Scene::Scene::ObjectPlacement::AsChild);
        auto inactive=std::make_unique<Mesh>();
        std::vector<Mesh::Vertex> lodVertices(3);lodVertices[1].pos[0]=2;lodVertices[2].pos[1]=2;
        Check(inactive->SetAuthoredVertices(lodVertices),"inactive LOD fixture rejected");
        inactive->CreateBuffer(&factory);
        std::weak_ptr<IGraphicsBuffer> inactiveBuffer=inactive->GetVertexBufferOwner();
        Check(tile->GetComponent<Mesh>()->StoreRuntimeLod(1,std::move(inactive)),"inactive LOD fixture installation failed");
        const auto id=scene.GetObjectId(tile);
        std::weak_ptr<IGraphicsBuffer> buffer=tile->GetComponent<Mesh>()->GetVertexBufferOwner();
        Check(!buffer.expired(),"fixture has no GPU buffer");
        scene.RequestRemoveObject(tile);scene.CommitPendingObjectChanges();
        Check(waits==0,"streamed render-only removal drained the GPU");
        Check(!scene.FindObjectById(id) && parent->Children.empty(),"logical removal left hierarchy references");
        Check(scene.GetComponents<Mesh>().empty(),"removed mesh remained in component registry");
        Check(!buffer.expired() && !inactiveBuffer.expired(),"active or inactive LOD died before fence completion");
        auto retired=scene.TakeRetiredGraphicsResources();
        Check(!retired.Empty() && retired.objects.size()==1,"retirement omitted object-owned resources");
        Check(scene.TakeRetiredGraphicsResources().Empty(),"retirement handed ownership out twice");
        Check(!buffer.expired(),"handoff released resources before its owning frame completed");
        retired={}; // The renderer performs this only after the submitted frame fence.
        Check(buffer.expired() && inactiveBuffer.expired(),"completed retirement leaked active/inactive LOD buffers");

        auto* scripted=scene.AddObject("scripted mesh");scripted->runtimeOnly=true;
        auto* script=scripted->AddComponent<ScriptedMesh>();
        script->destroyed=&destroyed;script->owner=std::this_thread::get_id();
        scene.RemoveObject(scripted);
        Check(waits==1 && destroyed==1,"subclass bypassed the original lifecycle/GPU wait");
        Check(scene.TakeRetiredGraphicsResources().Empty(),"scripted object was deferred");

        auto* subtree=scene.AddObject("runtime subtree");subtree->runtimeOnly=true;
        auto* child=create("child");
        scene.MoveObject(child,subtree,Engine::Scene::Scene::ObjectPlacement::AsChild);
        scene.RemoveObject(subtree);
        Check(waits==2,"subtree removal bypassed the conservative GPU fence");
        Check(scene.TakeRetiredGraphicsResources().Empty(),"subtree was deferred as a leaf");
        auto* authored=create("authored",false);scene.RemoveObject(authored);
        Check(waits==3,"authored object destruction semantics changed");
        scene.SetDeferredGraphicsRetirement(false);
        auto* immediate=create("immediate");scene.RemoveObject(immediate);
        Check(waits==4,"immediate rendering bypassed its required GPU drain");
        Check(scene.TakeRetiredGraphicsResources().Empty(),"immediate mode retained objects");
        int clearWaits=0;
        {
            Engine::Scene::Scene empty;
            empty.SetDeferredGraphicsRetirement(true);
            empty.SetBeforeGpuResourceDestruction([&] {++clearWaits;});
            auto* leaf=empty.AddObject("last leaf");leaf->runtimeOnly=true;
            leaf->AddComponent<Mesh>(); empty.RemoveObject(leaf);
            empty.ClearObjects();
            Check(clearWaits==1 && empty.TakeRetiredGraphicsResources().Empty(),
                "empty-scene clear did not drain and release pending retirement");
        }
        bool destructionWait=false;
        {
            Engine::Scene::Scene pending;
            pending.SetDeferredGraphicsRetirement(true);
            pending.SetBeforeGpuResourceDestruction([&] {destructionWait=true;});
            auto* leaf=pending.AddObject("last leaf before shutdown");leaf->runtimeOnly=true;
            leaf->AddComponent<Mesh>(); pending.RemoveObject(leaf);
            Check(!destructionWait,"last-leaf removal forced an immediate GPU wait");
        }
        Check(destructionWait,"scene destruction released unsubmitted retirement without a GPU fence");
        std::cout<<"Streamed resource lifetime, logical removal, fence handoff and lifecycle fallbacks passed.\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
