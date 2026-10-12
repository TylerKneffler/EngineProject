#pragma once

#include <filesystem>
#include <memory>
#include <string>

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
    void* Get(const std::string& path,
        Engine::Graphics::IGraphicsProvider* graphicsProvider);
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
    struct Entry
    {
        std::filesystem::file_time_type writeTime{};
        Engine::Graphics::IGraphicsProvider* provider = nullptr;
        std::shared_ptr<Engine::Components::Texture> source;
        std::shared_ptr<Engine::Graphics::IGraphicsTexture> generated;
    };
};
}
