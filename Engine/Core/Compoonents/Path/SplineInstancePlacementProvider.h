#pragma once

#include "Core/component.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <string>

namespace Engine::Components { class MeshInstanceLayer; }

class SplineInstancePlacementProvider final : public Engine::Core::Component
{
public:
    SplineInstancePlacementProvider();

    std::string pathObjectName;
    float spacing = 2.f;
    float uniformScale = 1.f;
    glm::vec3 offset{0.f};
    glm::vec3 up{0.f, 1.f, 0.f};

    void PreparePlacements();
    void Enabled() override { m_signature = 0; }
    void Disabled() override;
    void OnDestroy() override;
    bool DrawProperties(Engine::Editor::IEditorUi& ui) override;
    bool RequiresUpdate() const override { return false; }
    const std::string& GetBuildMessage() const { return m_message; }

private:
    uint64_t BuildSignature(const class SplinePath* path,
        const Engine::Components::MeshInstanceLayer* layer) const;
    uint64_t m_signature = 0;
    std::string m_message;
};
