#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Rendering/TextureResidency.h"
#include "Core/Rendering/TextureFootprint.h"
#include "TextureStreamingFixtures.h"
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace Engine::Graphics;
using Engine::Components::Texture;
void Check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
struct Handle:IGraphicsTexture
{
    bool ready=true;
    void* GetNativeHandle() const override {return const_cast<Handle*>(this);}
    bool IsUploadComplete() const override {return ready;}
};
struct Provider:IGraphicsProvider,IGraphicsTextureFactory
{
    bool ready=true; std::vector<std::weak_ptr<Handle>> handles;
    IShaderCompiler* GetShaderCompiler() override {return nullptr;}
    IGraphicsBufferFactory* GetBufferFactory() override {return nullptr;}
    IPipelineStateFactory* GetPipelineStateFactory() override {return nullptr;}
    IGraphicsContextFactory* GetContextFactory() override {return nullptr;}
    IGraphicsTextureFactory* GetTextureFactory() override {return this;}
    std::shared_ptr<IGraphicsTexture> CreateTexture2D(uint32_t,uint32_t,const uint8_t*,uint32_t,GraphicsTextureFormat,bool) override
    {auto handle=std::make_shared<Handle>();handle->ready=ready;handles.push_back(handle);return handle;}
};
int main()
{
    try
    {
        using namespace Engine::Rendering;
        Check(TextureDemandMip(4096,2048,13,512,1)==2,"projected demand/prefetch");
        Check(TextureDemandMip(3,1,2,0)==1,"offscreen floor");
        TextureMipHistory history;
        Check(history.Update(1,0,2)==0 && history.Update(2,5,2)==0 && history.Update(4,5,2)==5,"recession hold expires");
        const auto projection=glm::perspectiveLH_ZO(glm::radians(60.f),1.f,.1f,100.f);
        const auto world=glm::translate(glm::mat4(1.f),glm::vec3(0,0,10));
        const auto footprint=ProjectedTextureFootprint(glm::vec3(-1),glm::vec3(1),projection*world,{512,512},1.f);
        Check(footprint>0 && std::isfinite(footprint),"projected footprint");
        Check(ProjectedTextureFootprint(glm::vec3(-1),glm::vec3(1),projection*world,{1024,1024},1.f)>footprint*1.99f,"render resolution demand");
        Check(ProjectedTextureFootprint(glm::vec3(-1),glm::vec3(1),projection*world,{512,512},.25f)<footprint*.251f,"UV repeat demand");
        Check(std::isinf(ProjectedTextureFootprint(glm::vec3(-1),glm::vec3(1),projection,{512,512},1.f)),"near-plane demand");
        TextureStreamingSettings settings;settings.budgetBytes=4*1048576;settings.uploadBytesPerFrame=2*1048576;settings.holdFrames=2;
        Texture::ConfigureStreaming(settings);
        const auto path=std::filesystem::absolute(".local/texture-streaming-unit/fixture.tga");
        WriteStreamingFixture(path,0);
        auto texture=Texture::Acquire(path.generic_string());Provider provider;
        Check(texture->PrepareImmediate(&provider),"fallback upload");
        Check(texture->GetResidentFirstMip()==3,"source metadata/fallback mip");
        const auto* fallback=texture->GetGraphicsTexture();
        provider.ready=false;
        for(int frame=0;frame<200 && provider.handles.size()<2;++frame)
        {
            texture->RequestMipFootprint(1024);Texture::StreamFrame(&provider);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(provider.handles.size()==2,"async decode never uploaded");
        Check(texture->GetGraphicsTexture()==fallback,"published incomplete upload");
        Texture::StreamFrame(&provider);
        Check(texture->GetGraphicsTexture()==fallback,"pending upload replaced fallback");
        if(auto pending=provider.handles.back().lock()) pending->ready=true;
        texture->RequestMipFootprint(1024);Texture::StreamFrame(&provider);
        Check(texture->GetResidentFirstMip()==0,"completed upload not published");
        TextureFrameOwners frames;frames.Prepare(0);frames.Retain(texture->GetGraphicsTexture());
        std::weak_ptr<const IGraphicsTexture> old=texture->GetGraphicsTexture()->shared_from_this();
        for(int i=0;i<4;++i) Texture::StreamFrame(&provider);
        Check(texture->GetGraphicsTexture()==fallback && !old.expired(),"eviction freed in-flight frame");
        frames.Prepare(1);Check(!old.expired(),"unrelated frame slot freed image");
        frames.Prepare(0);Check(old.expired(),"completed frame slot leaked image");
        Check(Texture::GetStreamingSnapshot().evictions>0,"eviction telemetry");
        settings.budgetBytes=1;Texture::ConfigureStreaming(settings);
        texture->RequestMipFootprint(1024);Texture::StreamFrame(&provider);
        Check(Texture::GetStreamingSnapshot().blocked>0,"impossible budget not reported");
        texture->Reload();
        Check(!texture->PrepareImmediate(&provider),"impossible fallback floor was admitted");
        std::cout<<"Texture demand, publication, retirement and budget checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
