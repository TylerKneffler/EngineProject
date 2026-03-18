#include "Transform.h"
#include <glm/gtc/matrix_transform.hpp>

glm::mat4 Transform::GetWorldMatrix() const
{
    glm::mat4 t  = glm::translate(glm::mat4(1.f), position);
    glm::mat4 rx = glm::rotate(glm::mat4(1.f), rotation.x, { 1.f, 0.f, 0.f });
    glm::mat4 ry = glm::rotate(glm::mat4(1.f), rotation.y, { 0.f, 1.f, 0.f });
    glm::mat4 rz = glm::rotate(glm::mat4(1.f), rotation.z, { 0.f, 0.f, 1.f });
    glm::mat4 s  = glm::scale(glm::mat4(1.f), scale);
    return t * rz * ry * rx * s;
}