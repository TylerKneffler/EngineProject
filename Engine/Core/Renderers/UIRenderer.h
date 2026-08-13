#pragma once
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"

namespace Engine::Scene { class Scene; }

namespace Engine::Renderers
{
class UIRenderer
{
public:
    UIRenderer();
    ~UIRenderer();

    void Initialize(Engine::Graphics::IGraphicsProvider* graphicsProvider);
    void Render(Engine::Scene::Scene& scene, Engine::Graphics::IGraphicsContext* context, float viewportAspect);
    bool IsReady() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};
}
