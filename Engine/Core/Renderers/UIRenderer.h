#pragma once

class IGraphicsContext;
class IGraphicsProvider;
class Scene;

class UIRenderer
{
public:
    UIRenderer();
    ~UIRenderer();

    void Initialize(IGraphicsProvider* graphicsProvider);
    void Render(Scene& scene, IGraphicsContext* context, float viewportAspect);
    bool IsReady() const;

private:
    struct Impl;
    Impl* m_impl = nullptr;
};
