#pragma once

#include "Core/Components/Script.h"
#include "Core/Components/PropertyMacros.h"
#include <string>

// Owns main-menu behavior independently from the retained UI hierarchy.
// At runtime it locates the configured Play button and connects its listener.
class MainMenuGameManager final : public Engine::Core::Script
{
public:
    MainMenuGameManager();

    PROPERTY(Inspector, EditAnywhere, Category = "Main Menu")
    std::string playButtonObjectName = "Play Label";

    void Start() override;
};
