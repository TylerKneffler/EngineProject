#pragma once

#include "Core/Script.h"
#include <string>

namespace Engine::Components { class AnimationManager; }

class MmdDanceReplay final : public Engine::Core::Script
{
public:
    MmdDanceReplay();
    std::string characterObjectName = "Mococo Dance";
    void Start() override;
    void Update() override;

private:
    Engine::Components::AnimationManager* FindAnimation() const;
    static void Restart(Engine::Components::AnimationManager& animation);
};
