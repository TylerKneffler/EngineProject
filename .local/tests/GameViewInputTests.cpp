#include "InspectorTestUi.h"
#include "Engine/Editor/Core/View/Views/GameView.h"
#include <iostream>
#include <stdexcept>

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    try
    {
        Engine::Scene::Scene scene;
        Require(scene.IsGameplayInputEnabled() && !scene.IsGameplayInputManaged(),
            "Standalone scenes must retain native input");
        scene.SetGameplayInputEnabled(false);
        Engine::Editor::GameView view;
        view.SetScene(&scene);
        view.SetOpen(true);
        Engine::Editor::InspectorTestUi ui;
        ui.viewportVisible = ui.windowFocused = true;
        ui.viewportInput.available = {640.f, 360.f};
        ui.viewportInput.screenMin = {100.f, 200.f};
        ui.viewportInput.screenMax = {740.f, 560.f};
        ui.viewportInput.hovered = true;
        view.DrawPanel(ui);
        Require(!scene.IsGameplayInputEnabled(), "Hover/focus must not capture gameplay");
        ui.viewportInput.leftClicked = true;
        view.DrawPanel(ui);
        Require(scene.IsGameplayInputEnabled(), "Click inside Game must acquire input");
        Require(scene.GetGameplayInputAnchor() == glm::vec2(420.f, 380.f),
            "Mouse look must anchor inside the Game viewport");
        // Escape latches release even while the Game panel is still focused.
        scene.SetGameplayInputEnabled(false);
        ui.viewportInput.leftClicked = false;
        view.DrawPanel(ui);
        Require(!scene.IsGameplayInputEnabled(), "Released input must stay released");
        ui.viewportInput.hovered = false;
        ui.viewportInput.leftClicked = true;
        view.DrawPanel(ui);
        Require(!scene.IsGameplayInputEnabled(), "Click outside Game must not recapture");
        ui.viewportInput.hovered = true;
        view.DrawPanel(ui);
        Require(scene.IsGameplayInputEnabled(), "Click Game again must resume gameplay");
        ui.windowFocused = false;
        ui.viewportInput.leftClicked = false;
        view.DrawPanel(ui);
        Require(!scene.IsGameplayInputEnabled(), "Switching panels must release gameplay");
        ui.windowFocused = true;
        ui.viewportInput.leftClicked = true;
        view.DrawPanel(ui);
        ui.viewportVisible = false;
        view.DrawPanel(ui);
        Require(!scene.IsGameplayInputEnabled(), "Hiding Game must release gameplay");
        std::cout << "Game viewport input regression passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
