#pragma once

// Owns creation of ImGui's default editor dock tree. ImGui itself persists
// later user changes through imgui.ini.
class ImGuiDockspace
{
public:
    void Draw();
};
