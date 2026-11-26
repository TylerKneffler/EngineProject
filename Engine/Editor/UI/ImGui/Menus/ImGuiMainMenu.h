#pragma once
#include <deque>
#include <memory>
#include <string>

#include "Engine/Editor/Tools/AssetGeneratorRunner.h"
#include "Engine/Editor/Tools/TerrainGeneratorWindow.h"

namespace Engine::Editor
{
class EditorState;
class GameBuildManager;
enum class PlayState;

// Owns ImGui's editor commands, view menu, and play-state controls.
class ImGuiMainMenu
{
public:
    void Draw(EditorState& state, PlayState playState,
        GameBuildManager* buildManager) const;

private:
    void DrawFileMenu(EditorState& state, PlayState playState,
        GameBuildManager* buildManager) const;
    void DrawViewsMenu(EditorState& state) const;
    void DrawToolbarMenu(EditorState& state) const;
    void DrawRenderingMenu(EditorState& state, PlayState playState) const;
    void OpenPanel(EditorState& state, const char* type) const;
    // Tools > Asset generators (tools/generate_*.py, tools/cook_*.py).
    void DrawToolsMenu(EditorState& state) const;
    void PollAssetGenerators(EditorState& state) const;
    mutable std::unique_ptr<AssetGeneratorRunner> m_generators;
    // Tools > Terrain generator (map-scale base heights).
    mutable std::unique_ptr<TerrainGeneratorWindow> m_terrainGenerator;
    mutable std::deque<AssetGeneratorRunner::Generator> m_generatorQueue;
    mutable bool m_openNewAnimationPopup = false;
    mutable char m_newAnimationName[128]{};
    mutable std::string m_newAnimationError;
};
}
