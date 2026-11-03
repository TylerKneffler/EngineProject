#include "Core/Rendering/DrawBatchPreparation.h"
#include "Core/Threading/JobScheduler.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>
using Preparation=Engine::Rendering::DrawBatchPreparation;
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
std::vector<Preparation::Input> Inputs(uint32_t count)
{
    std::vector<Preparation::Input> input;
    for(uint32_t slot=0;slot<count;++slot)
    {
        Preparation::Input item; item.slot=slot; item.index=slot;
        std::get<4>(item.key)=32; std::get<5>(item.key)=3;
        std::get<8>(item.key)=slot%7; std::get<10>(item.key)=slot%3;
        std::get<11>(item.key)={{0,3},{3,6}};
        input.push_back(std::move(item));
    }
    return input;
}
Preparation::Result Reference(const std::vector<Preparation::Input>& input)
{
    Preparation::Result result;
    std::map<Engine::Rendering::DrawBatchKey,uint32_t> lookup;
    std::vector<std::vector<std::pair<uint32_t,uint32_t>>> members;
    for(const auto& item:input)
    {
        auto found=lookup.find(item.key);
        if(found==lookup.end())
        { found=lookup.emplace(item.key,uint32_t(members.size())).first; members.push_back({}); }
        members[found->second].emplace_back(item.slot,item.index);
    }
    for(auto& group:members)
    {
        std::sort(group.begin(),group.end());
        Preparation::Group output;
        for(const auto& [slot,index]:group) output.members.push_back(index);
        result.groups.push_back(std::move(output));
    }
    return result;
}
void Check(const Preparation::Result& result,const std::vector<Preparation::Input>& input)
{
    const auto reference=Reference(input);
    Require(reference.groups.size()==result.groups.size(),"batch compatibility changed group count");
    for(size_t group=0;group<result.groups.size();++group)
        Require(reference.groups[group].members==result.groups[group].members,"batch group order or slot-sorted members differ from serial renderer");
}
void Regression()
{
    Preparation cache; auto input=Inputs(1024);
    auto result=cache.Prepare(input); Require(!result.reused,"cold cache reported hit"); Check(result,input);
    std::mt19937 random(41);
    for(unsigned frame=0;frame<24;++frame)
    {
        std::shuffle(input.begin(),input.end(),random);
        // Include gaps in source indexes, as ineligible draws never enter keys.
        for(uint32_t i=0;i<input.size();++i) input[i].index=i*2;
        result=cache.Prepare(input); Require(result.reused,"camera order invalidated compatibility cache"); Check(result,input);
    }
    const auto verifyChange=[&](bool expectedReuse=false) {
        result=cache.Prepare(input); Require(result.reused==expectedReuse,"compatibility assignment reuse was incorrect"); Check(result,input);
        result=cache.Prepare(input); Require(result.reused,"unchanged compatibility was rebuilt"); Check(result,input);
    };
    // Every key category affects compatibility, including all texture slots,
    // indirect LOD meshes, species ranges, shadow flags and isolated owners.
    int identity[8]{};
    std::get<0>(input[0].key)=&identity[0]; verifyChange();
    std::get<1>(input[0].key)=&identity[1]; verifyChange();
    std::get<2>(input[0].key)=&identity[2]; verifyChange();
    for(auto& texture:std::get<3>(input[0].key)) { texture=reinterpret_cast<const Engine::Graphics::IGraphicsTexture*>(&identity[3]); verifyChange(); }
    ++std::get<4>(input[0].key); verifyChange(); ++std::get<5>(input[0].key); verifyChange();
    ++std::get<6>(input[0].key); verifyChange(); ++std::get<7>(input[0].key); verifyChange();
    ++std::get<8>(input[0].key); verifyChange(); std::get<9>(input[0].key)=true; verifyChange();
    ++std::get<10>(input[0].key); verifyChange();
    std::get<11>(input[0].key)[1].second=18; verifyChange();
    for(auto& lod:std::get<12>(input[0].key)) { lod=&identity[4]; verifyChange(); }
    std::get<13>(input[0].key)=&identity[5]; verifyChange();
    input[0].slot=99999; verifyChange(); // Recycling identity.
    input.erase(input.begin()+20,input.begin()+70); verifyChange(true);
    auto added=input[5]; added.slot=100001; added.index=200001; input.push_back(added); verifyChange();
    for(unsigned edit=0;edit<24;++edit)
    {
        for(auto& item:input) std::get<8>(item.key)=edit%4;
        verifyChange();
    }
    result=cache.Prepare(input,false); Require(!result.reused,"disabled reuse kept assignments"); Check(result,input);
    Preparation shadow;
    Check(shadow.Prepare(input),input);
    const auto reference=input; auto snapshot=input;
    auto future=Engine::Threading::JobScheduler::Get().Submit([&cache,snapshot=std::move(snapshot)]() mutable { return cache.Prepare(std::move(snapshot)); });
    input.clear(); input.shrink_to_fit(); // Worker owns its copied identities.
    result=future.get(); Require(result.reused,"worker snapshot missed unchanged cache");
    Check(result,reference);
    result=cache.Prepare({}); Require(result.groups.empty(),"empty view retained stale batches");
    result=cache.Prepare({}); Require(result.reused,"empty cache was not reusable");
    Check(cache.Prepare(reference),reference);
    Preparation otherScene; Check(otherScene.Prepare(Inputs(3)),Inputs(3));
    std::cout<<"Batch snapshot compatibility, deterministic order, reordering cache hits, edits, membership, empty views, separate owners and worker publication passed.\n";
}
void Benchmark()
{
    std::filesystem::create_directories(".local/performance-logs");
    std::ofstream report(".local/performance-logs/parallel-stage7-batch-benchmark.jsonl");
    for(uint32_t count:{512u,4096u}) for(unsigned mode=0;mode<3;++mode)
    {
        auto input=Inputs(count); Preparation cache; cache.Prepare(input);
        double total=0; uint64_t hits=0;
        for(unsigned frame=0;frame<72;++frame)
        {
            std::rotate(input.begin(),input.begin()+17,input.end());
            for(uint32_t i=0;i<count;++i) input[i].index=i;
            auto snapshot=input; const auto started=std::chrono::steady_clock::now();
            Preparation::Result result;
            if(mode==0) result=Reference(snapshot);
            else if(mode==1) result=cache.Prepare(std::move(snapshot));
            else result=Engine::Threading::JobScheduler::Get().Submit([&cache,snapshot=std::move(snapshot)]() mutable { return cache.Prepare(std::move(snapshot)); }).get();
            const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            if(frame>=8) { total+=elapsed; hits+=result.reused; }
        }
        const auto* name=mode==0 ? "serial-map":mode==1 ? "cached-owner":"cached-worker";
        report<<"{\"items\":"<<count<<",\"mode\":\""<<name<<"\",\"frames\":64,\"wall_ms\":"<<total/64<<",\"hits\":"<<hits<<"}\n";
        std::cout<<count<<" "<<name<<": "<<total/64<<" ms, "<<hits<<" hits\n";
    }
}
int main(int argc,char** argv)
{
    try { if(argc>1 && std::string(argv[1])=="--benchmark") Benchmark(); else Regression(); }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
