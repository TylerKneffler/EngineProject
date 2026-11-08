#pragma once

#include "Core/Graphics/IGraphicsTexture.h"
#include <algorithm>
#include <mutex>
#include <vector>

namespace Engine::Rendering
{
// Payload accounting for Texture uploads, not physical residency or OS usage.
// Weak handles include resources retained by command packets without extending
// their lifetime. Call Record only after successful creation on the owner thread.
struct TextureResidencySnapshot
{
    uint64_t residentPayloadBytes = 0;
    uint64_t sourcePayloadBytes = 0;
    uint64_t uploadedPayloadBytes = 0;
    uint64_t uploads = 0;
    uint64_t releasedResources = 0;
    uint64_t residentResources = 0;
    uint64_t omittedTopMips = 0;
    uint64_t allocatedBytes = 0; // exact DX12/Vulkan, conservative DX11 estimate
};

class TextureResidency
{
public:
    static uint64_t Payload(uint32_t width, uint32_t height,
        uint32_t levels, uint32_t bytesPerPixel)
    {
        uint64_t bytes = 0;
        for (uint32_t mip = 0; mip < levels && width && height; ++mip)
        {
            bytes += uint64_t(width) * height * bytesPerPixel;
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        return bytes;
    }

    static void Record(const std::shared_ptr<Graphics::IGraphicsTexture>& resource,
        uint64_t residentBytes, uint64_t sourceBytes, uint32_t firstMip,
        const void* provider = nullptr, uint64_t allocationEstimate = 0)
    {
        if (!resource || !resource->GetNativeHandle()) return;
        auto& state = State();
        std::lock_guard lock(state.mutex);
        Prune(state);
        const uint64_t allocation = resource->GetAllocationSizeBytes();
        state.entries.push_back({resource, residentBytes, sourceBytes, firstMip,
            provider, allocation ? allocation : allocationEstimate});
        state.totals.uploadedPayloadBytes += residentBytes;
        ++state.totals.uploads;
    }

    static TextureResidencySnapshot Snapshot(const void* provider = nullptr)
    {
        auto& state = State();
        std::lock_guard lock(state.mutex);
        Prune(state);
        auto result = state.totals;
        for (const auto& entry : state.entries)
        {
            if (provider && entry.provider != provider) continue;
            result.residentPayloadBytes += entry.resident;
            result.sourcePayloadBytes += entry.source;
            result.omittedTopMips += entry.firstMip;
            ++result.residentResources;
            result.allocatedBytes += entry.allocation;
        }
        return result;
    }

private:
    struct Entry
    {
        std::weak_ptr<Graphics::IGraphicsTexture> resource;
        uint64_t resident, source;
        uint32_t firstMip;
        const void* provider;
        uint64_t allocation;
    };
    struct Storage
    {
        std::mutex mutex;
        std::vector<Entry> entries;
        TextureResidencySnapshot totals;
    };
    static Storage& State() { static Storage state; return state; }
    static void Prune(Storage& state)
    {
        const auto end = std::remove_if(state.entries.begin(), state.entries.end(),
            [](const Entry& entry) { return entry.resource.expired(); });
        state.totals.releasedResources += std::distance(end, state.entries.end());
        state.entries.erase(end, state.entries.end());
    }
};
}
