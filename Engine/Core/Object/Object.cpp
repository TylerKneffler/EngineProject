#include "Object.h"

Object::Object()
{
    this->transform = Transform();
}

Object::Object(Transform transform)
{
    this->transform = transform;
}

Object::~Object()
{
}