#pragma once

class Scene;

class PhysicsSystem
{
public:
    static void Step(Scene& scene, float deltaTime);
    static void Reset(Scene& scene);
};
