#pragma once

#include "Engine/Editor/UI/IEditorUiBackend.h"

struct ImDrawData;

namespace Engine::Editor
{
class EditorUI;

class ImGuiUiBackend final : public IEditorUiBackend
{
public:
    ImGuiUiBackend();
    ~ImGuiUiBackend() override;

    const char* Name() const override { return "ImGui"; }
    bool Initialize(void* nativeWindow, ::Engine::Renderers::IEditorRenderer& renderer) override;
    void Shutdown() override;
    bool HandleMessage(void* nativeWindow, uint32_t message,
                       uintptr_t wParam, intptr_t lParam) override;
    void Resize(uint32_t width, uint32_t height) override;
    void BeginFrame() override;
    bool NeedsContinuousRendering() const override;
    void Render(void* commandBuffer) override;
    void EndFrame() override;
    void DrawEditor(EditorState& state, PlayState playState,
                    GameBuildManager* buildManager) override;
private:
    void RenderDrawData(::ImDrawData* drawData, void* commandBuffer);
    enum class GraphicsApi { None, DirectX11, DirectX12, Vulkan };
    GraphicsApi m_graphicsApi = GraphicsApi::None;
    ::Engine::Renderers::IEditorRenderer* m_renderer = nullptr;
    bool m_initialized = false;
    std::unique_ptr<EditorUI> m_presentation;
    EditorState* m_editorState = nullptr;
    GameBuildManager* m_buildManager = nullptr;
};
}
