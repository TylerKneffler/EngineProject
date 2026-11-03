#pragma once
#include "RenderSpatialIndex.h"
#include "Core/Threading/JobScheduler.h"
#include <stdexcept>

namespace Engine::Rendering
{
class RenderBoundsCache
{
public:
    struct Input
    {
        uint32_t slot=0;
        bool bounded=false;
        glm::vec3 minimum{0},maximum{0};
        glm::mat4 world{1};
    };
    struct Statistics { uint64_t reused=0,updated=0; };
private:
    struct Entry { Input input; RenderBounds bounds; bool valid=false; };
    struct Pending { Input input; size_t output; RenderBounds bounds; };
    std::vector<Entry> m_entries;
    std::vector<Pending> m_pending;
public:
    Statistics Prepare(const std::vector<Input>& input,std::vector<RenderBounds>& output)
    {
        output.assign(input.size(),{});
        return PrepareInto(input,output,false);
    }
    // Caller-owned slot storage retains inactive bounds. Visibility must be
    // consumed only through active draw slots, never by iterating this storage.
    Statistics PreparePersistent(const std::vector<Input>& input,uint32_t slotCount,std::vector<RenderBounds>& output)
    {
        for(const auto& value:input) if(value.slot>=slotCount)
            throw std::out_of_range("Render bounds slot exceeds persistent storage");
        output.resize(slotCount);
        return PrepareInto(input,output,true);
    }
private:
    Statistics PrepareInto(const std::vector<Input>& input,std::vector<RenderBounds>& output,bool persistent)
    {
        Statistics statistics; m_pending.clear();
        for(size_t index=0;index<input.size();++index)
        {
            const auto& value=input[index];
            const size_t destination=persistent ? value.slot:index;
            if(!value.bounded) { output[destination]={}; continue; }
            if(m_entries.size()<=value.slot) m_entries.resize(size_t(value.slot)+1);
            const auto& entry=m_entries[value.slot];
            if(entry.valid && entry.input.minimum==value.minimum && entry.input.maximum==value.maximum && entry.input.world==value.world)
            { output[destination]=entry.bounds; ++statistics.reused; }
            else { m_pending.push_back({value,destination,{}}); ++statistics.updated; }
        }
        // Workers touch only copied matrices/bounds and their distinct result.
        Threading::JobScheduler::Get().For(m_pending.size(),64,[&](size_t index) {
            auto& item=m_pending[index]; item.bounds=TransformBounds(item.input.minimum,item.input.maximum,item.input.world);
        });
        for(const auto& item:m_pending)
        {
            output[item.output]=item.bounds;
            m_entries[item.input.slot]={item.input,item.bounds,true};
        }
        return statistics;
    }
};
}
