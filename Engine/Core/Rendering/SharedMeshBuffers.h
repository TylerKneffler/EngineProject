#pragma once
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Serialization/PrefabLoadProfile.h"
#include <cstring>
#include <algorithm>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace Engine::Rendering
{
struct PreparedMeshBuffer
{
    std::vector<unsigned char> bytes;
    uint64_t hash = 1469598103934665603ull;
};
inline uint64_t HashMeshBuffer(const unsigned char* bytes, size_t size)
{
    uint64_t hash = 1469598103934665603ull;
    size_t offset = 0;
    for (; size - offset >= sizeof(uint64_t); offset += sizeof(uint64_t))
    {
        uint64_t word;
        std::memcpy(&word, bytes + offset, sizeof(word));
        hash = (hash ^ word) * 1099511628211ull;
    }
    for (; offset < size; ++offset)
        hash = (hash ^ bytes[offset]) * 1099511628211ull;
    return hash;
}
inline PreparedMeshBuffer PrepareMeshBuffer(const void* data, size_t size)
{
    PreparedMeshBuffer result;
    const auto* bytes = static_cast<const unsigned char*>(data);
    result.bytes.assign(bytes, bytes + size);
    result.hash = HashMeshBuffer(result.bytes.data(), size);
    return result;
}
// Content sharing also covers generated prefab geometry with different asset
// paths. The byte comparison protects against hash collisions. Weak entries
// release their device resources with the final mesh owner.
inline std::shared_ptr<Graphics::IGraphicsBuffer> AcquireMeshBuffer(
    Graphics::IGraphicsBufferFactory* factory, Graphics::IGraphicsBuffer::Usage usage,
    const void* data, size_t size, uint32_t stride,
    PreparedMeshBuffer* prepared = nullptr)
{
    struct Entry { std::vector<unsigned char> bytes; std::unique_ptr<Graphics::IGraphicsBuffer> buffer; };
    using Key = std::tuple<const void*, int, size_t, uint32_t, uint64_t>;
    static std::map<Key, std::vector<std::weak_ptr<Entry>>> cache;
    static std::mutex mutex;
    const bool usePrepared = prepared && prepared->bytes.size() == size;
    const auto* bytes = usePrepared ? prepared->bytes.data() : static_cast<const unsigned char*>(data);
    uint64_t hash;
    {
    Serialization::PrefabLoadProfile::Stage stage("mesh_buffer_hash");
    // This is only an in-process bucket key: equality still checks every byte.
    // Mix words to avoid a serial multiply for every byte of large meshes.
    // memcpy supports unaligned inputs without aliasing or overreading tails.
    hash = usePrepared ? prepared->hash : HashMeshBuffer(bytes, size);
    }
    const Key key{factory, int(usage), size, stride, hash};
    std::lock_guard<std::mutex> lock(mutex);
    auto& entries = cache[key];
    {
    Serialization::PrefabLoadProfile::Stage stage("mesh_buffer_cache_probe");
    for (auto it = entries.begin(); it != entries.end();)
    {
        auto entry = it->lock();
        if (!entry) { it = entries.erase(it); continue; }
        if (std::memcmp(entry->bytes.data(), bytes, size) == 0)
            return {entry, entry->buffer.get()};
        ++it;
    }
    }
    auto entry = std::make_shared<Entry>();
    {
    Serialization::PrefabLoadProfile::Stage stage("mesh_buffer_cpu_mirror");
    if (usePrepared) entry->bytes = std::move(prepared->bytes);
    else entry->bytes.assign(bytes, bytes + size);
    }
    {
    Serialization::PrefabLoadProfile::Stage stage("mesh_buffer_backend_create");
    entry->buffer = usage == Graphics::IGraphicsBuffer::Usage::VertexBuffer
        ? (usePrepared ? factory->CreateStaticVertexBufferDeferred(size, entry->bytes.data())
                       : factory->CreateStaticVertexBuffer(size, entry->bytes.data()))
        : factory->CreateBuffer(usage, Graphics::IGraphicsBuffer::AccessMode::Upload, size, entry->bytes.data(), stride);
    }
    if (!entry->buffer) return {};
    entries.push_back(entry);
    if (cache.size() > 2048)
        for (auto it = cache.begin(); it != cache.end();)
        {
            auto& list = it->second;
            list.erase(std::remove_if(list.begin(), list.end(), [](auto& value) { return value.expired(); }), list.end());
            if (list.empty()) it = cache.erase(it); else ++it;
        }
    return {entry, entry->buffer.get()};
}
}
