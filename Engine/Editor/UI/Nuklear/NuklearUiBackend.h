#pragma once

#include "Engine/Editor/UI/IEditorUiBackend.h"
#include "Engine/Editor/UI/Nuklear/NuklearEditorUi.h"
#include <functional>

class Object;
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
    void RequestSwitch(EditorUiKind kind) override;
    void SetSwitchCallback(std::function<void(EditorUiKind)> callback);

private:
    void DrawHierarchy(EditorState& state, float x, float y, float w, float h);
    void DrawObjectRow(Object* object, int depth, class HierarchyView* hierarchy);
    void DrawProperties(EditorState& state, float x, float y, float w, float h);
    void DrawViewport(EditorState& state, float x, float y, float w, float h);
    void DrawConsole(EditorState& state, float x, float y, float w, float h);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    IEditorRenderer* m_renderer = nullptr;
    ImGuiUiBackend* m_renderBridge = nullptr;
    std::function<void(EditorUiKind)> m_switchCallback;
    bool m_inputOpen = false;
    NuklearEditorUi m_editorUi;
};
