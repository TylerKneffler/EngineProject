#include "Scripts/Rotate.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <glm/gtc/matrix_transform.hpp>

Rotate::Rotate()
{
    SetTypeName(COMPONENT_TYPE_NAME(Rotate));
    RegisterField("axis", axis);
    RegisterField("speed", speed);
}

namespace
{
struct RotateRegistration
{
    RotateRegistration()
    {
        RegisterComponentType<Rotate>();
    }
};

RotateRegistration g_rotateRegistration;
}

void Rotate::Start()
{
    // Nothing to initialise — axis and speed are set directly.
}

void Rotate::Update()
{
    if (!Owner) return;
    constexpr float kFixedDt = 1.f / 60.f;
    Owner->transform.rotation += axis * glm::radians(speed) * kFixedDt;
}

void Rotate::DrawProperties(IEditorUi& ui)
{
    // Custom interactive editor for Rotate script properties
    ui.DragFloat3("Rotation Axis", &axis.x, 0.01f, -1.f, 1.f);
    ui.DragFloat("Speed (deg/s)", &speed, 0.5f, -360.f, 360.f);
}
