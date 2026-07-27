#pragma once

// Nuklear menu and toolbar feature.

#include <functional>

struct nk_context;
class EditorState;
class GameBuildManager;
enum class PlayState;

// Draws the Nuklear editor command surface. Panel ownership stays with the
// panel host and is reached through the supplied open-panel command.
class NuklearToolbar
{
public:
    using OpenPanelCommand = std::function<void(const char*)>;

    void Draw(nk_context& context, float width,
        EditorState& state, PlayState playState, GameBuildManager* buildManager,
        const OpenPanelCommand& openPanel) const;
};
