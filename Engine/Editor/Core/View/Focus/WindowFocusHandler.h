#pragma once
#include <functional>
#include <vector>
#include <memory>

namespace Engine::Editor
{
    // Forward declaration
    class IEditorPanel;

    /// Enumeration for cursor behavior on focus
    enum class CursorBehaviorOnFocus
    {
        Visible,        ///< Cursor remains visible
        Hidden,         ///< Cursor is hidden
        Captured,       ///< Cursor hidden and locked to center (for game windows)
        Confined,       ///< Cursor confined to window but visible
        ConfiedHidden   ///< Cursor confined to window but hidden
    };

    /// Manages window focus state and cursor behavior for editor views
    /// Handles proper focus transitions between:
    /// - Editor UI panels (Scene, Hierarchy, Properties, etc.)
    /// - Game/Game Preview windows (which capture cursor)
    /// - Fullscreen game windows
    class WindowFocusHandler
    {
    public:
        WindowFocusHandler();
        ~WindowFocusHandler();

        /// Register a view that can receive focus
        /// @param panel The editor panel to register
        /// @param cursorBehavior How cursor should behave when this panel is focused
        void RegisterPanel(IEditorPanel* panel, CursorBehaviorOnFocus cursorBehavior);

        /// Unregister a previously registered view
        /// @param panel The editor panel to unregister
        void UnregisterPanel(IEditorPanel* panel);

        /// Handle when a panel gains focus
        /// @param panel The panel receiving focus
        void OnPanelFocused(IEditorPanel* panel);

        /// Handle when focus is lost (e.g., clicking outside editor)
        void OnFocusLost();

        /// Query current focus state
        bool IsPanelFocused(IEditorPanel* panel) const;
        IEditorPanel* GetFocusedPanel() const { return m_focusedPanel; }

        /// Query if a view that captures cursor (game window) is focused
        bool IsGameViewFocused() const;

        /// Manual cursor state management
        void SetCursorVisibility(bool visible);
        bool IsCursorVisible() const { return m_cursorVisible; }

        void SetCursorLocked(bool locked);
        bool IsCursorLocked() const { return m_cursorLocked; }

        /// Callbacks for external systems to respond to focus changes
        std::function<void(IEditorPanel*)> OnFocusChanged;  // Called when focus switches
        std::function<void(CursorBehaviorOnFocus)> OnCursorBehaviorChanged;

    private:
        struct FocusablePanel
        {
            IEditorPanel* panel;
            CursorBehaviorOnFocus cursorBehavior;
        };

        std::vector<FocusablePanel> m_registeredPanels;
        IEditorPanel* m_focusedPanel = nullptr;
        CursorBehaviorOnFocus m_currentCursorBehavior = CursorBehaviorOnFocus::Visible;

        bool m_cursorVisible = true;
        bool m_cursorLocked = false;

        void ApplyCursorBehavior(CursorBehaviorOnFocus behavior);
    };

} // namespace Engine::Editor
