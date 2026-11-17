#pragma once
#include "PersistentInstancePool.h"
#include "Core/Graphics/IGraphicsContext.h"
#include <array>

namespace Engine::Rendering
{
struct InstanceTileRecord
{
    glm::vec4 world[3]{};
    glm::vec4 minimum{0}; glm::uvec4 range{0}; glm::vec4 maximum{0};
    glm::vec4 origin{0}; glm::uvec4 reserved{0}; glm::vec4 lod{0};
};
static_assert(sizeof(InstanceTileRecord)==144);
inline void AppendInstanceCullChunks(std::vector<InstanceTileRecord>& records,
    const InstanceTileRecord& tile)
{
    constexpr uint32_t placementsPerGroup = 256;
    for (uint32_t offset = 0; offset < tile.range.y;)
    {
        auto chunk = tile;
        chunk.range.x += offset;
        chunk.range.y = std::min(placementsPerGroup, tile.range.y - offset);
        records.push_back(chunk);
        offset += chunk.range.y;
    }
}
struct VisibleInstanceRecord { glm::uvec4 rows[3]{}; };

class PersistentInstanceBatches
{
public:
    using Key=std::vector<uintptr_t>;
    struct Batch
    {
        uint64_t serial=0;
        bool gpu=false,metadataChanged=false;
        bool cold=false,membershipChanged=false,modeChanged=false;
        uint32_t candidates=0;
        uint64_t metadataUploadBytes=0,idUploadBytes=0,allocations=0;
        std::vector<InstanceTileRecord> previous;
        std::vector<VisibleInstanceRecord> ids;
        std::array<uint8_t,256> constants{};
        std::unique_ptr<Graphics::IGraphicsBuffer> input,output,draw;
        std::unique_ptr<Graphics::IGraphicsBuffer> visibilityReadback;
        bool visibilityReadbackPending=false;
        uint32_t completedGpuCandidates=0,completedTransitionCandidates=0,readbackLod=0;
        bool completedReadback=false,readbackShadow=false;
        uint64_t readbackViewIdentity=0;
    };
private:
    // A batch unused this many frames gives its buffers to the spare list.
    static constexpr uint64_t SpareAfterFrames=8;
    static constexpr size_t MaxSpareBatches=256;
    struct Slot { uint64_t serial=UINT64_MAX; std::map<Key,Batch> batches;
        std::vector<Batch> spare;
        std::vector<std::unique_ptr<Graphics::IGraphicsBuffer>> retired;
        Slot()=default; Slot(const Slot&)=delete; Slot& operator=(const Slot&)=delete;
        Slot(Slot&&) noexcept=default; Slot& operator=(Slot&&) noexcept=default; };
    std::vector<Slot> m_slots;
public:
    Batch& Acquire(Graphics::IGraphicsProvider* provider,Key key,bool gpu,
        const std::vector<InstanceTileRecord>& tiles,const void* drawData,
        const PersistentInstancePool& pool,uint64_t& uploadBytes)
    {
        auto* contexts=provider->GetContextFactory(); const auto index=contexts->GetPreparedFrameSlot(); const auto serial=contexts->GetPreparedFrameSerial();
        if(m_slots.size()<=index) m_slots.resize(size_t(index)+1);
        auto& slot=m_slots[index];
        if(slot.serial!=serial)
        {
            // The context factory has waited this slot's GPU fence.
            for (auto& entry : slot.batches)
            {
                auto& previous = entry.second;
                if (!previous.visibilityReadbackPending || !previous.visibilityReadback)
                    continue;
                if (!previous.visibilityReadback->IsReadbackReady()) continue;
                if (auto* mapped = static_cast<const uint8_t*>(previous.visibilityReadback->Map()))
                {
                    std::memcpy(&previous.completedGpuCandidates, mapped + sizeof(uint32_t),
                        sizeof(previous.completedGpuCandidates));
                    std::memcpy(&previous.completedTransitionCandidates, mapped + 20,
                        sizeof(previous.completedTransitionCandidates));
                    previous.visibilityReadback->Unmap();
                    previous.completedReadback = true;
                    previous.visibilityReadbackPending = false;
                }
            }
            slot.serial=serial;
            slot.retired.clear();
            // This slot's fence has passed, so buffers of batches it no longer
            // uses are idle. Park them for reuse by new keys (bounded).
            for(auto it=slot.batches.begin();it!=slot.batches.end();)
                if(serial>it->second.serial && serial-it->second.serial>SpareAfterFrames)
                {
                    if(it->second.output && slot.spare.size()<MaxSpareBatches)
                    {
                        it->second.visibilityReadbackPending=false;
                        slot.spare.push_back(std::move(it->second));
                    }
                    it=slot.batches.erase(it);
                }
                else ++it;
        }
        auto [found,inserted]=slot.batches.try_emplace(std::move(key));
        auto& batch=found->second;
        if(inserted && !slot.spare.empty())
        {
            // Keys change whenever streaming replaces a bound texture or mesh
            // buffer, which happens constantly while the view turns. The
            // buffers below do not depend on those resources; recycle a
            // retired batch's buffers instead of creating new ones.
            auto& spare=slot.spare.back();
            batch.input=std::move(spare.input); batch.output=std::move(spare.output);
            batch.draw=std::move(spare.draw); batch.visibilityReadback=std::move(spare.visibilityReadback);
            batch.gpu=spare.gpu; batch.constants=spare.constants;
            slot.spare.pop_back();
        }
        batch.cold=inserted || !batch.output;
        batch.modeChanged=!batch.cold && batch.gpu!=gpu;
        batch.membershipChanged=!batch.cold && batch.previous.size()!=tiles.size();
        if(!batch.cold && !batch.membershipChanged)
            for(size_t i=0;i<tiles.size();++i)
                if(batch.previous[i].range!=tiles[i].range) { batch.membershipChanged=true; break; }
        batch.serial=serial; batch.metadataUploadBytes=0; batch.idUploadBytes=0; batch.allocations=0;
        const auto bytes=tiles.size()*sizeof(InstanceTileRecord);
        batch.metadataChanged=batch.cold || batch.gpu!=gpu || batch.previous.size()!=tiles.size() ||
            std::memcmp(batch.previous.data(),tiles.data(),bytes)!=0;
        auto* factory=provider->GetBufferFactory();
        if (gpu && !batch.visibilityReadback)
            batch.visibilityReadback=factory->CreateBuffer(
                Graphics::IGraphicsBuffer::Usage::StorageBuffer,
                Graphics::IGraphicsBuffer::AccessMode::Readback,48,nullptr,48);
        batch.candidates=0; for(const auto& tile:tiles) batch.candidates+=tile.range.y;
        if(gpu && (!batch.input || batch.metadataChanged))
        {
            const size_t capacity=std::max<size_t>(sizeof(InstanceTileRecord),bytes);
            if(!batch.input || batch.input->GetSize()<capacity)
            {
                if (batch.input) slot.retired.push_back(std::move(batch.input));
                batch.input=factory->CreateBuffer(Graphics::IGraphicsBuffer::Usage::ShaderResource,Graphics::IGraphicsBuffer::AccessMode::Upload,capacity,tiles.data(),48); ++batch.allocations;
            }
            else if(auto* mapped=batch.input->Map())
            { std::memcpy(mapped,tiles.data(),bytes); batch.input->FlushMappedWrites(0,bytes); }
            batch.metadataUploadBytes=bytes; uploadBytes+=bytes;
        }
        const size_t outputBytes=size_t(batch.candidates+1)*sizeof(VisibleInstanceRecord);
        if(!batch.output || batch.gpu!=gpu || batch.output->GetSize()<outputBytes)
        {
            size_t capacity=48*64; while(capacity<outputBytes) capacity*=2;
            if (batch.output) slot.retired.push_back(std::move(batch.output));
            batch.output=factory->CreateBuffer(gpu ? Graphics::IGraphicsBuffer::Usage::StorageBuffer:Graphics::IGraphicsBuffer::Usage::ShaderResource,
                gpu ? Graphics::IGraphicsBuffer::AccessMode::Default:Graphics::IGraphicsBuffer::AccessMode::Upload,capacity,nullptr,48);
            ++batch.allocations; batch.metadataChanged=true;
        }
        if(!gpu && batch.metadataChanged)
        {
            batch.ids.assign(size_t(batch.candidates)+1,{});
            size_t destination=1;
            for(const auto& tile:tiles) for(uint32_t i=0;i<tile.range.y;++i)
            {
                auto& visible=batch.ids[destination++]; visible.rows[0]={tile.range.z,tile.range.x+i,0,0};
                visible.rows[1]=pool.Get(tile.range.x+i).identity;
            }
            if(auto* mapped=batch.output->Map())
            { std::memcpy(mapped,batch.ids.data(),outputBytes); batch.output->FlushMappedWrites(0,outputBytes); }
            batch.idUploadBytes=outputBytes; uploadBytes+=outputBytes;
        }
        batch.gpu=gpu; batch.previous=tiles;
        std::array<uint8_t,256> constants{}; std::memcpy(constants.data(),drawData,16);
        if(!batch.draw || batch.constants!=constants)
        {
            if(!batch.draw) { batch.draw=factory->CreateBuffer(Graphics::IGraphicsBuffer::Usage::ConstantBuffer,Graphics::IGraphicsBuffer::AccessMode::Upload,256,constants.data()); ++batch.allocations; }
            else if(auto* mapped=batch.draw->Map()) { std::memcpy(mapped,constants.data(),256); batch.draw->Unmap(); }
            batch.constants=constants; uploadBytes+=256;
        }
        return batch;
    }
    static bool ScheduleVisibilityReadback(Graphics::IGraphicsContext* context,
        Batch& batch, uint32_t lod, bool shadow, uint64_t viewIdentity)
    {
        if (!context || !batch.gpu || !batch.output || !batch.visibilityReadback ||
            batch.visibilityReadbackPending)
            return false;
        // The counters occupy the first structured record. Copy the complete
        // element so backends requiring structure-aligned regions can read it.
        if (!context->CopyBuffer(batch.output.get(), batch.visibilityReadback.get(),
            sizeof(VisibleInstanceRecord)))
            return false;
        batch.readbackLod=lod;
        batch.readbackShadow=shadow;
        batch.readbackViewIdentity=viewIdentity;
        batch.visibilityReadbackPending=true;
        return true;
    }
};
}
