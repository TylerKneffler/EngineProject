#pragma once
#include "Core/Rendering/SceneRenderer.h"

#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

#include <functional>

namespace Engine::Graphics { class IGraphicsContext; }

namespace Engine::Rendering
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
    using Scene = Engine::Scene::Scene;
    SceneRendererGeometrySubmission(
        SceneRenderer& renderer,
        Engine::Graphics::IGraphicsContext* context);

    Engine::Graphics::IPipelineState* GeometryPipeline(
        const PreparedDraw& draw,
        Engine::Graphics::IPipelineState* base) const;
    void SubmitGeometry(const PreparedDraw& draw, bool shadowGeometry = false,
        uint64_t selectedPlacementId = 0);
    void SubmitMaterialGeometry(
        const PreparedDraw& draw,
        Engine::Graphics::IPipelineState* base,
        const std::function<void()>& restoreState = {});

private:
    SceneRenderer& m_renderer;
    Engine::Graphics::IGraphicsContext* m_context = nullptr;
};
}
