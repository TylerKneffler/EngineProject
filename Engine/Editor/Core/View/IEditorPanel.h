#pragma once
#include <string>
#include <functional>
#include "View/Focus/WindowFocusHandler.h"
namespace Engine::Editor
{
class IEditorUi;

// ---------------------------------------------------------------------------
// IEditorPanel — common polymorphic interface for every editor panel.
//
// Both graphics-backed 3-D views and package-neutral utility panels
// panels (HierarchyView, PropertiesView, etc.) derive from this interface so
// the editor can manage them uniformly through a single vector.
//
// Lifecycle:
//   - DrawPanel(ui)  : defines the panel once against the active UI facade.
//   - NeedsRender()  : returns true for panels that own an offscreen DX12
//                      render target (SceneView, GameView).
//   - Render3D(cmd)  : for NeedsRender panels, issues the scene draw calls
//                      into cmd before DrawPanel() reads the texture.
//
// Focus & Cursor Handling:
//   - SetCursorBehaviorOnFocus() : Configure how cursor behaves when panel is focused
//   - GetCursorBehaviorOnFocus() : Query current cursor behavior configuration
//   - CaptureCursorOnFocus() : Convenience method to make panel capture cursor (for game windows)
// ---------------------------------------------------------------------------
class IEditorPanel
{
public:
    virtual ~IEditorPanel() = default;

    // Draws through the selected package-neutral UI implementation.
    virtual void DrawPanel(IEditorUi& ui) = 0;

    // Returns true if this panel owns an offscreen DX12 render target.
    virtual bool NeedsRender() const { return false; }

    // Subclasses override to issue scene draw calls into cmd each frame.
    // cmd: opaque graphics command list handle (cast internally to ID3D12GraphicsCommandList*)
    virtual void Render3D(void* /*cmd*/) {}

    // Allows document-aware commands such as Ctrl+S to follow panel focus.
    std::function<void()> OnFocused;

    // ---- Title / open state ----
    const std::string& GetTitle() const { return m_title; }
    void SetTitle(const std::string& t)  { m_title = t;   }

    bool IsOpen() const       { return m_open; }
    void SetOpen(bool open)   { m_open = open; }

    // ---- Cursor & Focus Handling ----
    /// Set cursor behavior when this panel receives focus
    void SetCursorBehaviorOnFocus(CursorBehaviorOnFocus behavior)
    {
        m_cursorBehaviorOnFocus = behavior;
    }

    /// Get cursor behavior configuration for this panel
    CursorBehaviorOnFocus GetCursorBehaviorOnFocus() const
    {
        return m_cursorBehaviorOnFocus;
    }

    /// Query whether this panel should hide/capture the cursor when focused
    bool ShouldCaptureCursorOnFocus() const
    {
        return m_cursorBehaviorOnFocus != CursorBehaviorOnFocus::Visible &&
               m_cursorBehaviorOnFocus != CursorBehaviorOnFocus::Confined;
    }

protected:
    std::string m_title;
    bool        m_open = true;
    CursorBehaviorOnFocus m_cursorBehaviorOnFocus = CursorBehaviorOnFocus::Visible;
};
}
