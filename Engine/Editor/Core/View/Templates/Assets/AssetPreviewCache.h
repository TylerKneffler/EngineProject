#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace Engine::Components { class Texture; }
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
    void* Get(const std::string& path,
        Engine::Graphics::IGraphicsProvider* graphicsProvider);
    void Invalidate(const std::string& path);
    void Clear();
    static bool Supports(const std::string& path);
    static bool IsCircularPreview(const std::string& path);

private:
    struct Entry
    {
        std::filesystem::file_time_type writeTime{};
        Engine::Graphics::IGraphicsProvider* provider = nullptr;
        std::shared_ptr<Engine::Components::Texture> source;
        std::shared_ptr<Engine::Graphics::IGraphicsTexture> generated;
    };
    std::unordered_map<std::string, Entry> m_entries;
};
}
