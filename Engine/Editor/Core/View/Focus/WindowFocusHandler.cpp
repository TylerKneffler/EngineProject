#include "pch.h"
#include "WindowFocusHandler.h"
#include "Engine/Editor/Core/View/IEditorPanel.h"
#include <algorithm>

namespace Engine::Editor
{
    WindowFocusHandler::WindowFocusHandler()
        : m_focusedPanel(nullptr)
        , m_currentCursorBehavior(CursorBehaviorOnFocus::Visible)
        , m_cursorVisible(true)
        , m_cursorLocked(false)
    {
    }

    WindowFocusHandler::~WindowFocusHandler() = default;

    void WindowFocusHandler::RegisterPanel(IEditorPanel* panel, CursorBehaviorOnFocus cursorBehavior)
    {
        if (!panel) return;

        // Check if already registered
        auto it = std::find_if(m_registeredPanels.begin(), m_registeredPanels.end(),
            [panel](const FocusablePanel& fp) { return fp.panel == panel; });

        if (it == m_registeredPanels.end())
        {
            m_registeredPanels.emplace_back(FocusablePanel{ panel, cursorBehavior });
        }
    }

    void WindowFocusHandler::UnregisterPanel(IEditorPanel* panel)
    {
        if (!panel) return;

        auto it = std::find_if(m_registeredPanels.begin(), m_registeredPanels.end(),
            [panel](const FocusablePanel& fp) { return fp.panel == panel; });

        if (it != m_registeredPanels.end())
        {
            m_registeredPanels.erase(it);

            // If the focused panel is unregistered, clear focus
            if (m_focusedPanel == panel)
            {
                m_focusedPanel = nullptr;
                ApplyCursorBehavior(CursorBehaviorOnFocus::Visible);
            }
        }
    }

    void WindowFocusHandler::OnPanelFocused(IEditorPanel* panel)
    {
        if (!panel) return;

        // Find the focus configuration for this panel
        auto it = std::find_if(m_registeredPanels.begin(), m_registeredPanels.end(),
            [panel](const FocusablePanel& fp) { return fp.panel == panel; });

        if (it != m_registeredPanels.end())
        {
            // Only process if focus actually changed
            if (m_focusedPanel != panel)
            {
                m_focusedPanel = panel;
                m_currentCursorBehavior = it->cursorBehavior;

                // Apply the cursor behavior for the newly focused panel
                ApplyCursorBehavior(it->cursorBehavior);

                // Fire callbacks
                if (OnFocusChanged)
                    OnFocusChanged(panel);

                if (OnCursorBehaviorChanged)
                    OnCursorBehaviorChanged(it->cursorBehavior);

                OutputDebugStringA("[WindowFocusHandler] Panel focused, cursor behavior changed\n");
            }
        }
    }

    void WindowFocusHandler::OnFocusLost()
    {
        m_focusedPanel = nullptr;
        ApplyCursorBehavior(CursorBehaviorOnFocus::Visible);

        if (OnFocusChanged)
            OnFocusChanged(nullptr);

        OutputDebugStringA("[WindowFocusHandler] Focus lost\n");
    }

    bool WindowFocusHandler::IsPanelFocused(IEditorPanel* panel) const
    {
        return m_focusedPanel == panel;
    }

    bool WindowFocusHandler::IsGameViewFocused() const
    {
        if (!m_focusedPanel) return false;

        // Check if the focused panel's cursor behavior is one that captures/confines
        return m_currentCursorBehavior == CursorBehaviorOnFocus::Captured ||
               m_currentCursorBehavior == CursorBehaviorOnFocus::ConfiedHidden;
    }

    void WindowFocusHandler::SetCursorVisibility(bool visible)
    {
        if (m_cursorVisible == visible) return;

        m_cursorVisible = visible;

        if (visible)
        {
            ShowCursor(TRUE);
            OutputDebugStringA("[WindowFocusHandler] Cursor shown\n");
        }
        else
        {
            ShowCursor(FALSE);
            OutputDebugStringA("[WindowFocusHandler] Cursor hidden\n");
        }
    }

    void WindowFocusHandler::SetCursorLocked(bool locked)
    {
        if (m_cursorLocked == locked) return;

        m_cursorLocked = locked;

        if (locked)
        {
            HWND foreground = GetForegroundWindow();
            if (foreground)
            {
                RECT rect;
                GetClientRect(foreground, &rect);
                POINT center = { rect.right / 2, rect.bottom / 2 };
                ClientToScreen(foreground, &center);
                SetCursorPos(center.x, center.y);
                OutputDebugStringA("[WindowFocusHandler] Cursor locked to center\n");
            }
        }
        else
        {
            OutputDebugStringA("[WindowFocusHandler] Cursor unlocked\n");
        }
    }

    void WindowFocusHandler::ApplyCursorBehavior(CursorBehaviorOnFocus behavior)
    {
        switch (behavior)
        {
        case CursorBehaviorOnFocus::Visible:
            SetCursorVisibility(true);
            SetCursorLocked(false);
            break;

        case CursorBehaviorOnFocus::Hidden:
            SetCursorVisibility(false);
            SetCursorLocked(false);
            break;

        case CursorBehaviorOnFocus::Captured:
            SetCursorVisibility(false);
            SetCursorLocked(true);
            break;

        case CursorBehaviorOnFocus::Confined:
            SetCursorVisibility(true);
            SetCursorLocked(false);  // Confined but not locked to center
            break;

        case CursorBehaviorOnFocus::ConfiedHidden:
            SetCursorVisibility(false);
            SetCursorLocked(false);  // Confined but not locked to center
            break;
        }
    }

} // namespace Engine::Editor
