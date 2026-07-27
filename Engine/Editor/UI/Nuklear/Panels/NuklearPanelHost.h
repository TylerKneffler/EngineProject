#pragma once

#include "Engine/Editor/UI/Nuklear/Layout/NuklearDockLayout.h"

struct nk_context;
class EditorState;
class IEditorPanel;
class NuklearEditorUi;

// Owns Nuklear tab selection and the placement/lifecycle of editor panels.
class NuklearPanelHost
{
public:
    void OpenPanel(EditorState& state, const char* type);
    void DrawDockedPanels(nk_context& context, NuklearEditorUi& ui,
        EditorState& state, const NuklearDockGeometry& geometry);
    void DrawPreferences(NuklearEditorUi& ui, EditorState& state,
        float width, float height) const;
    void CleanupClosedPanels(EditorState& state);

private:
    IEditorPanel* m_activeLeftTab = nullptr;
    IEditorPanel* m_activeCenterTab = nullptr;
};
