#include "pch.h"
#include "TransformHierarchy.h"
#include "Transform.h"
#include "Core/Object.h"
#include "Core/Threading/JobScheduler.h"
#include <chrono>
#include <unordered_map>

namespace Engine::Components
{
glm::mat4 TransformHierarchy::ReadPreparedWorldMatrix(const Transform& transform) const
{
    // Root validation is already constant-time and cheaper than a dictionary lookup.
    if(!transform.Owner || !transform.Owner->Parent) return transform.GetWorldMatrix();
    return m_topologyValid && m_indices.find(&transform)!=m_indices.end()
        ? transform.m_cachedWorldMatrix:transform.GetWorldMatrix();
}

TransformHierarchy::Statistics TransformHierarchy::Prepare(const std::vector<Transform*>& transforms)
{
    using Clock=std::chrono::steady_clock;
    const auto start=Clock::now();
    const auto ms=[](auto first,auto last) { return std::chrono::duration<double,std::milli>(last-first).count(); };
    Statistics stats;
    bool rebuild=!m_topologyValid || transforms!=m_sources;
    auto& parents=m_parentScratch; parents.clear(); parents.reserve(transforms.size());
    for(auto* transform:transforms)
    {
        if(!transform) { stats.valid=false; stats.totalMs=ms(start,Clock::now()); return stats; }
        auto* parent=transform->Owner ? transform->Owner->Parent:nullptr;
        parents.push_back(parent ? &parent->transform:nullptr);
    }
    rebuild=rebuild || parents!=m_parents;
    if(rebuild)
    {
        m_topologyValid=false; m_levels.clear();
        // Retain dictionary storage across streaming additions/removals. Flat
        // adjacency arrays avoid allocating a child vector for every object.
        if(++m_generation==0) { m_indices.clear(); ++m_generation; }
        m_indices.reserve(transforms.size());
        for(size_t i=0;i<transforms.size();++i)
        {
            auto [entry,inserted]=m_indices.try_emplace(transforms[i],Index{i,m_generation});
            if(!inserted)
            {
                if(entry->second.generation==m_generation) { stats.valid=false; stats.totalMs=ms(start,Clock::now()); return stats; }
                entry->second={i,m_generation};
            }
        }
        for(auto it=m_indices.begin();it!=m_indices.end();)
            if(it->second.generation!=m_generation) it=m_indices.erase(it); else ++it;
        m_nodes.resize(transforms.size()); m_firstChild.assign(transforms.size(),NoParent);
        m_nextSibling.resize(transforms.size()); m_order.clear(); m_order.reserve(transforms.size());
        for(size_t i=0;i<transforms.size();++i)
        {
            const auto parent=m_indices.find(parents[i]);
            const auto parentIndex=parent==m_indices.end() ? NoParent:parent->second.position;
            m_nodes[i].parent=parentIndex;
            if(parentIndex==NoParent) m_order.push_back(i);
            else { m_nextSibling[i]=m_firstChild[parentIndex]; m_firstChild[parentIndex]=i; }
        }
        size_t begin=0,end=m_order.size();
        while(begin<end)
        {
            m_levels.emplace_back(begin,end);
            for(size_t i=begin;i<end;++i)
                for(auto child=m_firstChild[m_order[i]];child!=NoParent;child=m_nextSibling[child]) m_order.push_back(child);
            begin=end; end=m_order.size();
        }
        if(m_order.size()!=transforms.size()) { stats.valid=false; stats.totalMs=ms(start,Clock::now()); return stats; }
        m_sources=transforms; m_parents.swap(parents); m_topologyValid=true;
        stats.topologyRebuilt=true;
    }
    stats.levels=m_levels.size();
    if(!rebuild)
    {
        bool changed=false;
        for(size_t i=0;i<transforms.size();++i)
        {
            const auto& source=*transforms[i];
            if(!source.m_localCacheInitialized || !source.m_worldCacheInitialized ||
                source.position!=source.m_cachedPosition || source.rotation!=source.m_cachedRotation ||
                source.scale!=source.m_cachedScale || source.m_localRevision!=source.m_cachedLocalRevisionForWorld)
            { changed=true; break; }
            if(m_parents[i])
            {
                if(m_nodes[i].parent==NoParent) m_parents[i]->GetWorldMatrixD();
                if(m_parents[i]->m_worldRevision!=source.m_cachedParentWorldRevision) { changed=true; break; }
            }
        }
        if(!changed)
        {
            stats.worldReused=transforms.size(); stats.totalMs=ms(start,Clock::now());
            stats.snapshotMs=stats.totalMs; return stats;
        }
    }
    // No workers touch a Transform, Object, or lazy accessor. Snapshot capture
    // and cache publication belong to the caller's scene owner.
    for(size_t i=0;i<transforms.size();++i)
    {
        const auto& source=*transforms[i]; auto& node=m_nodes[i];
        const bool changed=source.m_localCacheInitialized && (source.position!=source.m_cachedPosition ||
            source.rotation!=source.m_cachedRotation || source.scale!=source.m_cachedScale);
        node.localRevision=source.m_localRevision;
        if(changed) Transform::AdvanceRevision(node.localRevision);
        node.localDirty=!source.m_localCacheInitialized || changed;
        if(node.localDirty)
        { node.position=source.position; node.rotation=source.rotation; node.scale=source.scale; }
        node.worldRevision=source.m_worldRevision;
        node.worldInitialized=source.m_worldCacheInitialized;
        node.cachedLocalRevision=source.m_cachedLocalRevisionForWorld;
        node.cachedParentRevision=source.m_cachedParentWorldRevision;
        const auto* ownerParent=source.Owner ? source.Owner->Parent:nullptr;
        node.parentChanged=source.m_cachedParent!=ownerParent;
        node.parentRevision=0;
        if(node.parent==NoParent) node.externalWorld=glm::dmat4(1);
        if(node.parent==NoParent && m_parents[i])
        {
            // Parents outside a partial batch are validated once by the owner.
            node.externalWorld=m_parents[i]->GetWorldMatrixD();
            node.parentRevision=m_parents[i]->m_worldRevision;
        }
        stats.localUpdated+=node.localDirty;
    }
    const auto captured=Clock::now();
    auto& dirty=m_dirtyScratch; dirty.reserve(transforms.size());
    for(const auto& level:m_levels)
    {
        dirty.clear();
        for(size_t order=level.first;order<level.second;++order)
        {
            const auto index=m_order[order];
            auto& node=m_nodes[index];
            if(node.parent!=NoParent) node.parentRevision=m_nodes[node.parent].worldRevision;
            const bool dependenciesChanged=node.worldInitialized && (node.cachedLocalRevision!=node.localRevision ||
                node.parentChanged || node.cachedParentRevision!=node.parentRevision);
            node.worldDirty=!node.worldInitialized || dependenciesChanged;
            if(dependenciesChanged) Transform::AdvanceRevision(node.worldRevision);
            if(node.worldDirty)
            {
                if(!node.localDirty) node.localD=transforms[index]->m_cachedLocalMatrixD;
                if(node.parent!=NoParent && !m_nodes[node.parent].worldDirty)
                    m_nodes[node.parent].worldD=transforms[node.parent]->m_cachedWorldMatrixD;
                dirty.push_back(index); ++stats.worldUpdated;
            }
            else ++stats.worldReused;
        }
        // Joining each level establishes parent-before-child ordering. Narrow
        // levels stay serial; independent wide levels use workload-sized jobs.
        if(dirty.empty()) continue;
        Threading::JobScheduler::Get().For(dirty.size(),32,[&](size_t task) {
            auto& node=m_nodes[dirty[task]];
            if(node.localDirty)
            {
                node.localD=Transform::ComposeLocalMatrix(node.position,node.rotation,node.scale);
                node.local=glm::mat4(node.localD);
            }
            node.worldD=(node.parent==NoParent ? node.externalWorld:m_nodes[node.parent].worldD)*node.localD;
            node.world=glm::mat4(node.worldD);
        });
    }
    const auto computed=Clock::now();
    for(size_t i=0;i<transforms.size();++i)
    {
        auto& target=*transforms[i]; const auto& node=m_nodes[i];
        if(node.localDirty)
        {
            target.m_cachedPosition=node.position; target.m_cachedRotation=node.rotation; target.m_cachedScale=node.scale;
            target.m_cachedLocalMatrixD=node.localD; target.m_cachedLocalMatrix=node.local;
            target.m_localRevision=node.localRevision; target.m_localCacheInitialized=true;
        }
        if(node.worldDirty)
        {
            target.m_cachedWorldMatrixD=node.worldD; target.m_cachedWorldMatrix=node.world;
            target.m_worldRevision=node.worldRevision; target.m_cachedParent=target.Owner ? target.Owner->Parent:nullptr;
            target.m_cachedLocalRevisionForWorld=node.localRevision; target.m_cachedParentWorldRevision=node.parentRevision;
            target.m_worldCacheInitialized=true;
        }
    }
    const auto committed=Clock::now();
    stats.snapshotMs=ms(start,captured); stats.computeMs=ms(captured,computed);
    stats.commitMs=ms(computed,committed); stats.totalMs=ms(start,committed);
    return stats;
}
}
