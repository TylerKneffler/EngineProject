#include "Core/Rendering/RenderSpatialIndex.h"
#include "Core/Rendering/RenderBoundsCache.h"
#include "Core/Threading/JobScheduler.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
using Index=Engine::Rendering::RenderSpatialIndex;
using Bounds=Engine::Rendering::RenderBounds;
void Require(bool valid,const char* message) { if(!valid) throw std::runtime_error(message); }
std::vector<Bounds> Grid(size_t count)
{
    std::vector<Bounds> bounds(count);
    for(size_t i=0;i<count;++i)
    {
        const glm::vec3 center(float(i%128)*2,float(i%5),float(i/128)*2);
        bounds[i]={center-glm::vec3(.5f),center+glm::vec3(.5f),true};
    }
    return bounds;
}
glm::mat4 Clip(unsigned view)
{
    auto clip=glm::scale(glm::mat4(1),glm::vec3(.035f,.08f,.025f));
    clip*=glm::rotate(glm::mat4(1),float(view)*.17f,glm::normalize(glm::vec3(1,2,1)));
    clip*=glm::translate(glm::mat4(1),glm::vec3(-float(view)*11,-2,-float(view)*2));
    clip[3].z+=.5f; return clip;
}
std::vector<uint8_t> Brute(const std::vector<Bounds>& bounds,const glm::mat4& clip)
{
    glm::vec4 row[4]; for(int i=0;i<4;++i) row[i]={clip[0][i],clip[1][i],clip[2][i],clip[3][i]};
    const glm::vec4 planes[]={row[3]+row[0],row[3]-row[0],row[3]+row[1],row[3]-row[1],row[2],row[3]-row[2]};
    std::vector<uint8_t> result(bounds.size(),1);
    for(size_t i=0;i<bounds.size();++i) if(bounds[i].bounded) for(const auto& plane:planes)
    {
        bool allOutside=true;
        for(unsigned corner=0;corner<8;++corner)
        {
            const glm::vec3 point((corner&1) ? bounds[i].maximum.x:bounds[i].minimum.x,
                (corner&2) ? bounds[i].maximum.y:bounds[i].minimum.y,(corner&4) ? bounds[i].maximum.z:bounds[i].minimum.z);
            if(glm::dot(plane,glm::vec4(point,1))>=-1.e-4f) { allOutside=false; break; }
        }
        if(allOutside) { result[i]=0; break; }
    }
    return result;
}
void Check(Index& index,const std::vector<Bounds>& bounds,bool brute)
{
    Index rebuilt; rebuilt.Update(bounds);
    for(unsigned view=0;view<12;++view)
    {
        const auto clip=Clip(view);
        const auto visible=index.Query(clip);
        Require(visible==rebuilt.Query(clip),"refit visibility differs from a rebuilt tree");
        if(brute) Require(visible==Brute(bounds,clip),"tree visibility differs from brute-force corner/plane testing");
        Require(index.Query(clip)==visible,"cached view changed without a bounds update");
    }
    for(unsigned point=0;point<8;++point)
    {
        const glm::vec3 position(float(point)*3.25f,0,float(point)*2.f);
        std::vector<uint32_t> expected;
        for(uint32_t i=0;i<bounds.size();++i) if(!bounds[i].bounded ||
            (position.x>=bounds[i].minimum.x && position.x<=bounds[i].maximum.x &&
             position.z>=bounds[i].minimum.z && position.z<=bounds[i].maximum.z)) expected.push_back(i);
        auto actual=index.QueryPointXZ(position); std::sort(actual.begin(),actual.end());
        Require(actual==expected,"refit point query lost or added candidates");
    }
}
void PersistentSlotsRegression()
{
    using Cache=Engine::Rendering::RenderBoundsCache;
    Cache cache; Index index; std::vector<Bounds> retained;
    std::vector<Cache::Input> active;
    const auto grid=Grid(2048);
    for(uint32_t slot=0;slot<grid.size();++slot)
    { Cache::Input item; item.slot=slot; item.bounded=true; item.minimum=grid[slot].minimum; item.maximum=grid[slot].maximum; active.push_back(item); }
    const auto update=[&](uint32_t count) {
        cache.PreparePersistent(active,count,retained);
        const auto stats=index.Update(retained);
        // Packed reference has only active items in their current order. This
        // checks the mapping used by camera and each shadow cascade separately.
        std::vector<Bounds> packed;
        for(const auto& item:active) packed.push_back(item.bounded ?
            Engine::Rendering::TransformBounds(item.minimum,item.maximum,item.world):Bounds{});
        for(unsigned view=0;view<12;++view)
        {
            const auto clip=Clip(view); const auto expected=Brute(packed,clip);
            const auto& visibility=index.Query(clip);
            for(size_t item=0;item<active.size();++item)
                Require(visibility[active[item].slot]==expected[item],"persistent slot visibility differs from active packed reference");
        }
        return stats;
    };
    Require(update(2048).rebuilds==1,"initial persistent tree missing");
    std::reverse(active.begin(),active.end());
    Require(!update(2048).changedBounds,"draw order changed persistent bounds");
    const auto saved=active;
    active.erase(active.begin(),active.begin()+1000);
    Require(!update(2048).rebuilds,"temporary draw removal rebuilt persistent tree");
    active.clear(); Require(!update(2048).changedBounds,"empty active frame discarded retained bounds");
    active=saved; Require(!update(2048).changedBounds,"LOD/visibility reactivation changed static slots");
    // Slot reuse after deletion must overwrite stale geometry, including the
    // bounded -> unbounded -> bounded transition and a move while inactive.
    active[0].world[3].x-=20;
    Require(update(2048).changedBounds==1,"recycled slot kept its previous transform");
    active[1].bounded=false; Require(update(2048).rebuilds==1,"unbounded recycled slot kept bounded classification");
    active[1].bounded=true; Require(update(2048).rebuilds==1,"bounded recycled slot remained unbounded");
    auto added=active[0]; added.slot=2055; active.push_back(added);
    Require(update(2056).rebuilds==1,"persistent capacity growth did not rebuild");
    active.clear(); Require(!update(2056).changedBounds,"inactive holes changed retained tree");
    Require(update(0).rebuilds==1 && retained.empty(),"explicit storage reset retained stale slots");
    active.push_back(added); active.back().slot=0; update(1);
    bool rejected=false;
    try { cache.PreparePersistent(active,0,retained); } catch(const std::out_of_range&) { rejected=true; }
    Require(rejected && retained.size()==1,"invalid slot mutated persistent storage");
    std::cout<<"Persistent draw-slot order, fades, inactivity, recycling, growth and reset passed.\n";
}
void Regression()
{
    Index index; auto bounds=Grid(2048); bounds[3].bounded=false;
    auto stats=index.Update(bounds); Require(stats.rebuilds==1,"initial spatial tree was not built"); Check(index,bounds,true);
    stats=index.Update(bounds); Require(!stats.changedBounds && !stats.refitNodes && !stats.rebuilds && stats.reusedBounds==bounds.size(),"static tree performed work");
    auto& jobs=Engine::Threading::JobScheduler::Get(); const auto before=jobs.ReadStatistics();
    stats=index.Update(bounds); Require(before.jobs==jobs.ReadStatistics().jobs,"unchanged spatial tree scheduled jobs");
    std::array<std::vector<uint8_t>,12> concurrentVisibility;
    jobs.For(concurrentVisibility.size(),1,[&](size_t view) {
        concurrentVisibility[view]=index.QueryUncached(Clip(static_cast<unsigned>(view)));
    });
    for(unsigned view=0;view<concurrentVisibility.size();++view)
        Require(concurrentVisibility[view]==Brute(bounds,Clip(view)),
            "concurrent uncached view query differs from brute-force visibility");
    // The same clip must be invalidated after an isolated move.
    const auto old=index.Query(glm::mat4(1)); bounds[0].minimum={-.25f,-.25f,.25f}; bounds[0].maximum={.25f,.25f,.75f};
    stats=index.Update(bounds);
    Require(!stats.rebuilds && stats.changedBounds==1 && stats.dirtyLeaves==1 && stats.refitNodes<32,"one edit rebuilt unrelated spatial nodes");
    Require(index.Query(glm::mat4(1))==Brute(bounds,glm::mat4(1)),"bounds edit retained stale visibility cache");
    Check(index,bounds,true);
    std::mt19937 random(42);
    for(unsigned frame=0;frame<40;++frame)
    {
        jobs.SetParallelForEnabled(frame%2!=0);
        for(unsigned move=0;move<(frame%3==0 ? 2048u:17u);++move)
        {
            auto& item=bounds[random()%bounds.size()];
            const glm::vec3 delta((int(random()%101)-50)*.002f,0,(int(random()%101)-50)*.002f);
            item.minimum+=delta; item.maximum+=delta;
        }
        stats=index.Update(bounds); Check(index,bounds,frame%10==0);
    }
    jobs.SetParallelForEnabled(true);
    bounds[5].bounded=false; Require(index.Update(bounds).rebuilds==1,"bounded membership change did not rebuild"); Check(index,bounds,false);
    bounds.push_back({{0,0,0},{1,1,1},true}); stats=index.Update(bounds);
    Require(stats.rebuilds==1 && stats.changedBounds==1 && stats.reusedBounds==bounds.size()-1,"insert counters or membership rebuild are incorrect"); Check(index,bounds,false);
    bounds.erase(bounds.begin()+8); Require(index.Update(bounds).rebuilds==1,"remove did not rebuild"); Check(index,bounds,false);
    // Strongly separating formerly clustered nodes must eventually repartition.
    Index quality; auto clustered=Grid(2048); quality.Update(clustered); bool rebuilt=false;
    for(unsigned frame=0;frame<12;++frame)
    {
        clustered[0].minimum={100000.f+float(frame),0,100000.f}; clustered[0].maximum=clustered[0].minimum+glm::vec3(1);
        const auto update=quality.Update(clustered); rebuilt=rebuilt || update.qualityRebuilds!=0;
        if(frame==0) Require(update.qualityRebuilds==1,"single teleport left a permanently degraded static tree");
        Check(quality,clustered,false);
    }
    Require(rebuilt,"degraded refit tree did not trigger a quality rebuild");
    index.Update({}); Require(index.Query(glm::mat4(1)).empty() && index.QueryPointXZ({0,0,0}).empty(),"cleared tree retained candidates");
    std::vector<Bounds> unbounded(5); index.Update(unbounded); Require(index.Query(glm::mat4(1))==std::vector<uint8_t>(5,1),"unbounded-only index was not visible");
    unbounded[2].minimum.x=1; stats=index.Update(unbounded); Require(!stats.refitNodes && !stats.rebuilds,"unbounded bounds performed a tree refit");
    Index boundary; std::vector<Bounds> edge={{{1,0,0},{2,1,1},true},{{-2,0,0},{-1,1,1},true}}; boundary.Update(edge);
    Require(boundary.Query(glm::mat4(1))==std::vector<uint8_t>(2,1),"touching clip boundary was culled");
    Index rays;
    std::vector<Bounds> rayBounds={{{2,-1,-1},{3,1,1},true},
        {{6,-1,-1},{7,1,1},true},{{0,0,0},{0,0,0},false}};
    rays.Update(rayBounds);
    Require(rays.QueryRay({0,0,0},{1,0,0},5.f)==std::vector<uint32_t>({0,2}),
        "ray query missed the near box or included the far box");
    Require(rays.QueryRay({0,2,0},{1,0,0},10.f)==std::vector<uint32_t>({2}),
        "parallel ray crossed a box outside its slab");
    rayBounds[0].minimum.x=8; rayBounds[0].maximum.x=9;
    rays.Update(rayBounds);
    Require(rays.QueryRay({0,0,0},{1,0,0},5.f)==std::vector<uint32_t>({2}),
        "ray query kept a stale box after refit");
    std::cout<<"Spatial index: sparse/dense serial and parallel refits, independent visibility/point queries, cache invalidation, membership, quality rebuilds, empty/unbounded/boundary cases passed.\n";
}
void Benchmark()
{
    namespace fs=std::filesystem; fs::create_directories(".local/performance-logs");
    std::ofstream report(".local/performance-logs/parallel-stage5-spatial.jsonl");
    for(const auto count:{1024u,16384u}) for(unsigned workload=0;workload<3;++workload) for(unsigned mode=0;mode<3;++mode)
    {
        auto bounds=Grid(count); Index index; index.SetIncrementalUpdatesEnabled(mode!=0);
        Engine::Threading::JobScheduler::Get().SetParallelForEnabled(mode!=1); index.Update(bounds);
        double total=0,refit=0,build=0,nodes=0,rebuilds=0; unsigned measured=0;
        for(unsigned frame=0;frame<68;++frame)
        {
            if(workload) for(size_t i=0;i<(workload==1 ? 1u:bounds.size());++i)
            { const glm::vec3 shift(.01f,0,0); bounds[i].minimum+=shift; bounds[i].maximum+=shift; }
            auto snapshot=bounds; const auto started=std::chrono::steady_clock::now();
            const auto update=index.Update(std::move(snapshot));
            const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            if(frame>=8) { total+=elapsed; refit+=update.refitMs; build+=update.buildMs; nodes+=update.refitNodes; rebuilds+=update.rebuilds; ++measured; }
        }
        const auto* name=mode==0 ? "rebuild":mode==1 ? "refit-serial":"refit-parallel";
        const auto* work=workload==0 ? "static":workload==1 ? "sparse":"dense";
        report<<"{\"items\":"<<count<<",\"workload\":\""<<work<<"\",\"mode\":\""<<name<<"\",\"frames\":"<<measured
            <<",\"total_ms\":"<<total/measured<<",\"refit_ms\":"<<refit/measured<<",\"build_ms\":"<<build/measured
            <<",\"refit_nodes\":"<<nodes/measured<<",\"rebuilds\":"<<rebuilds/measured<<"}\n";
        std::cout<<count<<" "<<work<<" "<<name<<": "<<total/measured<<" ms\n";
    }
    Engine::Threading::JobScheduler::Get().SetParallelForEnabled(true);
}
void SlotBenchmark()
{
    using Cache=Engine::Rendering::RenderBoundsCache;
    const auto grid=Grid(4096);
    std::vector<Cache::Input> all;
    for(uint32_t slot=0;slot<grid.size();++slot)
    { Cache::Input item; item.slot=slot; item.bounded=true; item.minimum=grid[slot].minimum; item.maximum=grid[slot].maximum; all.push_back(item); }
    std::filesystem::create_directories(".local/performance-logs");
    std::ofstream report(".local/performance-logs/parallel-stage6-slot-benchmark.jsonl");
    for(bool persistent:{false,true})
    {
        Cache cache; Index index; std::vector<Bounds> bounds;
        if(persistent) cache.PreparePersistent(all,4096,bounds); else cache.Prepare(all,bounds);
        index.Update(bounds);
        double total=0,indexMs=0; uint64_t rebuilds=0,refits=0;
        for(unsigned frame=0;frame<128;++frame)
        {
            std::vector<Cache::Input> active;
            const size_t count=3072+(frame%5)*64;
            for(size_t i=0;i<count;++i) active.push_back(all[(i+frame*17)%all.size()]);
            const auto started=std::chrono::steady_clock::now();
            if(persistent) cache.PreparePersistent(active,4096,bounds); else cache.Prepare(active,bounds);
            const auto stats=index.Update(bounds);
            const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            if(frame>=8) { total+=elapsed; indexMs+=stats.totalMs; rebuilds+=stats.rebuilds; refits+=stats.refitNodes; }
        }
        report<<"{\"mode\":\""<<(persistent ? "persistent":"packed")<<"\",\"frames\":120,\"prepare_and_index_ms\":"<<total/120
            <<",\"index_ms\":"<<indexMs/120<<",\"rebuilds\":"<<rebuilds<<",\"refit_nodes\":"<<refits<<"}\n";
        std::cout<<(persistent ? "Persistent":"Packed")<<" rotating active set: "<<total/120<<" ms, "<<rebuilds<<" rebuilds.\n";
    }
}
int main(int argc,char** argv)
{
    try { if(argc>1 && std::string(argv[1])=="--slot-benchmark") SlotBenchmark(); else if(argc>1 && std::string(argv[1])=="--benchmark") Benchmark(); else { Regression(); PersistentSlotsRegression(); } }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
