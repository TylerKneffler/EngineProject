#pragma once
#include "Core/Graphics/IGraphicsProvider.h"
#include <glm/glm.hpp>
#include <array>
#include <cstring>
#include <vector>

namespace Engine::Rendering
{
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
