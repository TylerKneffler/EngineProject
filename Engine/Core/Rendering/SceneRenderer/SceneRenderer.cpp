#include "Core/Rendering/SceneRenderer.h"

#include "Core/Rendering/RenderSystem.h"
#include "Core/Rendering/SceneRenderer/SceneNativeRenderPacket.h"
#include "Core/Renderers/UIRenderer.h"

namespace Engine::Rendering
{
SceneRenderer::SceneRenderer(Scene& scene, IGraphicsProvider* graphicsProvider)
    : m_scene(scene), m_graphicsProvider(graphicsProvider)
{
    m_scene.AddObserver(this);
    if (m_graphicsProvider)
        Initialize();
}

SceneRenderer::~SceneRenderer()
{
    // The camera occlusion job reads frame items owned by this renderer.
    JoinEarlyCameraOcclusion();
    m_scene.RemoveObserver(this);
}

void SceneRenderer::OnObjectsClearing()
{
    m_screenLodHistory.clear();
    m_screenLodCounts = {};
}

void SceneRenderer::OnStructureChanged(Object* owner)
{
    if (owner)
        m_frameLookupDirty.insert(owner);
    else
        m_frameLookupFull = true;
}

void SceneRenderer::OnBackgroundWorkFence()
{
    JoinEarlyCameraOcclusion();
}

void SceneRenderer::OnSceneDestroying()
{
    // Destroys this renderer; nothing may touch members afterwards.
    RenderSystem::Release(m_scene);
}
}
