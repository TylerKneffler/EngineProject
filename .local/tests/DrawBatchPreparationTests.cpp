#include "Core/Rendering/DrawBatchPreparation.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
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
}
