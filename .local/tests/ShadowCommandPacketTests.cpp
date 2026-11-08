#include "Core/Graphics/GraphicsCommandPacket.h"
#include <array>
#include <iostream>
#include <stdexcept>

using namespace Engine::Graphics;
void Check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
struct Texture : IGraphicsTexture { void* GetNativeHandle() const override { return nullptr; } };
struct Target : IGraphicsContext
{
    bool supported=true,accept=true,active=false;
    unsigned clears=0;
    std::array<float,4> depth{.2f,.3f,.4f,.5f};
    bool SupportsDepthRectangleClear() const override { return supported; }
    bool BeginDepthOnlyPass(const IGraphicsTexture*,float value) override
    { active=accept; if(active && value>=0) depth.fill(value); return active; }
    void EndDepthOnlyPass() override { Check(active,"depth pass end out of order"); active=false; }
    void ClearDepthRectangle(const ScissorRect& r,float value) override
    {
        Check(active,"rectangle clear replayed outside depth pass"); ++clears;
        for(int y=r.top;y<r.bottom;++y) for(int x=r.left;x<r.right;++x) depth[y*2+x]=value;
    }
    void SetPipeline(const IPipelineState*) override {}
    void SetConstantBuffer(uint32_t,const IGraphicsBuffer*,uint64_t) override {}
    void SetStructuredBuffer(uint32_t,const IGraphicsBuffer*) override {}
    void SetVertexBuffer(uint32_t,const IGraphicsBuffer*,uint32_t,uint64_t) override {}
    void SetIndexBuffer(const IGraphicsBuffer*,uint32_t,uint64_t) override {}
    void SetViewport(const Viewport&) override {}
    void SetScissorRect(const ScissorRect&) override {}
    void Clear(float,float,float,float,float) override {}
    void DrawInstanced(uint32_t,uint32_t,uint32_t,uint32_t) override {}
    void DrawIndexedInstanced(uint32_t,uint32_t,uint32_t,int32_t,uint32_t) override {}
    void TransitionResource(void*,ResourceState,ResourceState) override {}
    void* GetNativeHandle() const override { return nullptr; }
};
int main()
{
    try
    {
        GraphicsCommandPacket packet;
        auto texture=std::make_shared<Texture>();
        packet.SetDepthRectangleClearSupported(true);
        packet.BeginDepthOnlyPass(texture.get(),-1);
        packet.ClearDepthRectangle({0,0,1,1},1);
        packet.EndDepthOnlyPass();
        Target target;
        packet.Replay(target);
        Check(target.depth==std::array<float,4>{1,.3f,.4f,.5f} && target.clears==1 && !target.active,
            "packet clear did not preserve other depth tiles");
        Target rejected; rejected.accept=false;
        packet.Replay(rejected);
        Check(rejected.clears==0,"failed depth pass replayed its rectangle clear");
        Target unsupported; unsupported.supported=false;
        bool threw=false;
        try { packet.Replay(unsupported); } catch(const std::logic_error&) { threw=true; }
        Check(threw,"unsupported replay silently lost rectangle clearing");
        packet.Reset();
        Check(!packet.SupportsDepthRectangleClear(),"recycled packet retained old backend capabilities");
        target.clears=0;
        packet.Replay(target);
        Check(!target.clears,"recycled packet replayed old depth commands");
        std::cout << "Depth rectangle packet order, preservation, fallback and reset passed\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
