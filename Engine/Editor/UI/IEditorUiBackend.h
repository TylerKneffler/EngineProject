#pragma once

#include <cstdint>
#include <memory>

class IEditorRenderer;
class EditorState;
class GameBuildManager;
enum class PlayState;

enum class EditorUiKind
{
    ImGui,
    Nuklear
};

// Owns one concrete UI package's platform and graphics integration. Editor
// presentation code talks to the package-neutral UI facade separately.
class IEditorUiBackend
{
public:
    virtual ~IEditorUiBackend() = default;

    virtual const char* Name() const = 0;
    virtual bool Initialize(void* nativeWindow, IEditorRenderer& renderer) = 0;
    virtual void Shutdown() = 0;
    virtual bool HandleMessage(void* nativeWindow, uint32_t message,
                               uintptr_t wParam, intptr_t lParam) = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual void BeginInput() {}
    virtual void EndInput() {}
    virtual void BeginFrame() = 0;
    virtual void Render(void* commandBuffer) = 0;
    virtual void EndFrame() = 0;
    virtual void DrawEditor(EditorState& state, PlayState playState,
                            GameBuildManager* buildManager) = 0;
    virtual EditorUiKind ActiveKind() const = 0;
    virtual void RequestSwitch(EditorUiKind kind) = 0;
};

std::unique_ptr<IEditorUiBackend> CreateEditorUiBackend();
