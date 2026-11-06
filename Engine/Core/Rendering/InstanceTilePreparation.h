#pragma once
#include "PersistentInstanceBatches.h"
#include "RenderJobs.h"
#include <memory>
#include <unordered_map>

namespace Engine::Rendering
{
// Copied, camera-independent values only. GPU resources and live components
// remain on the scene owner; workers publish immutable tile records after join.
class InstanceTilePreparation
{
public:
    struct Input
    {
        uint32_t slot=0,base=0,maximumLod=0;
        glm::mat4 world{1};
        glm::vec3 minimum{0},maximum{0};
        glm::vec4 lod{0};
        std::vector<std::pair<uint32_t,uint32_t>> ranges;
        bool operator==(const Input& other) const
        {
            return slot==other.slot && base==other.base && maximumLod==other.maximumLod &&
                world==other.world && minimum==other.minimum && maximum==other.maximum &&
                lod==other.lod && ranges==other.ranges;
        }
    };
    using Records=std::vector<InstanceTileRecord>;
    using RecordsOwner=std::shared_ptr<const Records>;
    struct Result
    {
        std::vector<RecordsOwner> records;
        uint64_t reused=0,built=0,workerBatches=0;
    };
    // A pass may use different LOD bands for the same object slot. Retain
    // both versions so alternating shadow/color passes do not evict each other.
    Result Prepare(const std::vector<Input>& inputs,uint64_t serial,uint32_t domain=0)
    {
        Result result;
        result.records.resize(inputs.size());
        std::vector<size_t> changed;
        for(size_t i=0;i<inputs.size();++i)
        {
            auto found=m_entries.find((uint64_t(domain)<<32)|inputs[i].slot);
            if(found!=m_entries.end() && found->second.input==inputs[i])
            {
                found->second.serial=serial;
                result.records[i]=found->second.records;
                ++result.reused;
            }
            else changed.push_back(i);
        }
        const auto& scheduler=RenderJobs::Get();
        if(changed.size()>=128 && scheduler.WorkerCount() && scheduler.ParallelForEnabled())
            result.workerBatches=1;
        // Each index owns its output. No shared map mutation, resource calls,
        // component reads or concurrent vector growth occurs in this loop.
        RenderJobs::Get().For(changed.size(),64,[&](size_t index)
        {
            const auto i=changed[index];
            const auto& input=inputs[i];
            auto records=std::make_shared<Records>(input.ranges.size());
            for(size_t range=0;range<input.ranges.size();++range)
            {
                auto& tile=(*records)[range];
                for(int row=0;row<3;++row) for(int column=0;column<4;++column)
                    tile.world[row][column]=input.world[column][row];
                tile.minimum=glm::vec4(input.minimum,0);
                tile.maximum=glm::vec4(input.maximum,0);
                tile.range={input.base+input.ranges[range].first,input.ranges[range].second,input.slot,input.maximumLod};
                tile.origin=input.world[3];
                tile.lod=input.lod;
            }
            result.records[i]=std::move(records);
        });
        // Join precedes owner-side publication. Prior readers own their record
        // vectors even when a source is edited, removed or its slot is reused.
        for(const auto i:changed)
            m_entries[(uint64_t(domain)<<32)|inputs[i].slot]={inputs[i],result.records[i],serial};
        result.built=changed.size();
        for(auto it=m_entries.begin();it!=m_entries.end();)
            if(serial>it->second.serial && serial-it->second.serial>240) it=m_entries.erase(it);
            else ++it;
        return result;
    }
private:
    struct Entry { Input input; RecordsOwner records; uint64_t serial; };
    std::unordered_map<uint64_t,Entry> m_entries;
};
}
