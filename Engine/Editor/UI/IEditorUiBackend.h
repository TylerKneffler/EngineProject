#pragma once

#include <cstdint>
#include <memory>
#include "Core/Renderers/IEditorRenderer.h"

namespace Engine::Editor
{
class EditorState;
class GameBuildManager;
enum class PlayState;

// Owns the editor UI package's platform and graphics integration.
class IEditorUiBackend
{
public:
    virtual ~IEditorUiBackend() = default;

    virtual const char* Name() const = 0;
    virtual bool Initialize(void* nativeWindow, ::Engine::Renderers::IEditorRenderer& renderer) = 0;
    virtual void Shutdown() = 0;
    virtual bool HandleMessage(void* nativeWindow, uint32_t message,
                               uintptr_t wParam, intptr_t lParam) = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual void BeginInput() {}
    virtual void EndInput() {}
    virtual void BeginFrame() = 0;
    virtual bool NeedsContinuousRendering() const { return false; }
    virtual void Render(void* commandBuffer) = 0;
    virtual void EndFrame() = 0;
    virtual void DrawEditor(EditorState& state, PlayState playState,
                            GameBuildManager* buildManager) = 0;
};

std::unique_ptr<IEditorUiBackend> CreateEditorUiBackend();
}
