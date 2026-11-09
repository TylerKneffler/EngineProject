#pragma once

#include "Core/Scene/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"
#include "Core/Rendering/MeshLodSelection.h"

#include <map>
#include <vector>

namespace Engine::Scene::Renderer
{
class SceneRendererLodSelection
{
public:
    void Prepare(
        Scene& scene,
        const std::vector<PreparedDraw>& draws,
        const Engine::Components::Camera* camera,
        const glm::vec3& cameraPosition,
        const glm::mat4& viewProjection,
        uint32_t viewportWidth,
        uint32_t viewportHeight,
        uint64_t instanceViewOrdinal,
        bool shadowOnly,
        bool hasPortalViews,
        bool includeEditorVisuals);

    bool IsSelected(const PreparedDraw& draw, bool shadow = false) const;
    int ChooseScreen(
        const Engine::Components::Mesh& policy,
        glm::vec3 minimum,
        glm::vec3 maximum,
        const glm::mat4& world,
        uint64_t identity,
        uint64_t placement,
        uint64_t domain,
        int maximumLod);
    std::map<uint64_t, std::vector<int>>& PlacementLevels();

private:
    Scene* m_scene = nullptr;
    const std::vector<PreparedDraw>* m_draws = nullptr;
    std::vector<uint8_t> m_selectedLods;
    std::vector<uint8_t> m_distanceSelectedLods;
    std::map<uint64_t, std::vector<int>> m_screenPlacementLevels;
    Engine::Rendering::MeshLodView m_screenView{};
    uint64_t m_screenSerial = 0;
    uint64_t m_instanceViewOrdinal = 0;
    const Engine::Components::Camera* m_camera = nullptr;
};
}
