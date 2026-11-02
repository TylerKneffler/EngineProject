#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <future>
#include <atomic>
#include <vector>
#include <chrono>
#include <unordered_map>

namespace Engine::Components { class Texture; }
namespace Engine::Scene { class Scene; }
namespace Engine::Graphics
{
class IGraphicsProvider;
class IGraphicsTexture;
}

namespace Engine::Editor
{
// Lazily creates small, GPU-backed asset thumbnails. Material assets use a
// generated PBR sphere and HDR/EXR assets use a tone-mapped mirror ball.
class AssetPreviewCache
{
public:
    AssetPreviewCache()=default;
    AssetPreviewCache(const AssetPreviewCache&)=delete;
    AssetPreviewCache& operator=(const AssetPreviewCache&)=delete;
    void* Get(const std::string& path,
        Engine::Graphics::IGraphicsProvider* graphicsProvider);
    // Call this cache on the editor/graphics owner; workers receive CPU inputs only.
    // Poll before drawing to retire idle previews and request redraws without waiting.
    // GPU uploads happen only in Get during drawing.
    bool Poll();
    void Invalidate(const std::string& path);
    void Clear();
    // Captures the current scene through its active game camera and persists a
    // compact derived thumbnail beside the scene file.
    static bool CaptureScene(const std::string& path,
        Engine::Scene::Scene& scene,
        Engine::Graphics::IGraphicsProvider* graphicsProvider);
    static void MovePersistentPreview(const std::string& oldPath,
        const std::string& newPath);
    static void RemovePersistentPreview(const std::string& path);
    static bool Supports(const std::string& path);
    static bool IsCircularPreview(const std::string& path);

private:
    using Clock=std::chrono::steady_clock;
    struct Prepared
    {
        std::filesystem::file_time_type writeTime{};
        std::vector<uint8_t> pixels;
        bool changed=false,stable=true;
    };
    struct Entry
    {
        std::filesystem::file_time_type writeTime{};
        Engine::Graphics::IGraphicsProvider* provider = nullptr;
        std::future<Prepared> pending;
        std::shared_ptr<std::atomic<bool>> cancelled=std::make_shared<std::atomic<bool>>(false);
        ~Entry() { cancelled->store(true); }
        Clock::time_point checkAfter{},lastUse{};
        bool initialized=false,notified=false,invalidated=false;
        std::shared_ptr<Engine::Graphics::IGraphicsTexture> generated;
    };
    // Graphics-backed entries are retained/released only by preview owners, never
    // by the generic CacheStore's worker-side pruning.
    std::unordered_map<std::string,std::shared_ptr<Entry>> m_requests;
    inline static std::unordered_map<std::string,std::weak_ptr<Entry>> s_entries;
    inline static unsigned s_creationsUntilPrune=128;
    static void InvalidateEntries(const std::string& path);
    bool m_waitingForSlot=false;
};
}
