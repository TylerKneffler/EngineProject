// Component GPU-resource lifetime on the scene: deferred retirement of buffers
// and render-only objects, and budgeted PrepareGraphicsResources dispatch.
// Rendering itself lives in Engine::Rendering::SceneRenderer.
#include "Core/Scene/Scene.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Serialization/PrefabLoadProfile.h"

#include <algorithm>
#include <chrono>

namespace Engine::Scene
{
Scene::RetiredGraphicsResources Scene::TakeRetiredGraphicsResources()
{
    RetiredGraphicsResources retired;
    retired.objects.swap(m_retiredRenderObjects);
    retired.buffers.swap(m_retiredRenderBuffers);
    retired.sharedBuffers.swap(m_retiredSharedRenderBuffers);
    return retired;
}

void Scene::RetireGraphicsBuffer(std::unique_ptr<IGraphicsBuffer> buffer)
{
    if (buffer && m_deferGraphicsRetirement)
        m_retiredRenderBuffers.push_back(std::move(buffer));
}

void Scene::RetireGraphicsBuffer(std::shared_ptr<IGraphicsBuffer> buffer)
{
    if (buffer && m_deferGraphicsRetirement)
        m_retiredSharedRenderBuffers.push_back(std::move(buffer));
}

void Scene::RetireRenderObject(std::unique_ptr<Object> detached)
{
    AssertOwnerThread();
    if (!detached) return;
    if (m_deferGraphicsRetirement) m_retiredRenderObjects.push_back(std::move(detached));
    else if (m_beforeGpuResourceDestruction) m_beforeGpuResourceDestruction();
}

void Scene::ProcessGraphicsPreparation(IGraphicsProvider* graphicsProvider)
{
    AssertOwnerThread();
    if (!m_graphicsPreparationPending || !graphicsProvider) return;
    Engine::Serialization::PrefabLoadProfile profile("scene graphics preparation");
    if (m_graphicsPreparationRestart && m_graphicsPreparationItems.empty())
    {
        m_graphicsPreparationItems.clear();
        m_graphicsPreparationCursor = 0;
        for (const auto& object : m_objects)
            for (auto* component : object->Components)
                if (component)
                    m_graphicsPreparationItems.emplace_back(GetObjectId(object.get()), component);
        m_graphicsPreparationRestart = false;
    }
    const auto started = std::chrono::steady_clock::now();
    while (m_graphicsPreparationCursor < m_graphicsPreparationItems.size())
    {
        const auto [id, component] = m_graphicsPreparationItems[m_graphicsPreparationCursor++];
        auto* object = FindObjectById(id);
        if (object && std::find(object->Components.begin(), object->Components.end(), component) != object->Components.end())
        {
            Engine::Serialization::PrefabLoadProfile::Stage stage(
                Engine::Serialization::PrefabLoadProfile::Enabled()
                    ? "graphics_" + component->GetTypeName() : "");
            component->PrepareGraphicsResources(graphicsProvider);
        }
        // Individual driver calls cannot be interrupted. Yield between
        // components, including when one upload exceeds the soft budget.
        if (m_graphicsPreparationBudgetMs > 0 &&
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count() >= m_graphicsPreparationBudgetMs)
            return;
    }
    m_graphicsPreparationItems.clear();
    m_graphicsPreparationCursor = 0;
    // Finish this queue before rescanning requests added during preparation;
    // otherwise continuous streaming can keep restarting at the first object.
    if (m_graphicsPreparationRestart) return;
    m_graphicsPreparationPending = false;
    m_graphicsPreparationBudgetMs = 0;
}
}
