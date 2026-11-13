#include "Core/Rendering/DrawBatchPreparation.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include "Core/Rendering/PreparedInstanceDraw.h"
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>
using namespace Engine::Rendering;
using namespace Engine::Graphics;
static void require(bool value) { if (!value) std::abort(); }
static void check(DrawBatchPreparation& cached, const std::vector<DrawBatchPreparation::Input>& in) {
 DrawBatchPreparation fresh;
 auto a=cached.Prepare(in), b=fresh.Prepare(in,false);
 require(a.groups.size()==b.groups.size());
 for(size_t i=0;i<a.groups.size();++i) require(a.groups[i].members==b.groups[i].members);
 require(cached.CachedSlotCount()==in.size());
}
struct Buffer final : IGraphicsBuffer {
 Usage GetUsage() const override { return Usage::VertexBuffer; }
 AccessMode GetAccessMode() const override { return AccessMode::Default; }
 uint64_t GetSize() const override { return 4; }
 void* Map() override { return nullptr; }
 void Unmap() override {}
 void* GetNativeHandle() const override { return nullptr; }
};
struct Texture final : IGraphicsTexture { void* GetNativeHandle() const override { return nullptr; } };
static void resourceLifetime() {
 GraphicsCommandPacketPool pool;
 auto firstPacket=pool.Acquire(); auto* firstAddress=firstPacket.get();
 auto retainedBuffer=std::make_shared<Buffer>(); std::weak_ptr<Buffer> retained=retainedBuffer;
 firstPacket->SetVertexBuffer(0,retainedBuffer.get(),4); retainedBuffer.reset();
 // Represents the fence-retained frame and an independent replay reader.
 auto inFlight=firstPacket; firstPacket.reset();
 auto secondPacket=pool.Acquire(); require(secondPacket.get()!=firstAddress && !retained.expired());
 inFlight.reset();
 auto recycled=pool.Acquire(); require(recycled.get()==firstAddress && retained.expired());
 GraphicsCommandPacket packet;
 auto buffer=std::make_shared<Buffer>(); auto texture=std::make_shared<Texture>();
 std::weak_ptr<Buffer> weakBuffer=buffer; std::weak_ptr<Texture> weakTexture=texture;
 packet.SetVertexBuffer(0,buffer.get(),4); packet.SetTexture(0,texture.get());
 buffer.reset(); texture.reset();
 require(!weakBuffer.expired() && !weakTexture.expired());
 packet.Reset(); require(weakBuffer.expired() && weakTexture.expired());
 auto depth=std::make_shared<Texture>(); std::weak_ptr<Texture> weakDepth=depth;
 require(packet.BeginDepthOnlyPass(depth.get())); depth.reset(); require(!weakDepth.expired());
 packet.Reset(); require(weakDepth.expired());
 TextureFrameOwners owners;
 auto t=std::make_shared<Texture>(); std::weak_ptr<Texture> weak=t;
 owners.Prepare(0); owners.Retain(t.get()); t.reset(); require(!weak.expired());
 owners.Prepare(1); require(!weak.expired()); owners.Prepare(0); require(weak.expired());
}
struct DrawRecorder {
 const IPipelineState* pipeline=nullptr;
 std::vector<uint64_t> bindings, draw;
 void SetPipeline(const IPipelineState* p) { pipeline=p; }
 void SetConstantBuffer(uint32_t slot,const IGraphicsBuffer* p)
 { bindings.insert(bindings.end(),{1,slot,uint64_t(p)}); }
 void SetStructuredBuffer(uint32_t slot,const IGraphicsBuffer* p)
 { bindings.insert(bindings.end(),{2,slot,uint64_t(p)}); }
 void SetVertexBuffer(uint32_t slot,const IGraphicsBuffer* p,uint32_t stride,uint64_t offset)
 { bindings.insert(bindings.end(),{3,slot,uint64_t(p),stride,offset}); }
 void SetIndexBuffer(const IGraphicsBuffer* p,uint32_t count,uint64_t offset)
 { bindings.insert(bindings.end(),{4,uint64_t(p),count,offset}); }
 void DrawInstancesIndirect(const IGraphicsBuffer* p,uint64_t offset,bool indexed)
 { draw={1,uint64_t(p),offset,uint64_t(indexed)}; }
 void DrawIndexedInstanced(uint32_t count,uint32_t instances,uint32_t first,int base,uint32_t start)
 { draw={2,count,instances,first,uint64_t(base),start}; }
 void DrawInstanced(uint32_t count,uint32_t instances,uint32_t first,uint32_t start)
 { draw={3,count,instances,first,start}; }
};
static void reuseVisibilityAcrossPasses() {
 Buffer constants,visible,placements,vertices,indices,delta,weights;
 PreparedInstanceDraw data{&constants,&visible,&placements,&vertices,&indices,32,120,36,12,73,true};
 const auto* depth=reinterpret_cast<const IPipelineState*>(&delta);
 const auto* color=reinterpret_cast<const IPipelineState*>(&weights);
 for (bool indexed : {false,true}) for (bool indirect : {false,true}) {
  data.indices=indexed ? &indices:nullptr; data.indirect=indirect;
  DrawRecorder first,second;
  data.Submit(first,depth,&delta,&weights);
  data.Submit(second,color,&delta,&weights);
  require(first.pipeline==depth && second.pipeline==color);
  require(first.bindings==second.bindings && first.draw==second.draw);
  if(indirect) require(first.draw==std::vector<uint64_t>{1,uint64_t(&visible),0,uint64_t(indexed)});
  else if(indexed) require(first.draw==std::vector<uint64_t>{2,36,73,12,0,0});
  else require(first.draw==std::vector<uint64_t>{3,36,73,12,0});
 }
 // A subsequent view/frame publishes its own visibility and range data.
 data.visible=&indices; data.elementCount=18; data.firstElement=24; data.instanceCount=7;
 data.indirect=false; DrawRecorder changed; data.Submit(changed,color,&delta,&weights);
 require(changed.draw==std::vector<uint64_t>{2,18,7,24,0,0});
}
int main() {
 DrawBatchPreparation cache;
 DrawBatchKey a{}, b{}; std::get<4>(b)=42;
 std::vector<DrawBatchPreparation::Input> in={{a,3,0},{a,1,1},{b,2,2}};
 check(cache,in); check(cache,in);
 std::swap(in[0],in[1]); check(cache,in); check(cache,in);
 in[1].index=7; check(cache,in); check(cache,in);
 in[0].key=b; check(cache,in); check(cache,in);
 Texture materialTextureA, materialTextureB;
 std::get<3>(in[0].key)[0]=&materialTextureA; check(cache,in); check(cache,in);
 std::get<3>(in[0].key)[0]=&materialTextureB; check(cache,in); check(cache,in);
 std::get<2>(in[0].key)=&materialTextureA; check(cache,in);
 auto otherView=in; otherView.erase(otherView.begin()+1); check(cache,otherView); check(cache,in);
 in.erase(in.begin()+1); check(cache,in); check(cache,in);
 in.push_back({a,4,9}); check(cache,in); check(cache,in);
 cache.Prepare(in,false); check(cache,in);
 for(uint32_t i=0;i<1000;++i) { check(cache,{{a,i,i},{b,i+1,i+1}}); }
 check(cache,{}); require(cache.CachedSlotCount()==0);
 resourceLifetime();
 reuseVisibilityAcrossPasses();
}
