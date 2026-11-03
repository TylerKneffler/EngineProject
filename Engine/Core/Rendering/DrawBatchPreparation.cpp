#include "DrawBatchPreparation.h"
#include <algorithm>
#include <chrono>

namespace Engine::Rendering
{
DrawBatchPreparation::Result DrawBatchPreparation::Prepare(std::vector<Input> input,bool allowReuse)
{
    const auto started=std::chrono::steady_clock::now();
    if(!allowReuse) { m_slots.clear(); m_lookup.clear(); m_compatibility.clear(); m_free.clear(); }
    Result result;
    std::unordered_map<uint32_t,uint32_t> activeGroups;
    for(uint32_t i=0;i<input.size();++i)
    {
        const auto& item=input[i];
        auto slot=m_slots.find(item.slot);
        uint32_t identity;
        if(slot!=m_slots.end() && m_compatibility[slot->second].key==item.key)
        { identity=slot->second; ++result.reusedKeys; }
        else
        {
            // Acquire the new assignment before releasing the old one. Slot
            // publication and reference counts remain coherent if allocation
            // fails; no group ID referenced by an active slot can be recycled.
            auto entry=m_lookup.find(item.key);
            bool created=false;
            if(entry==m_lookup.end())
            {
                identity=uint32_t(m_compatibility.size());
                if(!m_free.empty()) identity=m_free.back();
                Compatibility replacement{item.key,0};
                if(identity==m_compatibility.size()) m_compatibility.push_back(std::move(replacement));
                else m_compatibility[identity]=std::move(replacement);
                try { entry=m_lookup.emplace(item.key,identity).first; }
                catch(...) { m_compatibility[identity]={}; throw; }
                if(!m_free.empty() && m_free.back()==identity) m_free.pop_back();
                created=true;
            }
            identity=entry->second;
            uint32_t old=UINT32_MAX;
            if(slot==m_slots.end())
            {
                try { m_slots.emplace(item.slot,identity); }
                catch(...)
                {
                    if(created) { m_lookup.erase(entry); m_compatibility[identity]={}; m_free.push_back(identity); }
                    throw;
                }
            }
            else { old=slot->second; slot->second=identity; }
            ++m_compatibility[identity].references;
            if(old!=UINT32_MAX && --m_compatibility[old].references==0)
            {
                m_lookup.erase(m_compatibility[old].key);
                m_compatibility[old]={}; m_free.push_back(old);
            }
        }
        const auto [group,inserted]=activeGroups.emplace(identity,uint32_t(result.groups.size()));
        if(inserted) result.groups.push_back({});
        result.groups[group->second].members.push_back(i);
    }
    result.reused=result.reusedKeys==input.size();
    for(auto& group:result.groups)
    {
        std::sort(group.members.begin(),group.members.end(),[&](auto a,auto b) { return input[a].slot<input[b].slot; });
        for(auto& member:group.members) member=input[member].index;
    }
    result.buildMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    return result;
}
}
