#pragma once

class Object;

class Component
{
private:
public:
    Component() = default;
    virtual ~Component() = default;

    Object* Owner = nullptr; // The Object this component is attached to

    bool singlecomponent = false; // If true, only one component of this type can be added to an Object
};