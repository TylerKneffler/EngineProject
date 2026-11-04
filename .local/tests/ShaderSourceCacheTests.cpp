#include "Core/Rendering/ShaderSourceCache.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <stdexcept>
using Cache=Engine::Rendering::ShaderSourceCache;
namespace fs=std::filesystem;
void Require(bool test,const char* message) { if(!test) throw std::runtime_error(message); }
void Write(const fs::path& path,const char* text)
{ fs::create_directories(path.parent_path()); std::ofstream out(path); out<<text; }
int main()
{
    const auto root=fs::temp_directory_path()/("engine-shader-cache-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    try
    {
        Cache cache;
        const auto preferred=root/"project.shaders"/"surface.hlsl", fallback=root/"engine"/"surface.hlsl";
        Write(fallback,"initial");
        int searches=0;
        const auto resolve=[&]() -> const Cache::Source& {
            return cache.Resolve("material","surface.hlsl",[&]{++searches;return Cache::SearchPaths{preferred,fallback};});
        };
        const auto settle=[&] {
            for(int i=0;i<20;++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); cache.Poll(); resolve(); }
        };
        const auto initial=resolve();
        Require(initial.exists && initial.hlsl && initial.path==fallback.generic_string(),"fallback resolution");
        // Directory timestamps from fixture creation may complete after the watch
        // is armed. Drain those before measuring an unchanged render phase.
        settle();
        const auto checks=cache.GetStatistics().metadataChecks;
        for(int i=0;i<1000;++i) { cache.Poll(); resolve(); }
#if defined(_WIN32)
        Require(searches==1 && cache.GetStatistics().metadataChecks==checks,"unchanged frames performed filesystem checks");
#endif
        const auto wait=[&](const auto& predicate) {
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(4);
            do { cache.Poll(); if(predicate()) return; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            while(std::chrono::steady_clock::now()<until);
            throw std::runtime_error("shader change was not recovered");
        };
        // Creating missing higher-priority directories/files must replace fallback.
        Write(preferred,"preferred");
        wait([&]{return resolve().path==preferred.generic_string();});
        const auto before=resolve().revision;
#if defined(_WIN32)
        const auto originalTime=fs::last_write_time(preferred);
#endif
        Write(preferred,"different"); // Same length, with the timestamp restored below.
#if defined(_WIN32)
        fs::last_write_time(preferred,originalTime);
#endif
        wait([&]{return resolve().revision!=before;});
        // Includes invalidate even with the entry point's timestamp unchanged.
        const auto includeBefore=resolve().revision;
        Write(preferred.parent_path()/"common.hlsli","include");
#if defined(_WIN32)
        wait([&]{return resolve().revision!=includeBefore;});
#endif
        fs::remove(preferred);
        wait([&]{return resolve().path==fallback.generic_string();});
        fs::remove(fallback);
        wait([&]{return !resolve().exists;});
        Write(preferred,"recovered");
        wait([&]{return resolve().exists && resolve().path==preferred.generic_string();});
        // Raw path and material location are keys, independent of revision notifications.
        Write(root/"other"/"changed.HLSL","other");
        const auto& other=cache.Resolve("material","changed.HLSL",[&]{return Cache::SearchPaths{root/"other"/"changed.HLSL"};});
        Require(other.exists && other.hlsl && other.path!=resolve().path,"direct path edit");
        const auto& moved=cache.Resolve("moved-material","surface.hlsl",[&]{return Cache::SearchPaths{root/"other"/"changed.HLSL"};});
        Require(moved.exists && moved.path==other.path,"material relocation");
        const auto& shared=cache.Resolve("second-material","changed.HLSL",[&]{return Cache::SearchPaths{root/"other"/"changed.HLSL"};});
        Require(shared.revision==other.revision,"shared sources must share bytecode generation");
        // Drain remaining queued edit notifications before measuring steady state.
        settle();
        const auto stable=cache.GetStatistics().metadataChecks;
        for(int i=0;i<1000;++i) { cache.Poll(); resolve(); }
#if defined(_WIN32)
        Require(cache.GetStatistics().metadataChecks==stable,"steady-state source checks recurred");
#endif
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<"\n"; fs::remove_all(root); return 1; }
    fs::remove_all(root);
    std::cout<<"Shader source cache checks passed\n";
}
