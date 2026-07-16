#pragma once

#include "Engine/Editor/UI/IEditorUiBackend.h"
#include "Engine/Editor/UI/ImGui/ImGuiUiBackend.h"
#include "Engine/Editor/UI/Nuklear/NuklearUiBackend.h"
#include <optional>

// Keeps the renderer integration alive while presentation packages change.
// Switch requests are committed only after the current frame has finished.
class SwitchableEditorUiBackend final : public IEditorUiBackend
{
public:
    explicit SwitchableEditorUiBackend(EditorUiKind initialKind);
    ~SwitchableEditorUiBackend() override;

    const char* Name() const override;
    bool Initialize(void* nativeWindow, IEditorRenderer& renderer) override;
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
    EditorUiKind ActiveKind() const override { return m_activeKind; }
    void RequestSwitch(EditorUiKind kind) override;

private:
    IEditorUiBackend& ActiveBackend();

    EditorUiKind m_activeKind;
    std::optional<EditorUiKind> m_pendingKind;
    ImGuiUiBackend m_imgui;
    NuklearUiBackend m_nuklear;
    bool m_initialized = false;
};
