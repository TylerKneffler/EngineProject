#pragma once
#include "Core/component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>

namespace Engine::Components
{
// Realtime or baked light. Point lights attenuate from their object position;
// global lights illuminate the whole scene from their Transform rotation.
class Light : public Engine::Core::Component
{
public:
    enum class Type : int
    {
        Point = 0,
        Ambient = 1
    };

    Light();
    ~Light() = default;

    PROPERTY(Inspector, EditAnywhere, Category = "Light")
    glm::vec3 color { 1.f, 0.95f, 0.85f };

    // Stored as an integer so existing generic component serialization can
    // preserve it. Scenes without this field remain point lights.
    PROPERTY(Inspector, EditAnywhere, Category = "Light")
    int lightType = static_cast<int>(Type::Point);

    PROPERTY(Inspector, EditAnywhere, Category = "Light", ClampMin = "0.0")
    float intensity = 4.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Light", ClampMin = "0.01")
    float range = 8.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Light", Range = "0.1, 8.0")
    float falloff = 2.f;

    // false = Realtime, true = Baked.
    PROPERTY(Inspector, EditAnywhere, Category = "Light")
    bool baked = false;

    Type GetLightType() const
    {
        return lightType == static_cast<int>(Type::Ambient)
            ? Type::Ambient : Type::Point;
    }

    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
};
}
