#pragma once

#include "Core/Component.h"
#include "Core/Model/ImportedModel.h"
#include <unordered_map>

namespace Engine::Components
{
class PmxMaterialMorph final : public Engine::Core::Component
{
public:
    PmxMaterialMorph();
    std::vector<Engine::Model::ImportedMaterialMorph> morphs;
    void Apply();
    void SetAnimatedMorphWeights(const std::unordered_map<std::string, float>& weights);
    void Start() override;
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;

private:
    struct State
    {
        glm::vec3 diffuse { 1.f };
        float alpha = 1.f;
        glm::vec3 specular { 1.f };
        float shininess = 32.f;
        glm::vec3 ambient { .1f };
        std::string alphaMode;
    };
    State m_input;
    State m_output;
    bool m_applied = false;
    std::unordered_map<std::string, float> m_animatedMorphWeights;
};
}
