#pragma once
#include "pch.h"
#include "Core/Compoonents/Transform.h"

class Object
{
private:
public:
    Object();
    Object(Transform transform);

    ~Object();

    Transform transform;

    Object *Parent = nullptr;
    std::vector<Object*> Children;
};