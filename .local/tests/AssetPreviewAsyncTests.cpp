#include "Engine/Editor/Core/View/Templates/Assets/AssetPreviewCache.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Engine/Editor/Core/Paint/BrushPreset.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Threading/JobScheduler.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <future>
#include <atomic>
#include <stdexcept>
#include <array>
using Engine::Editor::AssetPreviewCache;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct Image final:Engine::Graphics::IGraphicsTexture
{
    std::thread::id owner=std::this_thread::get_id();
    std::vector<uint8_t> pixels;
    ~Image() { if(owner!=std::this_thread::get_id()) std::terminate(); }
    void* GetNativeHandle() const override { return const_cast<Image*>(this); }
};
struct Provider final:Engine::Graphics::IGraphicsProvider,Engine::Graphics::IGraphicsTextureFactory
{
    std::thread::id owner=std::this_thread::get_id(); unsigned uploads=0;
    Engine::Graphics::IShaderCompiler* GetShaderCompiler() override { return nullptr; }
    Engine::Graphics::IGraphicsBufferFactory* GetBufferFactory() override { return nullptr; }
    Engine::Graphics::IPipelineStateFactory* GetPipelineStateFactory() override { return nullptr; }
    Engine::Graphics::IGraphicsContextFactory* GetContextFactory() override { return nullptr; }
    Engine::Graphics::IGraphicsTextureFactory* GetTextureFactory() override { return this; }
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> CreateTexture2D(uint32_t width,uint32_t height,
        const uint8_t* pixels,uint32_t mips,Engine::Graphics::GraphicsTextureFormat format,bool) override
    {
        Require(owner==std::this_thread::get_id(),"GPU upload ran off its owner");
        Require(width==128 && height==128 && mips==1 && format==Engine::Graphics::GraphicsTextureFormat::Rgba8,"bad thumbnail layout");
        auto result=std::make_shared<Image>(); result->pixels.assign(pixels,pixels+width*height*4); ++uploads; return result;
    }
};
void WriteImage(const fs::path& path,uint8_t red,uint8_t blue)
{
    std::array<uint8_t,18> header{}; header[2]=2; header[12]=2; header[14]=2; header[16]=32; header[17]=0x28;
    std::ofstream file(path,std::ios::binary|std::ios::trunc);
    file.write(reinterpret_cast<const char*>(header.data()),header.size());
    const uint8_t pixel[]={blue,0,red,255};
    for(int i=0;i<4;++i) file.write(reinterpret_cast<const char*>(pixel),4);
}
Image* Await(AssetPreviewCache& cache,const std::string& path,Provider& provider)
{
    const auto deadline=Clock::now()+std::chrono::seconds(10);
    while(Clock::now()<deadline)
    {
        cache.Poll(); if(auto image=cache.Get(path,&provider)) return static_cast<Image*>(image);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("thumbnail did not finish");
}
void Drain()
{
    const auto deadline=Clock::now()+std::chrono::seconds(10);
    while(Engine::Threading::JobScheduler::Get().ReadStatistics().pending && Clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(Engine::Threading::JobScheduler::Get().ReadStatistics().pending==0,"thumbnail jobs did not drain");
}
struct Gate
{
    std::promise<void> release; bool open=false;
    void Open() { if(!open) { open=true; release.set_value(); } }
    ~Gate() { Open(); }
};
int main()
{
    try
    {
        _putenv_s("ENGINE_WORKER_COUNT","2");
        const fs::path root=".local/test-output/asset-preview-async"; fs::create_directories(root);
        const auto image=(root/"image.tga").string(); WriteImage(image,255,0);
        auto live=Engine::Components::Texture::Acquire(image); Require(live->Load(),"live image load failed");
        auto detached=Engine::Components::Texture::DecodeDetached(image);
        Require(detached && detached.get()!=live.get() && detached->GetMipLevels()==1 && detached->GetPixels().size()==16,
            "detached texture reused live state or generated extra mips");
        Provider provider; AssetPreviewCache cache;
        auto& jobs=Engine::Threading::JobScheduler::Get();
        {
            Gate gate; auto released=gate.release.get_future().share(); auto entered=std::make_shared<std::atomic<unsigned>>(0);
            auto first=jobs.Submit([released,entered] { ++*entered; released.wait(); });
            auto second=jobs.Submit([released,entered] { ++*entered; released.wait(); });
            const auto deadline=Clock::now()+std::chrono::seconds(5);
            while(entered->load()!=2 && Clock::now()<deadline) std::this_thread::yield();
            Require(entered->load()==2,"test workers failed to enter gate");
            const auto started=Clock::now();
            for(unsigned i=0;i<20;++i)
            {
                const auto path=(root/("queued-"+std::to_string(i)+".tga")).string(); WriteImage(path,255,0);
                Require(cache.Get(path,&provider)==nullptr,"pending thumbnail unexpectedly published");
            }
            Require(jobs.ReadStatistics().pending==4,"preview queue exceeded two outstanding jobs");
            cache.Clear();
            Require(Clock::now()-started<std::chrono::seconds(2),"thumbnail request/destruction blocked on workers");
            gate.Open(); first.get(); second.get();
        }
        Drain(); Require(provider.uploads==0,"cancelled thumbnails published GPU data");
        WriteImage(image,0,255);
        {
            // Completion must wake an idle browser even when decoding/queue delay
            // exceeds the recent-visibility window used for metadata refresh.
            Gate gate; auto released=gate.release.get_future().share();
            auto first=jobs.Submit([released] { released.wait(); });
            auto second=jobs.Submit([released] { released.wait(); });
            Require(cache.Get(image,&provider)==nullptr,"first preview request blocked and uploaded");
            std::this_thread::sleep_for(std::chrono::milliseconds(2100));
            gate.Open(); first.get(); second.get();
        }
        const auto deadline=Clock::now()+std::chrono::seconds(10);
        bool notified=false; while(!notified && Clock::now()<deadline) { notified=cache.Poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        Require(notified && provider.uploads==0,"completion notification uploaded GPU data");
        auto preview=Await(cache,image,provider); const auto center=(64*128+64)*4;
        Require(preview->pixels[center+2]>200 && preview->pixels[center]<20,"thumbnail did not decode replacement image");
        Require(live->GetPixels()[0]==255 && live->GetPixels()[2]==0,"preview decoding mutated a live cached texture");
        const auto uploads=provider.uploads;
        for(int i=0;i<20;++i) Require(cache.Get(image,&provider)==preview,"unchanged thumbnail lost identity");
        Require(provider.uploads==uploads,"unchanged preview uploaded repeatedly");
        // Both clients see invalidation, but only their graphics owner publishes results.
        AssetPreviewCache other; Require(other.Get(image,&provider)==preview,"preview clients failed to share prepared data");
        WriteImage(image,255,0); other.Invalidate(image);
        cache.Get(image,&provider); Drain(); cache.Get(image,&provider);
        auto replaced=static_cast<Image*>(cache.Get(image,&provider));
        Require(replaced && replaced->pixels[center]>200 && replaced->pixels[center+2]<20,"invalidated preview republished stale data");
        // Discard a completed CPU result if the asset is invalidated before publication.
        const auto beforeStale=provider.uploads;
        WriteImage(image,0,255); cache.Invalidate(image); cache.Get(image,&provider); Drain();
        WriteImage(image,255,0); cache.Invalidate(image); cache.Get(image,&provider);
        Require(provider.uploads==beforeStale,"invalidated ready result was uploaded");
        Drain(); cache.Get(image,&provider);
        Require(provider.uploads==beforeStale+1,"replacement result was not published");
        // External edits are checked on workers, even without explicit invalidation.
        WriteImage(image,0,255); fs::last_write_time(image,fs::file_time_type::clock::now()+std::chrono::seconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        cache.Poll(); cache.Get(image,&provider); Drain();
        auto refreshed=static_cast<Image*>(cache.Get(image,&provider));
        Require(refreshed && refreshed->pixels[center+2]>200,"metadata refresh missed an external edit");
        // A derived scene preview changes independently of the scene file.
        const auto scene=(root/"test.scene").string();
        {
            std::ofstream file(root/".test.scene.preview",std::ios::binary); const uint32_t header[]={0x56525053,128,128};
            file.write(reinterpret_cast<const char*>(header),sizeof(header)); std::vector<uint8_t> pixels(128*128*4,77);
            file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());
        }
        Require(Await(cache,scene,provider)->pixels[0]==77,"scene sidecar preview failed");
        const auto bitmap=(root/"test.bmp").string();
        {
            std::array<uint8_t,70> data{}; data[0]='B'; data[1]='M'; data[2]=70; data[10]=54;
            data[14]=40; data[18]=2; data[22]=2; data[26]=1; data[28]=24;
            for(int y=0;y<2;++y) for(int x=0;x<2;++x) data[54+y*8+x*3+1]=255;
            std::ofstream file(bitmap,std::ios::binary); file.write(reinterpret_cast<const char*>(data.data()),data.size());
        }
        Require(Await(cache,bitmap,provider)->pixels[center+1]>200,"worker WIC image decoding failed");
        const auto material=(root/"test.material").string();
        { std::ofstream file(material); file<<R"({"baseColor":[1,0,0],"roughness":0.6,"metallic":0})"; }
        Require(Await(cache,material,provider)!=nullptr,"material rasterization failed");
        const auto brush=(root/"test.brush").string();
        Engine::Editor::Paint::BrushPreset preset; std::string brushError;
        Require(Engine::Editor::Paint::SaveBrushPreset(brush,preset,brushError),"brush fixture failed");
        Require(Await(cache,brush,provider)!=nullptr,"brush rasterization failed");
        const auto mesh=(root/"triangle.obj").string();
        { std::ofstream file(mesh); file<<"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"; }
        Require(Await(cache,mesh,provider)!=nullptr,"mesh rasterization failed");
        // Ready or queued work must be harmless after all owning caches close.
        other.Clear(); cache.Clear(); Drain();
        std::cout<<"Async previews: bounded/nonblocking jobs, private decoding, owner-only publication/destruction, shared invalidation, image/material/scene previews passed.\n";
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
