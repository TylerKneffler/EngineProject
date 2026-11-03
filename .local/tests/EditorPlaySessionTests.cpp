#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/Startup/EditorStartupSettings.h"
#include "Engine/Editor/Core/View/ViewFactory.h"
#include "Core/SceneManager.h"
#include <imgui.h>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace
{
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char** argv)
{
    std::cout.setf(std::ios::unitbuf);
    ImGui::CreateContext();
    try
    {
        auto settings = Engine::Editor::CreateEngineDevelopmentSettings();
        Engine::Editor::EditorState editor(GetModuleHandleW(nullptr), settings, {});
        struct ActiveSceneGuard
        {
            ~ActiveSceneGuard() { Engine::Core::SceneManager::SetActiveScene(nullptr); }
        } activeSceneGuard;
        Require(editor.Init(), "editor initialization failed");
        editor.InitializeUiState();
        auto* original = editor.GetScene();
        Engine::Core::SceneManager::SetActiveScene(original);
        const bool grass = argc > 1 && std::string(argv[1]) == "--grass";
        if (grass)
        {
            std::cout << "Loading full grass scene; waiting for completion.\n";
            editor.LoadStartupScene("Engine/Core/Assets/Scenes/terrain/grass_flat_test.scene");
            Require(original->GetObjects().size() >= 885, "full grass scene did not load");
        }
        auto* authored = original->AddObject("Retained authored object");
        authored->transform.position = {1.f, 2.f, 3.f};
        original->CommitPendingObjectChanges();
        Engine::Editor::HierarchyView* hierarchy = nullptr;
        Engine::Editor::PropertiesView* properties = nullptr;
        for (auto& panel : editor.GetPanels())
        {
            if (auto* view = dynamic_cast<Engine::Editor::HierarchyView*>(panel.get()))
                hierarchy = view;
            if (auto* view = dynamic_cast<Engine::Editor::PropertiesView*>(panel.get()))
                properties = view;
        }
        Require(hierarchy && properties, "missing editor panels");
        hierarchy->SetSelectedObject(authored);
        editor.TrackSceneChanges(true, false);
        authored->name = "Retained authored edit";
        hierarchy->OnHierarchyChanged();
        editor.TrackSceneChanges(true, false);
        Require(editor.CanUndo(), "fixture did not create undo history");
        const auto baseline = original->SaveSnapshotToString();
        editor.SetHasUnsavedChanges(true);
        const bool dirty = editor.HasUnsavedChanges();
        const int cycles = grass ? 1 : 3;
        for (int cycle = 0; cycle < cycles; ++cycle)
        {
            std::cout << "Creating runtime copy for cycle " << cycle + 1 << ".\n";
            const auto start = std::chrono::steady_clock::now();
            Require(editor.CapturePlayModeScene(), "runtime copy failed");
            auto* runtime = editor.GetScene();
            Require(runtime != original, "Play used authored scene");
            Require(Engine::Core::SceneManager::GetActiveScene() == runtime,
                "scene manager kept edit scene active");
            Require(hierarchy->GetScene() == runtime && properties->GetScene() == runtime,
                "editor panels did not follow runtime scene");
            Require(editor.CapturePlayModeScene() && editor.GetScene() == runtime,
                "duplicate capture replaced active session");
            auto newView = editor.GetViewFactory()->Create("Game");
            Require(dynamic_cast<Engine::Editor::GameView*>(newView.get())->GetScene() == runtime,
                "new panels use dormant edit scene");
            // New panels must participate in Stop rebinding too.
            editor.GetPanels().push_back(std::move(newView));
            runtime->Start();
            runtime->Update(1.f / 60.f);
            // Exercise arbitrary runtime deletion and complete scene replacement.
            if (!grass) runtime->ClearObjects();
            runtime->AddObject("Runtime replacement");
            runtime->CommitPendingObjectChanges();
            Require(authored->OwnerScene == original &&
                authored->transform.position == glm::dvec3(1.0, 2.0, 3.0),
                "runtime mutated retained objects");
            const auto stop = std::chrono::steady_clock::now();
            editor.RestorePlayModeScene();
            const double stopMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - stop).count();
            Require(editor.GetScene() == original, "Stop recreated edit scene");
            Require(Engine::Core::SceneManager::GetActiveScene() == original,
                "scene manager kept runtime scene active");
            Require(original->SaveSnapshotToString() == baseline,
                "Stop changed authored state");
            Require(hierarchy->GetSelectedObject() == authored &&
                properties->GetSelectedObject() == authored,
                "Stop did not restore original selection pointers");
            Require(editor.HasUnsavedChanges() == dirty, "Stop changed dirty state");
            Require(editor.CanUndo() && !editor.CanRedo(), "Play changed undo/redo history");
            for (const auto& panel : editor.GetPanels())
            {
                if (auto* view = dynamic_cast<Engine::Editor::GameView*>(panel.get()))
                    Require(view->GetScene() == original, "Game view has stale runtime pointer");
                if (auto* view = dynamic_cast<Engine::Editor::SceneView*>(panel.get()))
                    Require(view->GetScene() == original, "Scene view has stale runtime pointer");
            }
            editor.RestorePlayModeScene();
            std::cout << "Cycle " << cycle + 1 << ": Stop " << stopMs
                << " ms, total " << std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count() << " ms.\n";
            for (const auto& entry : editor.GetConsole()->GetEntries())
                if (entry.message.find("[Play]") == 0)
                    std::cout << entry.message << '\n';
            editor.GetConsole()->Clear();
        }
        original->FenceBackgroundWork();
        if (!grass)
        {
            editor.Undo();
            Require(editor.CanRedo(), "pre-play undo no longer works");
            editor.Redo();
            Require(original->SaveSnapshotToString() == baseline,
                "pre-play redo no longer restores authored edits");
        }
        Engine::Core::SceneManager::SetActiveScene(nullptr);
        editor.GetRenderer()->WaitIdle();
        editor.ReleaseUiPanels();
        Require(editor.GetPanels().empty() && !editor.GetConsole() && !editor.GetViewFactory(),
            "UI release left live panel/factory bindings");
        std::cout << "Retained edit scene Play/Stop checks passed.\n";
    }
    catch (const std::exception& error)
    {
        Engine::Core::SceneManager::SetActiveScene(nullptr);
        std::cerr << error.what() << '\n';
        return 1;
    }
    ImGui::DestroyContext();
    return 0;
}
