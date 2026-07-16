#pragma once

#include "Engine/Editor/UI/IEditorUiBackend.h"
#include <functional>

class EditorUI;

class ImGuiUiBackend final : public IEditorUiBackend
{
public:
    ImGuiUiBackend();
    ~ImGuiUiBackend() override;

    const char* Name() const override { return "ImGui"; }
    bool Initialize(void* nativeWindow, IEditorRenderer& renderer) override;
    void Shutdown() override;
    bool HandleMessage(void* nativeWindow, uint32_t message,
                       uintptr_t wParam, intptr_t lParam) override;
    void Resize(uint32_t width, uint32_t height) override;
    void BeginFrame() override;
    void Render(void* commandBuffer) override;
    void EndFrame() override;
    void DrawEditor(EditorState& state, PlayState playState,
                    GameBuildManager* buildManager) override;
    EditorUiKind ActiveKind() const override { return EditorUiKind::ImGui; }
    void RequestSwitch(EditorUiKind) override {}

    void DrawEditorPresentation(EditorState& state, PlayState playState,
                                GameBuildManager* buildManager,
                                const std::function<void()>& switchToNuklear);
    void RenderDrawData(struct ImDrawData* drawData, void* commandBuffer);
    void EndHiddenFrame();

private:
    enum class GraphicsApi { None, DirectX11, DirectX12, Vulkan };
    GraphicsApi m_graphicsApi = GraphicsApi::None;
    IEditorRenderer* m_renderer = nullptr;
    bool m_initialized = false;
    std::unique_ptr<EditorUI> m_presentation;
    std::function<void()> m_switchToNuklear;
};
