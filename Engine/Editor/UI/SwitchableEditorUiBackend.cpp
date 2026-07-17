#include "pch.h"
#include "SwitchableEditorUiBackend.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"

SwitchableEditorUiBackend::SwitchableEditorUiBackend(EditorUiKind initialKind)
    : m_activeKind(initialKind)
{
    m_nuklear.SetSwitchCallback([this](EditorUiKind kind) { RequestSwitch(kind); });
}

SwitchableEditorUiBackend::~SwitchableEditorUiBackend() { Shutdown(); }

const char* SwitchableEditorUiBackend::Name() const
{
    return m_activeKind == EditorUiKind::ImGui ? "ImGui" : "Nuklear";
}

bool SwitchableEditorUiBackend::Initialize(void* nativeWindow, IEditorRenderer& renderer)
{
    Shutdown();
    if (!m_imgui.Initialize(nativeWindow, renderer)) return false;
    if (!m_nuklear.InitializeWithBridge(nativeWindow, renderer, m_imgui))
    {
        m_imgui.Shutdown();
        return false;
    }
    m_initialized = true;
    return true;
}

void SwitchableEditorUiBackend::Shutdown()
{
    m_nuklear.Shutdown();
    m_imgui.Shutdown();
    m_pendingKind.reset();
    m_editorState = nullptr;
    m_buildManager = nullptr;
    m_initialized = false;
}

IEditorUiBackend& SwitchableEditorUiBackend::ActiveBackend()
{
    return m_activeKind == EditorUiKind::ImGui
        ? static_cast<IEditorUiBackend&>(m_imgui)
        : static_cast<IEditorUiBackend&>(m_nuklear);
}

bool SwitchableEditorUiBackend::HandleMessage(void* nativeWindow, uint32_t message,
    uintptr_t wParam, intptr_t lParam)
{
    const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool firstPress = (static_cast<uintptr_t>(lParam) & (uintptr_t{1} << 30)) == 0;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (keyDown && firstPress && control && shift && wParam == 'U')
    {
        RequestSwitch(m_activeKind == EditorUiKind::ImGui
            ? EditorUiKind::Nuklear : EditorUiKind::ImGui);
        return true;
    }
    if (keyDown && firstPress && control && !shift && wParam == 'S' && m_editorState)
    {
        m_editorState->SaveScene();
        return true;
    }
    if (keyDown && firstPress && control && !shift && wParam == 'B' && m_buildManager)
    {
        const PlayState state = m_buildManager->GetPlayState();
        if (state == PlayState::Stopped || state == PlayState::BuildFailed)
            m_buildManager->StartBuild(PostBuildAction::Nothing);
        return true;
    }
    return m_initialized && ActiveBackend().HandleMessage(nativeWindow, message, wParam, lParam);
}

void SwitchableEditorUiBackend::Resize(uint32_t width, uint32_t height)
{
    m_imgui.Resize(width, height);
    m_nuklear.Resize(width, height);
}

void SwitchableEditorUiBackend::BeginInput() { ActiveBackend().BeginInput(); }
void SwitchableEditorUiBackend::EndInput() { ActiveBackend().EndInput(); }

void SwitchableEditorUiBackend::BeginFrame()
{
    m_imgui.BeginFrame();
    if (m_activeKind == EditorUiKind::Nuklear) m_nuklear.BeginFrame();
}

void SwitchableEditorUiBackend::Render(void* commandBuffer)
{
    if (m_activeKind == EditorUiKind::ImGui) m_imgui.Render(commandBuffer);
    else m_nuklear.Render(commandBuffer);
}

void SwitchableEditorUiBackend::EndFrame()
{
    if (m_activeKind == EditorUiKind::ImGui) m_imgui.EndFrame();
    else m_nuklear.EndFrame();

    // The old package has now fully rendered, so changing input/presentation
    // routing cannot mix two packages inside one frame.
    if (m_pendingKind)
    {
        m_activeKind = *m_pendingKind;
        m_pendingKind.reset();
    }
}

void SwitchableEditorUiBackend::DrawEditor(EditorState& state, PlayState playState,
    GameBuildManager* buildManager)
{
    m_editorState = &state;
    m_buildManager = buildManager;
    if (m_activeKind == EditorUiKind::ImGui)
        m_imgui.DrawEditorPresentation(state, playState, buildManager,
            [this]() { RequestSwitch(EditorUiKind::Nuklear); });
    else
        m_nuklear.DrawEditor(state, playState, buildManager);
}

void SwitchableEditorUiBackend::RequestSwitch(EditorUiKind kind)
{
    if (kind != m_activeKind) m_pendingKind = kind;
}

std::unique_ptr<IEditorUiBackend> CreateEditorUiBackend(const std::string& initialPackage)
{
    if (initialPackage == "Nuklear" || initialPackage == "NUKLEAR")
        return std::make_unique<SwitchableEditorUiBackend>(EditorUiKind::Nuklear);
    if (initialPackage == "ImGui" || initialPackage == "IMGUI")
        return std::make_unique<SwitchableEditorUiBackend>(EditorUiKind::ImGui);
#if defined(ENGINE_EDITOR_UI_NUKLEAR)
    return std::make_unique<SwitchableEditorUiBackend>(EditorUiKind::Nuklear);
#else
    return std::make_unique<SwitchableEditorUiBackend>(EditorUiKind::ImGui);
#endif
}
