#pragma once

#include "Core/Scene/Scene.h"

#include <cstddef>
#include <vector>

namespace Engine::Scene::Renderer
{
struct TerrainFootprint
{
    float minX = 0.f, maxX = 0.f, minZ = 0.f, maxZ = 0.f, minY = 0.f, maxY = 0.f;
    float nearDistance = 0.f, farDistance = 0.f;
    float angleStart = 0.f, angleEnd = 0.f;
};

class VisibilityResolver
{
public:
    struct ViewState
    {
        float nearPlane = 0.1f;
        float farPlane = 1000.f;
        bool useOcclusion = true;
    };

    explicit VisibilityResolver(ViewState state = {}) : m_state(state) {}

    bool IsVisible(const void* object) const
    {
        return object != nullptr;
    }

    void UpdateView(const ViewState& state)
    {
        m_state = state;
    }

private:
    ViewState m_state;
};

class SceneRendererVisibility
{
public:
    using ViewState = VisibilityResolver::ViewState;

    explicit SceneRendererVisibility(ViewState state = {}) : m_resolver(state) {}

    void Prepare(const ViewState& state)
    {
        m_resolver.UpdateView(state);
    }

    bool IsVisible(const void* object) const
    {
        return m_resolver.IsVisible(object);
    }

    static std::optional<TerrainFootprint> BuildTerrainFootprint(
        const Scene::FrameRenderItem& item,
        const glm::vec3& cameraPosition);

    static bool HiddenByTerrain(
        const Scene::FrameRenderItem& item,
        const glm::vec3& cameraPosition,
        const std::vector<TerrainFootprint>& terrainBlockers);

    static bool HiddenBySolid(
        const Scene::FrameRenderItem& item,
        const glm::vec3& cameraPosition,
        const std::vector<TerrainFootprint>& solidBlockers);

    static void PrepareTerrainOcclusion(
        Scene& scene,
        const std::vector<Scene::FrameRenderItem>& frameRenderItems,
        const glm::vec3& cameraPosition,
        bool shadowOnly,
        bool includeEditorVisuals,
        bool hasPortalViews,
        bool editorMode2D,
        bool terrainWarpActive,
        std::vector<TerrainFootprint>& terrainBlockers,
        std::vector<TerrainFootprint>& solidBlockers);

private:
    VisibilityResolver m_resolver;
};
}
