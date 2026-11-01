#pragma once
#include "Core/Graphics/IGraphicsBuffer.h"
#include <cstring>
#include <algorithm>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace Engine::Rendering
{
// Content sharing also covers generated prefab geometry with different asset
// paths. The byte comparison protects against hash collisions. Weak entries
// release their device resources with the final mesh owner.
inline std::shared_ptr<Graphics::IGraphicsBuffer> AcquireMeshBuffer(
    Graphics::IGraphicsBufferFactory* factory, Graphics::IGraphicsBuffer::Usage usage,
    const void* data, size_t size, uint32_t stride)
{
    struct Entry { std::vector<unsigned char> bytes; std::unique_ptr<Graphics::IGraphicsBuffer> buffer; };
    using Key = std::tuple<const void*, int, size_t, uint32_t, uint64_t>;
    static std::map<Key, std::vector<std::weak_ptr<Entry>>> cache;
    static std::mutex mutex;
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    const Key key{factory, int(usage), size, stride, hash};
    std::lock_guard<std::mutex> lock(mutex);
    auto& entries = cache[key];
    for (auto it = entries.begin(); it != entries.end();)
    {
        auto entry = it->lock();
        if (!entry) { it = entries.erase(it); continue; }
        if (std::memcmp(entry->bytes.data(), data, size) == 0)
            return {entry, entry->buffer.get()};
        ++it;
    }
    auto entry = std::make_shared<Entry>();
    entry->bytes.assign(bytes, bytes + size);
    entry->buffer = usage == Graphics::IGraphicsBuffer::Usage::VertexBuffer
        ? factory->CreateStaticVertexBuffer(size, data)
        : factory->CreateBuffer(usage, Graphics::IGraphicsBuffer::AccessMode::Upload, size, data, stride);
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
