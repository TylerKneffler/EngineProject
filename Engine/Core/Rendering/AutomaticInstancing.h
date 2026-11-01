#pragma once
#include "Core/Graphics/IGraphicsProvider.h"
#include <glm/glm.hpp>
#include <array>
#include <cstring>
#include <vector>

namespace Engine::Rendering
{
// Two 48-byte shader records per placement: affine transform and object ID.
// Retaining the existing shader stride keeps the fallback and custom shaders
// compatible on every backend. Object records carry tile fades and lighting.
struct ExplicitInstanceRecord { glm::vec4 rows[3]{}; };
struct ExplicitInstanceMember
{
    std::shared_ptr<const void> lifetime;
    uint64_t revision=0;
    uint32_t object=0,first=0,count=0;
    glm::mat4 world{1};
    glm::vec3 minimum{0},maximum{0};
    glm::vec2 lodDistances{0};
    bool operator==(const ExplicitInstanceMember& other) const
    { return lifetime==other.lifetime && revision==other.revision && object==other.object && first==other.first && count==other.count && world==other.world && minimum==other.minimum && maximum==other.maximum && lodDistances==other.lodDistances; }
};
class ExplicitInstanceBatchCache
{
public:
    struct Batch
    {
        std::vector<ExplicitInstanceMember> members;
        std::vector<ExplicitInstanceRecord> records;
        std::array<uint8_t,256> constants{};
        std::shared_ptr<Graphics::IGraphicsBuffer> input;
        std::unique_ptr<Graphics::IGraphicsBuffer> instances,draw;
        bool gpu=false;
        uint32_t instanceCount=0;
    };
private:
    struct Slot { uint64_t serial=UINT64_MAX; size_t cursor=0; std::vector<std::unique_ptr<Batch>> batches; };
    std::vector<Slot> m_slots;
public:
    std::vector<ExplicitInstanceMember> members;
    template<class Prepare> Batch& Acquire(Graphics::IGraphicsProvider* provider,bool gpu,const void* constants,
        uint64_t& uploaded,Prepare&& prepare)
    {
        auto* contexts=provider->GetContextFactory(); const auto index=contexts->GetPreparedFrameSlot();
        if(m_slots.size()<=index) m_slots.resize(size_t(index)+1);
        auto& slot=m_slots[index];
        if(slot.serial!=contexts->GetPreparedFrameSerial()) { slot.serial=contexts->GetPreparedFrameSerial(); slot.cursor=0; }
        if(slot.cursor==slot.batches.size()) slot.batches.push_back(std::make_unique<Batch>());
        auto& batch=*slot.batches[slot.cursor++];
        if(!batch.instances || batch.gpu!=gpu || batch.members!=members)
        {
            auto* factory=provider->GetBufferFactory();
            Batch* shared=nullptr;
            if(gpu) for(const auto& candidate:slot.batches)
                if(candidate.get()!=&batch && candidate->gpu && candidate->input && candidate->members==members)
                { shared=candidate.get(); break; }
            size_t bytes=0;
            if(shared)
            { batch.input=shared->input; batch.instanceCount=shared->instanceCount; bytes=batch.input->GetSize(); }
            else
            {
                prepare(batch.records);
                bytes=batch.records.size()*sizeof(ExplicitInstanceRecord);
                if(gpu)
                {
                    if(!batch.input || batch.input->GetSize()!=bytes || batch.input.use_count()>1)
                        batch.input=factory->CreateBuffer(Graphics::IGraphicsBuffer::Usage::ShaderResource,
                            Graphics::IGraphicsBuffer::AccessMode::Upload,bytes,batch.records.data(),48);
                    else if(auto* data=batch.input->Map()) { std::memcpy(data,batch.records.data(),bytes); batch.input->Unmap(); }
                }
                else
                {
                    if(!batch.instances || batch.instances->GetSize()!=bytes || batch.gpu)
                        batch.instances=factory->CreateBuffer(Graphics::IGraphicsBuffer::Usage::ShaderResource,
                            Graphics::IGraphicsBuffer::AccessMode::Upload,bytes,batch.records.data(),48);
                    else if(auto* data=batch.instances->Map()) { std::memcpy(data,batch.records.data(),bytes); batch.instances->Unmap(); }
                }
                batch.instanceCount=uint32_t(batch.records.size()/2); uploaded+=bytes;
            }
            if(gpu && (!batch.instances || !batch.gpu || batch.instances->GetSize()!=bytes+48))
                batch.instances=factory->CreateBuffer(Graphics::IGraphicsBuffer::Usage::StorageBuffer,
                    Graphics::IGraphicsBuffer::AccessMode::Default,bytes+48,nullptr,48);
            batch.gpu=gpu; batch.members=members;
        }
        std::array<uint8_t,256> cb{}; std::memcpy(cb.data(),constants,16);
        if(!batch.draw || cb!=batch.constants)
        {
            if(!batch.draw) batch.draw=provider->GetBufferFactory()->CreateBuffer(Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
                Graphics::IGraphicsBuffer::AccessMode::Upload,256,cb.data());
            else if(auto* data=batch.draw->Map()) { std::memcpy(data,cb.data(),256); batch.draw->Unmap(); }
            batch.constants=cb; uploaded+=256;
        }
        return batch;
    }
};
struct AutomaticInstance
{
    glm::vec4 sphere{0};
    glm::uvec4 meta{0}; // object record, maximum resident LOD
    glm::vec4 lod{0};
};
struct InstanceCullConstants
{
    glm::mat4 viewProjection{1};
    glm::vec4 cameraPosition{0};
    glm::uvec4 counts{0};
    glm::ivec4 selection{-1,0,0,0};
};
class AutomaticInstanceCache
{
public:
    struct Batch
    {
        std::vector<AutomaticInstance> previousInput;
        std::vector<uint8_t> previousDraw;
        std::vector<glm::uvec4> directRecords;
        std::unique_ptr<Graphics::IGraphicsBuffer> input, output, draw;
        bool gpu = false;
    };
private:
    struct Slot { uint64_t serial=UINT64_MAX; size_t cursor=0; std::vector<std::unique_ptr<Batch>> batches; };
    std::vector<Slot> m_slots;
public:
    std::vector<AutomaticInstance> scratch;
    Batch& Acquire(Graphics::IGraphicsProvider* provider, bool gpu,
        const std::vector<AutomaticInstance>& instances, const void* drawData,
        uint64_t& uploadBytes)
    {
        auto* contexts=provider->GetContextFactory();
        const auto slotIndex=contexts->GetPreparedFrameSlot();
        const auto serial=contexts->GetPreparedFrameSerial();
        if(m_slots.size()<=slotIndex) m_slots.resize(size_t(slotIndex)+1);
        auto& slot=m_slots[slotIndex];
        if(slot.serial!=serial) { slot.serial=serial; slot.cursor=0; }
        if(slot.batches.size()<=slot.cursor) slot.batches.push_back(std::make_unique<Batch>());
        auto& batch=*slot.batches[slot.cursor++];
        auto* factory=provider->GetBufferFactory();
        const bool changed=batch.gpu!=gpu || batch.previousInput.size()!=instances.size() ||
            std::memcmp(batch.previousInput.data(),instances.data(),instances.size()*sizeof(AutomaticInstance))!=0;
        if(changed || !batch.output)
        {
            const auto bytes=instances.size()*sizeof(AutomaticInstance);
            if(gpu)
            {
                if(!batch.input || batch.input->GetSize()!=bytes)
                    batch.input=factory->CreateBuffer(Graphics::IGraphicsBuffer::Usage::ShaderResource,
                        Graphics::IGraphicsBuffer::AccessMode::Upload,bytes,instances.data(),sizeof(AutomaticInstance));
                else if(auto* mapped=batch.input->Map())
                { std::memcpy(mapped,instances.data(),bytes); batch.input->Unmap(); }
                if(!batch.output || !batch.gpu || batch.output->GetSize()!=(instances.size()+1)*48)
                    batch.output=factory->CreateBuffer(Graphics::IGraphicsBuffer::Usage::StorageBuffer,
                        Graphics::IGraphicsBuffer::AccessMode::Default,(instances.size()+1)*48,nullptr,48);
                uploadBytes+=bytes;
            }
            else
            {
                auto& records=batch.directRecords;
                records.assign((instances.size()+1)*3,glm::uvec4(0));
                for(size_t i=0;i<instances.size();++i) records[(i+1)*3].x=instances[i].meta.x;
                const size_t recordBytes=records.size()*sizeof(glm::uvec4);
                if(!batch.output || batch.gpu || batch.output->GetSize()!=recordBytes)
                    batch.output=factory->CreateBuffer(Graphics::IGraphicsBuffer::Usage::ShaderResource,
                        Graphics::IGraphicsBuffer::AccessMode::Upload,recordBytes,records.data(),48);
                else if(auto* mapped=batch.output->Map())
                { std::memcpy(mapped,records.data(),recordBytes); batch.output->Unmap(); }
                uploadBytes+=records.size()*sizeof(glm::uvec4);
            }
            batch.gpu=gpu; batch.previousInput=instances;
        }
        std::vector<uint8_t> constants(256,0);
        std::memcpy(constants.data(),drawData,16);
        if(batch.previousDraw!=constants || !batch.draw)
        {
            if(!batch.draw) batch.draw=factory->CreateBuffer(Graphics::IGraphicsBuffer::Usage::ConstantBuffer,
                Graphics::IGraphicsBuffer::AccessMode::Upload,256,constants.data());
            else if(auto* mapped=batch.draw->Map())
            { std::memcpy(mapped,constants.data(),256); batch.draw->Unmap(); }
            batch.previousDraw=std::move(constants); uploadBytes+=256;
        }
        return batch;
    }
};
}
