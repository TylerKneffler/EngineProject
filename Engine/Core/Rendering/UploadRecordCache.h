#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Engine::Rendering
{
// Compare normal CPU memory, never mapped upload memory (which may be write
// combined). The owner prepares storage before workers write disjoint records.
// Keep each cache with the GPU buffer it mirrors, including frame/pass rotation.
class UploadRecordCache
{
public:
    void Clear() { m_bytes.clear(); m_valid.clear(); m_stride=0; }
    void Prepare(size_t records,size_t stride)
    {
        if(!stride || records>std::numeric_limits<size_t>::max()/stride)
            throw std::length_error("Invalid upload record cache size");
        if(m_valid.size()==records && m_stride==stride) return;
        m_bytes.resize(records*stride);
        m_valid.assign(records,0);
        m_stride=stride;
    }
    bool Write(size_t index,const void* source,void* mappedBuffer)
    {
        if(index>=m_valid.size()) throw std::out_of_range("Upload record slot");
        auto* cached=m_bytes.data()+index*m_stride;
        if(m_valid[index] && std::memcmp(cached,source,m_stride)==0) return false;
        std::memcpy(static_cast<uint8_t*>(mappedBuffer)+index*m_stride,source,m_stride);
        std::memcpy(cached,source,m_stride);
        m_valid[index]=1;
        return true;
    }
    // Use after an external writer overwrites a slot without updating its mirror.
    void Invalidate(size_t index) { if(index<m_valid.size()) m_valid[index]=0; }
private:
    std::vector<uint8_t> m_bytes,m_valid;
    size_t m_stride=0;
};
}
