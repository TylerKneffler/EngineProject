#include "Camera.h"
#include "Core/Object.h"
#include <cassert>

using namespace DirectX;

XMMATRIX Camera::GetViewMatrix() const
{
    assert(Owner && "Camera requires an owner Object with a Transform");
    const glm::vec3& p = Owner->transform.position;
    XMFLOAT3 eye{ p.x, p.y, p.z };
    return XMMatrixLookAtLH(
        XMLoadFloat3(&eye),
        XMLoadFloat3(&target),
        XMLoadFloat3(&up));
}

XMMATRIX Camera::GetProjectionMatrix(float aspect) const
{
    return XMMatrixPerspectiveFovLH(
        XMConvertToRadians(fov), aspect, nearPlane, farPlane);
}

// ---- Serialization ----------------------------------------------------------
namespace
{
    JsonValue JXm3(const DirectX::XMFLOAT3& v)
    {
        return JsonValue::MakeArray().Push(JsonValue(v.x)).Push(JsonValue(v.y)).Push(JsonValue(v.z));
    }
    DirectX::XMFLOAT3 fromXm3(const JsonValue& v, DirectX::XMFLOAT3 def = {})
    {
        if (!v.IsArray() || v.ArraySize() < 3) return def;
        return { v.ArrayAt(0).AsFloat(), v.ArrayAt(1).AsFloat(), v.ArrayAt(2).AsFloat() };
    }
}

JsonValue Camera::Serialize() const
{
    JsonValue node = JsonValue::MakeObject();
    node.Set("type",  JsonValue(std::string("Camera")));
    node.Set("fov",   JsonValue(fov));
    node.Set("near",  JsonValue(nearPlane));
    node.Set("far",   JsonValue(farPlane));
    node.Set("target",JXm3(target));
    node.Set("up",    JXm3(up));
    return node;
}

void Camera::Deserialize(const JsonValue& v)
{
    if (v.Has("fov"))    fov       = v["fov"].AsFloat();
    if (v.Has("near"))   nearPlane = v["near"].AsFloat();
    if (v.Has("far"))    farPlane  = v["far"].AsFloat();
    if (v.Has("target")) target    = fromXm3(v["target"], target);
    if (v.Has("up"))     up        = fromXm3(v["up"],     up);
}
