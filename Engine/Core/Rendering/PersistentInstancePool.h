#pragma once
#include "Core/Graphics/IGraphicsProvider.h"
#include "RenderJobs.h"
#include <glm/glm.hpp>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <vector>
#include <unordered_set>

namespace Engine::Rendering
{
struct PersistentInstance
{
    glm::vec4 rows[3]{};
    glm::uvec4 identity{0};
};
static_assert(sizeof(PersistentInstance)==64);

// Placement storage is independent of cameras, draw order, materials and LODs.
// Each graphics frame slot receives only the ranges changed since its last use.
class PersistentInstancePool
{
    struct Source
    {
        std::weak_ptr<const void> lifetime;
        uint64_t revision=0,identity=0;
        uint32_t first=0,count=0,capacity=0;
    };
    struct Change { uint64_t revision; uint32_t first,count; bool created; };
    struct Slot
    {
        uint64_t serial=UINT64_MAX,applied=0;
        std::unique_ptr<Graphics::IGraphicsBuffer> buffer;
        void* mapped=nullptr;
        // Old allocations can still be referenced by earlier passes this frame.
        std::vector<std::unique_ptr<Graphics::IGraphicsBuffer>> retired;
        std::vector<std::shared_ptr<const void>> liveSources;
        std::unordered_set<const void*> liveKeys;
        std::vector<std::pair<uint32_t,uint32_t>> retiredRanges;
        // GPU-resident mode: this frame's staged changes (Upload memory).
        // shared: command packets that reference it keep it alive until replay.
        std::shared_ptr<Graphics::IGraphicsBuffer> staging;
        void* stagingMapped=nullptr;
        size_t stagingUsed=0;
    };
    std::map<const void*,Source> m_sources;
    std::map<uint64_t,const void*> m_stableSources;
    std::map<uint32_t,uint32_t> m_free;
    std::vector<PersistentInstance> m_records;
    std::vector<Change> m_changes;
    std::vector<Slot> m_slots;
    uint32_t m_currentSlot=0;
    uint64_t m_revision=0,m_nextIdentity=1,m_serial=UINT64_MAX;
    // GPU-resident mode (backends with IGraphicsContext::CopyBufferRegion):
    // one pool in GPU memory; Upload() collects the changed record ranges and
    // FlushUploads() copies them in through the frame slot's staging buffer.
    // Read across PCIe from Upload memory, the cull and the draws paid for
    // every resident placement each frame (~10 ms GPU with the 8 km forest,
    // todo.md Step 0). ENGINE_POOL_GPU_RESIDENT=0 keeps the Upload pool (A/B).
    // shared: command packets that bind it keep it alive until replay (a
    // retired unique buffer could be freed before a late packet replayed).
    std::shared_ptr<Graphics::IGraphicsBuffer> m_gpuPool;
    uint64_t m_gpuApplied=0;
    std::vector<std::pair<uint32_t,uint32_t>> m_gpuPending; // first record, count
    uint32_t Allocate(uint32_t capacity)
    {
        for(auto it=m_free.begin();it!=m_free.end();++it) if(it->second>=capacity)
        {
            const auto first=it->first,remaining=it->second-capacity;
            m_free.erase(it); if(remaining) m_free.emplace(first+capacity,remaining);
            return first;
        }
        const auto first=uint32_t(m_records.size());
        m_records.resize(size_t(first)+capacity); return first;
    }
    void Free(uint32_t first,uint32_t count)
    {
        auto next=m_free.lower_bound(first);
        if(next!=m_free.end() && first+count==next->first) { count+=next->second; next=m_free.erase(next); }
        if(next!=m_free.begin())
        { auto previous=std::prev(next); if(previous->first+previous->second==first)
          { first=previous->first; count+=previous->second; m_free.erase(previous); } }
        m_free[first]=count;
    }
public:
    struct Statistics
    {
        uint64_t newBytes=0,editedBytes=0,growthBytes=0,allocations=0,retirements=0,hits=0;
        uint64_t preparedRecords=0,workerPasses=0;
    } stats;
    void BeginFrame(Graphics::IGraphicsProvider* provider)
    {
        auto* contexts=provider->GetContextFactory(); const auto serial=contexts->GetPreparedFrameSerial();
        if(serial==m_serial) return;
        m_serial=serial; stats={};
        const auto slot=contexts->GetPreparedFrameSlot();
        if(m_slots.size()<=slot) m_slots.resize(size_t(slot)+1);
        // The provider has waited this slot's fence before publishing its serial.
        m_currentSlot=slot;
        for(const auto& range:m_slots[slot].retiredRanges) Free(range.first,range.second);
        m_slots[slot].retiredRanges.clear();
        m_slots[slot].retired.clear(); m_slots[slot].liveSources.clear(); m_slots[slot].liveKeys.clear();
        m_slots[slot].stagingUsed=0;
        for(auto it=m_sources.begin();it!=m_sources.end();)
            if(it->second.lifetime.expired())
            {
                const auto stable=m_stableSources.find(it->second.identity);
                if(stable!=m_stableSources.end() && stable->second==it->first) m_stableSources.erase(stable);
                Free(it->second.first,it->second.capacity); it=m_sources.erase(it); ++stats.retirements;
            }
            else ++it;
    }
    uint32_t Register(const std::shared_ptr<const void>& lifetime,uint64_t revision,
        const void* transforms,uint32_t count,uint64_t identity=0,const std::vector<uint64_t>& ids={})
    {
        auto& frame=m_slots[m_currentSlot];
        if(frame.liveKeys.insert(lifetime.get()).second) frame.liveSources.push_back(lifetime);
        auto found=m_sources.find(lifetime.get());
        // ENGINE_INSTANCE_POOL_STEAL=0: a replacement never takes over the
        // allocation of a source with the same stable identity (A/B runs).
        static const bool steal=[]{ const char* v=std::getenv("ENGINE_INSTANCE_POOL_STEAL"); return !(v && v[0]=='0'); }();
        if(found==m_sources.end() && identity && steal)
        {
            // Replacing a saved tile after an edit keeps its allocation. Compare
            // the new records instead of treating the replacement as a full upload.
            const auto stable=m_stableSources.find(identity);
            if(stable!=m_stableSources.end() && !frame.liveKeys.count(stable->second))
            {
                auto previous=m_sources.extract(stable->second);
                if(!previous.empty())
                {
                    previous.key()=lifetime.get(); previous.mapped().revision=UINT64_MAX;
                    found=m_sources.insert(std::move(previous)).position;
                    stable->second=lifetime.get();
                }
            }
        }
        if(found!=m_sources.end() && found->second.revision==revision && found->second.count==count)
        { ++stats.hits; return found->second.first; }
        const bool created=found==m_sources.end();
        auto& source=m_sources[lifetime.get()];
        bool moved=created || count>source.capacity;
        if(moved)
        {
            if(source.capacity) m_slots[m_currentSlot].retiredRanges.emplace_back(source.first,source.capacity);
            source.capacity=64; while(source.capacity<count) source.capacity*=2;
            source.first=Allocate(source.capacity);
        }
        if(created) source.identity=identity ? identity:m_nextIdentity++;
        if(identity) m_stableSources.emplace(identity,lifetime.get());
        source.lifetime=lifetime; source.count=count; source.revision=UINT64_MAX;
        uint32_t dirtyFirst=UINT32_MAX,dirtyLast=0;
        // Changed sources are copied while the scene owner is stable. Workers
        // only convert immutable affine/identity snapshots, never live meshes
        // or mapped GPU memory. Publication and dirty-range merging stay here.
        struct AffineSnapshot { glm::vec4 rows[3]; };
        static_assert(sizeof(AffineSnapshot)==48);
        std::vector<AffineSnapshot> affine(count);
        if(count) std::memcpy(affine.data(),transforms,size_t(count)*sizeof(AffineSnapshot));
        const std::vector<uint64_t> identities(ids);
        const auto sourceIdentity=source.identity;
        std::vector<PersistentInstance> records(count);
        // Small edits cost less than a scheduler handoff. Large changed sources
        // amortize snapshot and worker setup; unchanged sources returned above.
        RenderJobs::Get().For(count,count>=1024 ? 256:SIZE_MAX,[&](size_t index)
        {
            auto& record=records[index];
            std::memcpy(record.rows,affine[index].rows,sizeof(record.rows));
            const uint64_t placement=index<identities.size() ? identities[index]:uint64_t(index)+1;
            record.identity={uint32_t(sourceIdentity),uint32_t(sourceIdentity>>32),uint32_t(placement),uint32_t(placement>>32)};
        });
        stats.preparedRecords+=count;
        if(count>=1024 && RenderJobs::Get().WorkerCount() && RenderJobs::Get().ParallelForEnabled()) ++stats.workerPasses;
        for(uint32_t index=0;index<count;++index)
        {
            const auto& record=records[index];
            auto& destination=m_records[source.first+index];
            if(moved || std::memcmp(&destination,&record,sizeof(record))!=0)
            {
                destination=record;
                dirtyFirst=std::min(dirtyFirst,index); dirtyLast=index;
            }
            else if(dirtyFirst!=UINT32_MAX)
            {
                m_changes.push_back({++m_revision,source.first+dirtyFirst,dirtyLast-dirtyFirst+1,created}); dirtyFirst=UINT32_MAX;
            }
        }
        if(dirtyFirst!=UINT32_MAX) m_changes.push_back({++m_revision,source.first+dirtyFirst,dirtyLast-dirtyFirst+1,created});
        source.revision=revision;
        return source.first;
    }
    const PersistentInstance& Get(uint32_t index) const { return m_records.at(index); }
    // Diagnostics (ENGINE_TRACE_LAYER): records in [first, first + count)
    // whose copy in the current slot's GPU buffer differs from the CPU copy.
    uint32_t CountStaleRecords(uint32_t first,uint32_t count) const
    {
        if(m_currentSlot>=m_slots.size() || !m_slots[m_currentSlot].mapped) return 0;
        const auto& slot=m_slots[m_currentSlot];
        const size_t capacity=slot.buffer ? size_t(slot.buffer->GetSize()/sizeof(PersistentInstance)) : 0;
        uint32_t stale=0;
        for(uint32_t i=first;i<first+count && i<m_records.size();++i)
            if(i>=capacity || std::memcmp(static_cast<const PersistentInstance*>(slot.mapped)+i,&m_records[i],sizeof(PersistentInstance))!=0) ++stale;
        return stale;
    }
    uint64_t ResidentBytes() const
    { uint64_t bytes=m_gpuPool ? m_gpuPool->GetSize() : 0; for(const auto& slot:m_slots) { if(slot.buffer) bytes+=slot.buffer->GetSize(); if(slot.staging) bytes+=slot.staging->GetSize(); for(const auto& retired:slot.retired) bytes+=retired->GetSize(); } return bytes; }
    void Retire(std::unique_ptr<Graphics::IGraphicsBuffer> buffer)
    { if(buffer) m_slots[m_currentSlot].retired.push_back(std::move(buffer)); }
    // GPU-resident mode: records the copies of the ranges Upload() collected,
    // before anything this view records reads the pool. A no-op otherwise.
    void FlushUploads(Graphics::IGraphicsContext* context, Graphics::IGraphicsProvider* provider)
    {
        if(m_gpuPending.empty() || !m_gpuPool || !context || m_currentSlot>=m_slots.size()) return;
        auto ranges=std::move(m_gpuPending); m_gpuPending.clear();
        // Merge overlapping and adjacent ranges (fewer copies and barriers).
        std::sort(ranges.begin(),ranges.end());
        std::vector<std::pair<uint32_t,uint32_t>> merged;
        for(const auto& range:ranges)
        {
            const uint32_t end=std::min<uint32_t>(range.first+range.second,uint32_t(m_records.size()));
            if(end<=range.first) continue;
            if(!merged.empty() && range.first<=merged.back().first+merged.back().second)
                merged.back().second=std::max(merged.back().second,end-merged.back().first);
            else merged.push_back({range.first,end-range.first});
        }
        size_t total=0; for(const auto& range:merged) total+=size_t(range.second)*sizeof(PersistentInstance);
        if(!total) return;
        auto& slot=m_slots[m_currentSlot];
        if(!slot.staging || slot.staging->GetSize()<slot.stagingUsed+total)
        {
            size_t capacity=size_t(1)<<20; while(capacity<total) capacity*=2;
            // Copies recorded earlier keep the old one (shared).
            slot.staging=std::shared_ptr<Graphics::IGraphicsBuffer>(provider->GetBufferFactory()->CreateBuffer(Graphics::IGraphicsBuffer::Usage::ShaderResource,
                Graphics::IGraphicsBuffer::AccessMode::Upload,capacity,nullptr,sizeof(PersistentInstance)));
            slot.stagingMapped=slot.staging ? slot.staging->Map() : nullptr;
            slot.stagingUsed=0; ++stats.allocations;
            if(!slot.stagingMapped) throw std::runtime_error("Placement pool staging is not CPU writable");
        }
        for(const auto& range:merged)
        {
            const size_t bytes=size_t(range.second)*sizeof(PersistentInstance);
            std::memcpy(static_cast<uint8_t*>(slot.stagingMapped)+slot.stagingUsed,m_records.data()+range.first,bytes);
            slot.staging->FlushMappedWrites(slot.stagingUsed,bytes);
            if(!context->CopyBufferRegion(slot.staging.get(),slot.stagingUsed,m_gpuPool.get(),
                size_t(range.first)*sizeof(PersistentInstance),bytes))
                throw std::runtime_error("Placement pool copy failed");
            slot.stagingUsed+=bytes;
        }
    }
    Graphics::IGraphicsBuffer* Upload(Graphics::IGraphicsProvider* provider)
    {
        const auto index=provider->GetContextFactory()->GetPreparedFrameSlot();
        if(m_slots.size()<=index) m_slots.resize(size_t(index)+1);
        auto& slot=m_slots[index];
        const size_t required=std::max<size_t>(64,m_records.size())*sizeof(PersistentInstance);
        static const bool gpuResidentAllowed=[]{ const char* v=std::getenv("ENGINE_POOL_GPU_RESIDENT"); return !(v && v[0]=='0'); }();
        if(gpuResidentAllowed && provider->GetBufferFactory()->SupportsBufferRegionCopy())
        {
            if(!m_gpuPool || m_gpuPool->GetSize()<required)
            {
                size_t capacity=4096; while(capacity<required) capacity*=2;
                // Packets recorded earlier keep the old buffer (shared).
                m_gpuPool=std::shared_ptr<Graphics::IGraphicsBuffer>(provider->GetBufferFactory()->CreateBuffer(Graphics::IGraphicsBuffer::Usage::ShaderResource,
                    Graphics::IGraphicsBuffer::AccessMode::Default,capacity,nullptr,sizeof(PersistentInstance)));
                ++stats.allocations; stats.growthBytes+=capacity;
                m_gpuPending.clear();
                if(!m_records.empty()) m_gpuPending.push_back({0u,uint32_t(m_records.size())});
            }
            else for(const auto& change:m_changes) if(change.revision>m_gpuApplied)
            {
                m_gpuPending.push_back({change.first,change.count});
                const size_t bytes=size_t(change.count)*sizeof(PersistentInstance);
                if(change.created) stats.newBytes+=bytes; else stats.editedBytes+=bytes;
            }
            m_gpuApplied=m_revision;
            m_changes.clear();
            return m_gpuPool.get();
        }
        if(!slot.buffer || slot.buffer->GetSize()<required)
        {
            size_t capacity=4096; while(capacity<required) capacity*=2;
            if(slot.buffer) slot.retired.push_back(std::move(slot.buffer));
            slot.buffer=provider->GetBufferFactory()->CreateBuffer(Graphics::IGraphicsBuffer::Usage::ShaderResource,
                Graphics::IGraphicsBuffer::AccessMode::Upload,capacity,nullptr,sizeof(PersistentInstance));
            ++stats.allocations;
            slot.mapped=slot.buffer->Map();
            if(!slot.mapped) throw std::runtime_error("Persistent instance pool is not CPU writable");
            std::memset(slot.mapped,0,capacity);
            std::memcpy(slot.mapped,m_records.data(),m_records.size()*sizeof(PersistentInstance));
            slot.buffer->FlushMappedWrites(0,capacity); stats.growthBytes+=capacity; slot.applied=m_revision;
        }
        else for(const auto& change:m_changes) if(change.revision>slot.applied)
        {
            const size_t offset=size_t(change.first)*sizeof(PersistentInstance),bytes=size_t(change.count)*sizeof(PersistentInstance);
            std::memcpy(static_cast<uint8_t*>(slot.mapped)+offset,m_records.data()+change.first,bytes);
            slot.buffer->FlushMappedWrites(offset,bytes); if(change.created) stats.newBytes+=bytes; else stats.editedBytes+=bytes;
        }
        slot.applied=m_revision;
        uint64_t oldest=m_revision;
        for(const auto& other:m_slots) if(other.buffer) oldest=std::min(oldest,other.applied);
        m_changes.erase(std::remove_if(m_changes.begin(),m_changes.end(),[&](const auto& change){return change.revision<=oldest;}),m_changes.end());
        return slot.buffer.get();
    }
};
}
