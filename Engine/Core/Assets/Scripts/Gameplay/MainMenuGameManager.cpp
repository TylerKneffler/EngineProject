#include "Scripts/Gameplay/MainMenuGameManager.h"
#include "Core/Runtime/Game.h"
#include "Core/Scene/Scene.h"

#include "Core/Components/UI/UIButton.h"
#include "Core/Scene/Object.h"
#include "Core/Serialization/SceneSerializer.h"

MainMenuGameManager::MainMenuGameManager()
{
    SetTypeName(COMPONENT_TYPE_NAME(MainMenuGameManager));
    RegisterField("playButtonObjectName", playButtonObjectName);
}

namespace
{
struct MainMenuGameManagerRegistration
{
    MainMenuGameManagerRegistration()
    {
        Engine::Serialization::RegisterComponentType<MainMenuGameManager>(
            "MainMenuGameManager");
    }
};

MainMenuGameManagerRegistration g_registration;
}

void MainMenuGameManager::Start()
{
    Engine::Core::Object* playObject =
        FindObjectInSceneByName(playButtonObjectName);
    Engine::Components::UIButton* playButton = playObject
        ? playObject->GetComponent<Engine::Components::UIButton>() : nullptr;
    if (!playButton)
        return;

    // The callback captures no scene object. The requested transition is
    // performed at the safe post-update transition point on the next frame.
    playButton->AddOnClickListener([](Engine::Components::UIButton& button)
    {
        auto* scene = button.Owner ? button.Owner->GetScene() : nullptr;
        if (auto* game = scene ? scene->GetGame() : nullptr)
            game->RequestDefaultSceneLoad();
    });
}
