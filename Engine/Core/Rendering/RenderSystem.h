#pragma once

#include "Core/Scene/Scene.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace Engine::Rendering
{
class SceneRenderer;

// ---------------------------------------------------------------------------
// RenderSystem
//
// Engine-level owner of scene renderers. Each Scene that is drawn gets one
// SceneRenderer, created on first use from the scene's graphics provider and
// destroyed when the scene is destroyed (the renderer observes its scene).
// Scenes that are never drawn (headless server, importers) never get one.
//
// Every view of a scene (editor Scene/Game views, previews, the game window,
// exporters) shares that scene's renderer, so prepared frame data is reused
// across views exactly as before.
// ---------------------------------------------------------------------------
class RenderSystem
{
public:
    using Scene = Engine::Scene::Scene;

    // The scene's renderer, created on first use. Call on the scene owner
    // thread. A renderer whose scene was re-initialised with another graphics
    // provider is rebuilt.
    static SceneRenderer& For(Scene& scene);
    // The scene's renderer if it exists; never creates one. Safe from any
    // thread while the scene is alive.
    static SceneRenderer* Find(const Scene& scene);
    // Destroys the scene's renderer and its GPU resources. The next For()
    // builds a new one.
    static void Release(const Scene& scene);

    // Resources the scene retired (replaced component buffers, removed
    // render-only objects) plus the renderer's retired pipelines and shadow
    // textures. The caller keeps them until the GPU frame fence.
    static Scene::RetiredGraphicsResources TakeRetiredGraphicsResources(Scene& scene);

private:
    static RenderSystem& Instance();

    std::mutex m_mutex;
    std::unordered_map<const Scene*, std::unique_ptr<SceneRenderer>> m_renderers;
};
}
