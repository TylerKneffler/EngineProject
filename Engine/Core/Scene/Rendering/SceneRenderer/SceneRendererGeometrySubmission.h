#pragma once

#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

#include <functional>

namespace Engine::Graphics { class IGraphicsContext; }

namespace Engine::Scene::Renderer
{
struct SubmittedGeometry
{
    uint64_t draws = 0;
    uint64_t instances = 0;
    uint64_t triangles = 0;
};

class SceneRendererGeometrySubmission
{
public:
    SceneRendererGeometrySubmission(
        Scene& scene,
        Engine::Graphics::IGraphicsContext* context);

    Engine::Graphics::IPipelineState* GeometryPipeline(
        const PreparedDraw& draw,
        Engine::Graphics::IPipelineState* base) const;
    void SubmitGeometry(const PreparedDraw& draw, bool shadowGeometry = false);
    void SubmitMaterialGeometry(
        const PreparedDraw& draw,
        Engine::Graphics::IPipelineState* base,
        const std::function<void()>& restoreState = {});

private:
    Scene& m_scene;
    Engine::Graphics::IGraphicsContext* m_context = nullptr;
};
}
