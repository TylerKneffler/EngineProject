#include "Camera.h"
#include "Core/Object/Object.h"
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
