#pragma once
#include "Core/Object.h"
#include "Core/Rendering/RenderSpatialIndex.h"
#include <unordered_map>
#include <unordered_set>

namespace Engine::Components
{
struct SpatialUpdateRegion
{
    Core::Object* owner=nullptr;
    Core::Object* viewer=nullptr;
    Rendering::RenderBounds bounds{};
};
// Optional scheduling contract for viewer-local scripts. Mutation stays on
// the scene thread; dormant fields participate only in region invalidation.
class SpatialUpdate
{
public:
    virtual ~SpatialUpdate()=default;
    virtual uint64_t SpatialRegionRevision() const=0;
    virtual SpatialUpdateRegion DescribeSpatialRegion()=0;
    virtual bool TickSpatialRegion(bool editor)=0;
    virtual void LeaveSpatialRegion()=0;
};
class SpatialUpdateScheduler
{
    struct Entry { SpatialUpdate* source; uint64_t revision; SpatialUpdateRegion region; };
    std::vector<Entry> m_entries;
    std::unordered_set<SpatialUpdate*> m_active;
    uint64_t m_structure=UINT64_MAX;
    Rendering::RenderSpatialIndex m_index;
public:
    size_t activeCount=0, dormantCount=0;
    template<class Scene> bool Tick(Scene& scene,bool editor)
    {
        bool changed=false, rebuild=m_structure!=scene.GetStructureRevision();
        const auto& sources=scene.template GetComponents<SpatialUpdate>();
        if(rebuild)
        {
            std::unordered_set<SpatialUpdate*> live(sources.begin(),sources.end());
            for(auto it=m_active.begin();it!=m_active.end();)
                if(!live.count(*it)) it=m_active.erase(it); else ++it;
            m_entries.clear(); m_entries.reserve(sources.size());
            for(auto* source:sources) m_entries.push_back({source,UINT64_MAX,{}});
            m_structure=scene.GetStructureRevision();
        }
        for(auto& entry:m_entries)
        {
            const auto revision=entry.source->SpatialRegionRevision();
            if(rebuild || entry.revision!=revision)
            { entry.region=entry.source->DescribeSpatialRegion(); entry.revision=revision; rebuild=true; }
        }
        if(rebuild)
        {
            std::vector<Rendering::RenderBounds> bounds; bounds.reserve(m_entries.size());
            for(const auto& entry:m_entries) bounds.push_back(entry.region.bounds);
            m_index.Update(std::move(bounds));
        }
        std::unordered_map<Core::Object*,glm::vec3> viewers;
        for(const auto& entry:m_entries) if(entry.region.viewer)
            viewers.try_emplace(entry.region.viewer,glm::vec3(0));
        std::unordered_set<SpatialUpdate*> next;
        for(auto& [viewer,position]:viewers)
        {
            position=viewer->transform.GetWorldPosition();
            for(auto index:m_index.QueryPointXZ(position))
            {
                auto& entry=m_entries[index];
                if(entry.region.viewer==viewer && entry.region.owner && entry.region.owner->IsEnabledInHierarchy())
                    next.insert(entry.source);
            }
        }
        for(auto* source:m_active) if(!next.count(source))
        { source->LeaveSpatialRegion(); changed=true; }
        // Keep authored order for a deterministic shared preparation budget.
        for(const auto& entry:m_entries) if(next.count(entry.source))
            changed=entry.source->TickSpatialRegion(editor) || changed;
        m_active=std::move(next); activeCount=m_active.size(); dormantCount=m_entries.size()-activeCount;
        return changed;
    }
};
}
