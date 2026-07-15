#include "Scripts/Rotate.h"
#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include <glm/gtc/matrix_transform.hpp>

namespace
{
struct RotateRegistration
{
    RotateRegistration()
    {
        SceneSerializer::Register("Rotate", []() -> Component* { return new Rotate(); });
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

// ---- Serialization ----------------------------------------------------------
namespace
{
    JsonValue J3(const glm::vec3& v)
    {
        return JsonValue::MakeArray().Push(JsonValue(v.x)).Push(JsonValue(v.y)).Push(JsonValue(v.z));
    }
    glm::vec3 from3(const JsonValue& v, glm::vec3 def = {})
    {
        if (!v.IsArray() || v.ArraySize() < 3) return def;
        return { v.ArrayAt(0).AsFloat(), v.ArrayAt(1).AsFloat(), v.ArrayAt(2).AsFloat() };
    }
}

JsonValue Rotate::Serialize() const
{
    JsonValue node = JsonValue::MakeObject();
    node.Set("type",  JsonValue(std::string("Rotate")));
    node.Set("axis",  J3(axis));
    node.Set("speed", JsonValue(speed));
    return node;
}

void Rotate::Deserialize(const JsonValue& v)
{
    axis  = from3(v["axis"], axis);
    if (v.Has("speed")) speed = v["speed"].AsFloat();
}
