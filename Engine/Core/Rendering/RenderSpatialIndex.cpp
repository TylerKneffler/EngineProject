#include "RenderSpatialIndex.h"
#include "Core/Threading/JobScheduler.h"
#include <chrono>
#include <stdexcept>

namespace Engine::Rendering
{
RenderBounds RenderSpatialIndex::Merge(const RenderBounds& a,const RenderBounds& b)
{ return {glm::min(a.minimum,b.minimum),glm::max(a.maximum,b.maximum),true}; }

double RenderSpatialIndex::Area(const RenderBounds& bounds)
{
    const auto size=glm::max(glm::dvec3(bounds.maximum)-glm::dvec3(bounds.minimum),glm::dvec3(0));
    return 2.*(size.x*size.y+size.x*size.z+size.y*size.z);
}

uint32_t RenderSpatialIndex::BuildNode(uint32_t first,uint32_t count,uint32_t parent,uint32_t depth)
{
    const uint32_t index = uint32_t(m_nodes.size());
    m_nodes.push_back({});
    RenderBounds bounds = m_bounds[m_order[first]];
    for (uint32_t i=1;i<count;++i)
    { bounds.minimum=glm::min(bounds.minimum,m_bounds[m_order[first+i]].minimum);
      bounds.maximum=glm::max(bounds.maximum,m_bounds[m_order[first+i]].maximum); }
    m_nodes[index] = {bounds, first, count, 0, 0,parent,depth,Area(bounds)};
    if(m_dirtyLevels.size()<=depth) m_dirtyLevels.resize(size_t(depth)+1);
    if (count > 8)
    {
        const auto extent = bounds.maximum-bounds.minimum;
        const int axis = extent.x > extent.y ? (extent.x > extent.z ? 0:2) : (extent.y > extent.z ? 1:2);
        const uint32_t half=count/2;
        std::nth_element(m_order.begin()+first,m_order.begin()+first+half,m_order.begin()+first+count,
            [&](uint32_t a,uint32_t b) { return (m_bounds[a].minimum[axis]+m_bounds[a].maximum[axis]) <
                (m_bounds[b].minimum[axis]+m_bounds[b].maximum[axis]); });
        const auto left=BuildNode(first,half,index,depth+1), right=BuildNode(first+half,count-half,index,depth+1);
        m_nodes[index].left=left; m_nodes[index].right=right; m_nodes[index].count=0;
    }
    else for(uint32_t i=0;i<count;++i) m_leafForItem[m_order[first+i]]=index;
    return index;
}

void RenderSpatialIndex::Rebuild()
{
    m_order.clear(); m_unbounded.clear(); m_nodes.clear(); m_dirtyLevels.clear();
    m_leafForItem.assign(m_bounds.size(),NoNode);
    for(uint32_t i=0;i<m_bounds.size();++i)
        (m_bounds[i].bounded ? m_order:m_unbounded).push_back(i);
    if(!m_order.empty()) BuildNode(0,uint32_t(m_order.size()));
    m_marks.assign(m_nodes.size(),0); m_costDeltas.resize(m_nodes.size());
    m_generation=0; m_refitPasses=0; m_treeCost=0;
    for(const auto& node:m_nodes) m_treeCost+=node.area;
    m_baselineCost=m_treeCost;
}

RenderSpatialIndex::UpdateStatistics RenderSpatialIndex::Update(std::vector<RenderBounds> bounds)
{
    using Clock=std::chrono::steady_clock;
    const auto start=Clock::now();
    const auto ms=[](auto first,auto last) { return std::chrono::duration<double,std::milli>(last-first).count(); };
    UpdateStatistics stats;
    if(bounds.size()>UINT32_MAX) throw std::length_error("Spatial item count exceeds index range");
    bool membershipChanged=bounds.size()!=m_bounds.size() || !m_ready; m_changed.clear();
    const auto common=std::min(bounds.size(),m_bounds.size());
    for(uint32_t i=0;i<common;++i) if(!(bounds[i]==m_bounds[i]))
    {
        m_changed.push_back(i);
        membershipChanged=membershipChanged || bounds[i].bounded!=m_bounds[i].bounded;
    }
    stats.changedBounds=m_changed.size()+std::max(bounds.size(),m_bounds.size())-common;
    stats.reusedBounds=common-m_changed.size();
    const auto compared=Clock::now(); stats.compareMs=ms(start,compared);
    if(!membershipChanged && m_changed.empty()) { stats.totalMs=stats.compareMs; return stats; }
    if(!m_incremental) membershipChanged=true;
    for(auto& view:m_views) view.valid=false;
    m_ready=false; m_bounds=std::move(bounds);
    if(membershipChanged)
    {
        Rebuild(); stats.rebuilds=1; stats.buildMs=ms(compared,Clock::now());
    }
    else
    {
        if(++m_generation==0) { std::fill(m_marks.begin(),m_marks.end(),0); ++m_generation; }
        for(auto& level:m_dirtyLevels) level.clear();
        for(auto item:m_changed)
        {
            auto node=m_leafForItem[item];
            if(node==NoNode || m_marks[node]==m_generation) continue;
            ++stats.dirtyLeaves;
            while(node!=NoNode && m_marks[node]!=m_generation)
            {
                m_marks[node]=m_generation;
                m_dirtyLevels[m_nodes[node].depth].push_back(node);
                node=m_nodes[node].parent;
            }
        }
        for(size_t depth=m_dirtyLevels.size();depth>0;--depth)
        {
            const auto& level=m_dirtyLevels[depth-1];
            if(level.empty()) continue;
            stats.refitNodes+=level.size();
            const auto refit=[&](size_t task) {
                const auto index=level[task]; auto& node=m_nodes[index];
                RenderBounds updated;
                if(node.count)
                {
                    updated=m_bounds[m_order[node.first]];
                    for(uint32_t i=1;i<node.count;++i) updated=Merge(updated,m_bounds[m_order[node.first+i]]);
                }
                else updated=Merge(m_nodes[node.left].bounds,m_nodes[node.right].bounds);
                const auto area=Area(updated); m_costDeltas[index]=area-node.area;
                node.bounds=updated; node.area=area;
            };
            if(level.size()<256) for(size_t task=0;task<level.size();++task) refit(task);
            else Threading::JobScheduler::Get().For(level.size(),64,refit);
            for(auto index:level) m_treeCost+=m_costDeltas[index];
        }
        ++m_refitPasses;
        const auto refitted=Clock::now(); stats.refitMs=ms(compared,refitted);
        // Refit stays conservative even after large motion. Periodically
        // repartition a degraded tree to restore useful culling quality.
        if(m_nodes.size()>1 && m_refitPasses>=1 && m_treeCost>std::max(1.e-9,m_baselineCost)*2.)
        { Rebuild(); stats.rebuilds=stats.qualityRebuilds=1; stats.buildMs=ms(refitted,Clock::now()); }
    }
    m_ready=true; stats.totalMs=ms(start,Clock::now()); return stats;
}
}
