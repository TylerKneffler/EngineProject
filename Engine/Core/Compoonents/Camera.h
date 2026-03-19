#pragma once
#include "component.h"

class Camera : public Component
{
private:
public:
    Camera() = default;
    ~Camera() = default;

    float fov = 60.f; // vertical field of view in degrees
    float aspectRatio = 16.f / 9.f; // aspect ratio of the camera
    float nearPlane = 0.01f; // near clipping plane distance
    float farPlane = 100.f; // far clipping plane distance

};