#pragma once

#include "Engine/Editor/UI/IEditorUiBackend.h"
#include "Engine/Editor/UI/Nuklear/Layout/NuklearDockLayout.h"
#include "Engine/Editor/UI/Nuklear/NuklearEditorUi.h"
#include "Engine/Editor/UI/Nuklear/Input/NuklearInputHandler.h"
#include "Engine/Editor/UI/Nuklear/Panels/NuklearPanelHost.h"
#include "Engine/Editor/UI/Nuklear/Menus/NuklearToolbar.h"

class ImGuiUiBackend;

// Nuklear owns editor layout, widgets, and input. Its renderer-independent
// triangle output is submitted through the shared editor UI graphics bridge.
class NuklearUiBackend final : public IEditorUiBackend
{
public:
    NuklearUiBackend();
    ~NuklearUiBackend() override;

    const char* Name() const override { return "Nuklear"; }
    bool Initialize(void* nativeWindow, IEditorRenderer& renderer) override;
    bool InitializeWithBridge(void* nativeWindow, IEditorRenderer& renderer,
                              ImGuiUiBackend& renderBridge);
    void Shutdown() override;
    bool HandleMessage(void* nativeWindow, uint32_t message,
                       uintptr_t wParam, intptr_t lParam) override;
    void Resize(uint32_t width, uint32_t height) override;
    void BeginInput() override;
    void EndInput() override;
    void BeginFrame() override;
    void Render(void* commandBuffer) override;
    void EndFrame() override;
    void DrawEditor(EditorState& state, PlayState playState,
                    GameBuildManager* buildManager) override;
    EditorUiKind ActiveKind() const override { return EditorUiKind::Nuklear; }
    void RequestSwitch(EditorUiKind) override {}

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    IEditorRenderer* m_renderer = nullptr;
    ImGuiUiBackend* m_renderBridge = nullptr;
    bool m_inputOpen = false;
    NuklearInputHandler m_inputHandler;
    NuklearEditorUi m_editorUi;
    NuklearDockLayout m_dockLayout;
    NuklearToolbar m_toolbar;
    NuklearPanelHost m_panelHost;
};
