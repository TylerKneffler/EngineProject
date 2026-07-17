#include "pch.h"

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_API extern "C"
#include "nuklear.h"

#include "NuklearUiBackend.h"
#include "Engine/Editor/UI/ImGui/ImGuiUiBackend.h"
#include "imgui.h"
#include "Core/Renderers/IEditorRenderer.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/GameBuildManager.h"
#include "Engine/Editor/Core/View/IEditorPanel.h"
#include "Engine/Editor/Core/View/View.h"
#include "Engine/Editor/Core/View/ViewFactory.h"
#include "Engine/Editor/Core/View/Views/HierarchyView.h"
#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Engine/Editor/Core/View/Views/ConsoleView.h"
#include "Engine/Editor/Core/View/Views/AssetsExplorerView.h"
#include "Engine/Editor/Core/View/Views/PreferencesView.h"
#include "Core/Compoonents/Camera.h"

namespace
{
template <typename T>
T* FindPanel(EditorState& state)
{
    for (auto& panel : state.GetPanels())
        if (auto* result = dynamic_cast<T*>(panel.get()))
            return result;
    return nullptr;
}

float FontTextWidth(nk_handle handle, float height, const char* text, int length)
{
    auto* font = static_cast<ImFont*>(handle.ptr);
    if (!font || !text || length <= 0) return 0.f;
    return font->CalcTextSizeA(height, FLT_MAX, 0.f, text, text + length).x;
}

void QueryFontGlyph(nk_handle handle, float height, nk_user_font_glyph* output,
    nk_rune codepoint, nk_rune)
{
    auto* font = static_cast<ImFont*>(handle.ptr);
    if (!font || !output) return;
    ImFontGlyph* glyph = font->GetFontBaked(height)->FindGlyph(static_cast<ImWchar>(codepoint));
    if (!glyph) return;
    output->uv[0] = nk_vec2(glyph->U0, glyph->V0);
    output->uv[1] = nk_vec2(glyph->U1, glyph->V1);
    output->offset = nk_vec2(glyph->X0, glyph->Y0);
    output->width = glyph->X1 - glyph->X0;
    output->height = glyph->Y1 - glyph->Y0;
    output->xadvance = glyph->AdvanceX;
}

int MouseX(intptr_t lParam) { return static_cast<short>(LOWORD(lParam)); }
int MouseY(intptr_t lParam) { return static_cast<short>(HIWORD(lParam)); }
}

struct NuklearUiBackend::Impl
{
    nk_context context{};
    nk_user_font font{};
    nk_buffer commands{};
    nk_buffer vertices{};
    nk_buffer indices{};
    std::unique_ptr<ImDrawList> drawList;
    ImDrawData drawData;
    bool contextReady = false;
    bool fontReady = false;

    ~Impl()
    {
        if (contextReady) nk_free(&context);
        nk_buffer_free(&commands);
        nk_buffer_free(&vertices);
        nk_buffer_free(&indices);
    }

    bool Initialize()
    {
        contextReady = nk_init_default(&context, nullptr) != 0;
        if (!contextReady) return false;
        nk_buffer_init_default(&commands);
        nk_buffer_init_default(&vertices);
        nk_buffer_init_default(&indices);
        return true;
    }

    void EnsureFont()
    {
        ImFont* imguiFont = ImGui::GetFont();
        if (!imguiFont) return;
        font.userdata = nk_handle_ptr(imguiFont);
        font.height = ImGui::GetFontSize();
        font.width = FontTextWidth;
        font.query = QueryFontGlyph;
        font.texture = nk_handle_ptr(reinterpret_cast<void*>(
            static_cast<uintptr_t>(ImGui::GetIO().Fonts->TexRef.GetTexID())));
        if (!fontReady)
        {
            nk_style_set_font(&context, &font);
            fontReady = true;
        }
    }

    ImDrawData* Convert(float width, float height)
    {
        EnsureFont();
        nk_buffer_clear(&commands);
        nk_buffer_clear(&vertices);
        nk_buffer_clear(&indices);

        static const nk_draw_vertex_layout_element layout[] = {
            { NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(ImDrawVert, pos) },
            { NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(ImDrawVert, uv) },
            { NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, NK_OFFSETOF(ImDrawVert, col) },
            { NK_VERTEX_ATTRIBUTE_COUNT, NK_FORMAT_COUNT, 0 }
        };
        nk_convert_config config{};
        config.vertex_layout = layout;
        config.vertex_size = sizeof(ImDrawVert);
        config.vertex_alignment = NK_ALIGNOF(ImDrawVert);
        config.tex_null.texture = font.texture;
        const ImVec2 whitePixel = ImGui::GetFontTexUvWhitePixel();
        config.tex_null.uv = nk_vec2(whitePixel.x, whitePixel.y);
        config.circle_segment_count = 22;
        config.curve_segment_count = 22;
        config.arc_segment_count = 22;
        config.global_alpha = 1.f;
        config.shape_AA = NK_ANTI_ALIASING_ON;
        config.line_AA = NK_ANTI_ALIASING_ON;

        const nk_flags result = nk_convert(&context, &commands, &vertices, &indices, &config);
        if (result != NK_CONVERT_SUCCESS)
        {
            nk_clear(&context);
            return nullptr;
        }

        if (!drawList)
            drawList = std::make_unique<ImDrawList>(ImGui::GetDrawListSharedData());
        drawList->CmdBuffer.clear();
        drawList->IdxBuffer.clear();
        drawList->VtxBuffer.clear();

        const int vertexCount = static_cast<int>(nk_buffer_total(&vertices) / sizeof(ImDrawVert));
        const int indexCount = static_cast<int>(nk_buffer_total(&indices) / sizeof(ImDrawIdx));
        drawList->VtxBuffer.resize(vertexCount);
        drawList->IdxBuffer.resize(indexCount);
        if (vertexCount)
            memcpy(drawList->VtxBuffer.Data, nk_buffer_memory_const(&vertices),
                   static_cast<size_t>(vertexCount) * sizeof(ImDrawVert));
        if (indexCount)
            memcpy(drawList->IdxBuffer.Data, nk_buffer_memory_const(&indices),
                   static_cast<size_t>(indexCount) * sizeof(ImDrawIdx));

        unsigned int indexOffset = 0;
        const nk_draw_command* command = nullptr;
        nk_draw_foreach(command, &context, &commands)
        {
            if (!command->elem_count) continue;
            ImDrawCmd drawCommand;
            drawCommand.ClipRect = ImVec4(command->clip_rect.x, command->clip_rect.y,
                command->clip_rect.x + command->clip_rect.w,
                command->clip_rect.y + command->clip_rect.h);
            // The font atlas may not have a GPU handle until this very draw
            // submission. Preserve ImGui's deferred atlas reference instead
            // of freezing an invalid first-frame texture ID.
            drawCommand.TexRef = command->texture.ptr
                ? ImTextureRef(static_cast<ImTextureID>(
                    reinterpret_cast<uintptr_t>(command->texture.ptr)))
                : ImGui::GetIO().Fonts->TexRef;
            drawCommand.IdxOffset = indexOffset;
            drawCommand.ElemCount = command->elem_count;
            drawList->CmdBuffer.push_back(drawCommand);
            indexOffset += command->elem_count;
        }

        drawData.Clear();
        drawData.Valid = true;
        drawData.DisplayPos = ImVec2(0.f, 0.f);
        drawData.DisplaySize = ImVec2(width, height);
        drawData.FramebufferScale = ImVec2(1.f, 1.f);
        drawData.OwnerViewport = ImGui::GetMainViewport();
        drawData.Textures = &ImGui::GetPlatformIO().Textures;
        drawData.CmdLists.push_back(drawList.get());
        drawData.CmdListsCount = 1;
        drawData.TotalVtxCount = vertexCount;
        drawData.TotalIdxCount = indexCount;
        nk_clear(&context);
        return &drawData;
    }
};

NuklearUiBackend::NuklearUiBackend() = default;
NuklearUiBackend::~NuklearUiBackend() { Shutdown(); }

bool NuklearUiBackend::Initialize(void*, IEditorRenderer&)
{
    return false; // A persistent graphics bridge is required; use InitializeWithBridge.
}

bool NuklearUiBackend::InitializeWithBridge(void*, IEditorRenderer& renderer,
    ImGuiUiBackend& renderBridge)
{
    Shutdown();
    m_impl = std::make_unique<Impl>();
    if (!m_impl->Initialize())
    {
        m_impl.reset();
        return false;
    }
    m_renderer = &renderer;
    m_renderBridge = &renderBridge;
    m_editorUi.SetContext(&m_impl->context);
    return true;
}

void NuklearUiBackend::Shutdown()
{
    m_impl.reset();
    m_renderer = nullptr;
    m_renderBridge = nullptr;
    m_inputOpen = false;
}

void NuklearUiBackend::SetSwitchCallback(std::function<void(EditorUiKind)> callback)
{
    m_switchCallback = std::move(callback);
}

void NuklearUiBackend::RequestSwitch(EditorUiKind kind)
{
    if (m_switchCallback) m_switchCallback(kind);
}

bool NuklearUiBackend::HandleMessage(void* nativeWindow, uint32_t message,
    uintptr_t wParam, intptr_t lParam)
{
    if (!m_impl) return false;
    nk_context* context = &m_impl->context;
    const int x = MouseX(lParam), y = MouseY(lParam);
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (message == WM_KEYDOWN || message == WM_KEYUP ||
        message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)
    {
        switch (wParam)
        {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: nk_input_key(context, NK_KEY_SHIFT, down); return true;
        case VK_DELETE: nk_input_key(context, NK_KEY_DEL, down); return true;
        case VK_RETURN: nk_input_key(context, NK_KEY_ENTER, down); return true;
        case VK_TAB: nk_input_key(context, NK_KEY_TAB, down); return true;
        case VK_UP: nk_input_key(context, NK_KEY_UP, down); return true;
        case VK_DOWN: nk_input_key(context, NK_KEY_DOWN, down); return true;
        case VK_LEFT: nk_input_key(context, ctrl ? NK_KEY_TEXT_WORD_LEFT : NK_KEY_LEFT, down); return true;
        case VK_RIGHT: nk_input_key(context, ctrl ? NK_KEY_TEXT_WORD_RIGHT : NK_KEY_RIGHT, down); return true;
        case VK_BACK: nk_input_key(context, NK_KEY_BACKSPACE, down); return true;
        case VK_HOME: nk_input_key(context, NK_KEY_TEXT_START, down); nk_input_key(context, NK_KEY_SCROLL_START, down); return true;
        case VK_END: nk_input_key(context, NK_KEY_TEXT_END, down); nk_input_key(context, NK_KEY_SCROLL_END, down); return true;
        case VK_NEXT: nk_input_key(context, NK_KEY_SCROLL_DOWN, down); return true;
        case VK_PRIOR: nk_input_key(context, NK_KEY_SCROLL_UP, down); return true;
        case 'A': if (ctrl) { nk_input_key(context, NK_KEY_TEXT_SELECT_ALL, down); return true; } break;
        case 'C': if (ctrl) { nk_input_key(context, NK_KEY_COPY, down); return true; } break;
        case 'V': if (ctrl) { nk_input_key(context, NK_KEY_PASTE, down); return true; } break;
        case 'X': if (ctrl) { nk_input_key(context, NK_KEY_CUT, down); return true; } break;
        case 'Z': if (ctrl) { nk_input_key(context, NK_KEY_TEXT_UNDO, down); return true; } break;
        case 'R': if (ctrl) { nk_input_key(context, NK_KEY_TEXT_REDO, down); return true; } break;
        }
        return false;
    }
    switch (message)
    {
    case WM_CHAR: if (wParam >= 32) { nk_input_unicode(context, static_cast<nk_rune>(wParam)); return true; } break;
    case WM_LBUTTONDOWN: nk_input_button(context, NK_BUTTON_LEFT, x, y, 1); SetCapture(static_cast<HWND>(nativeWindow)); return true;
    case WM_LBUTTONUP: nk_input_button(context, NK_BUTTON_LEFT, x, y, 0); nk_input_button(context, NK_BUTTON_DOUBLE, x, y, 0); ReleaseCapture(); return true;
    case WM_LBUTTONDBLCLK: nk_input_button(context, NK_BUTTON_DOUBLE, x, y, 1); return true;
    case WM_RBUTTONDOWN: nk_input_button(context, NK_BUTTON_RIGHT, x, y, 1); SetCapture(static_cast<HWND>(nativeWindow)); return true;
    case WM_RBUTTONUP: nk_input_button(context, NK_BUTTON_RIGHT, x, y, 0); ReleaseCapture(); return true;
    case WM_MBUTTONDOWN: nk_input_button(context, NK_BUTTON_MIDDLE, x, y, 1); SetCapture(static_cast<HWND>(nativeWindow)); return true;
    case WM_MBUTTONUP: nk_input_button(context, NK_BUTTON_MIDDLE, x, y, 0); ReleaseCapture(); return true;
    case WM_MOUSEMOVE: nk_input_motion(context, x, y); return true;
    case WM_MOUSEWHEEL: nk_input_scroll(context, nk_vec2(0.f, static_cast<float>(static_cast<short>(HIWORD(wParam))) / WHEEL_DELTA)); return true;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
    case WM_KILLFOCUS:
    {
        const int mouseX = static_cast<int>(context->input.mouse.pos.x);
        const int mouseY = static_cast<int>(context->input.mouse.pos.y);
        nk_input_button(context, NK_BUTTON_LEFT, mouseX, mouseY, 0);
        nk_input_button(context, NK_BUTTON_DOUBLE, mouseX, mouseY, 0);
        nk_input_button(context, NK_BUTTON_RIGHT, mouseX, mouseY, 0);
        nk_input_button(context, NK_BUTTON_MIDDLE, mouseX, mouseY, 0);
        return false;
    }
    }
    return false;
}

void NuklearUiBackend::Resize(uint32_t, uint32_t) {}
void NuklearUiBackend::BeginInput() { if (m_impl && !m_inputOpen) { nk_input_begin(&m_impl->context); m_inputOpen = true; } }
void NuklearUiBackend::EndInput() { if (m_impl && m_inputOpen) { nk_input_end(&m_impl->context); m_inputOpen = false; } }
void NuklearUiBackend::BeginFrame() { if (m_impl) m_impl->EnsureFont(); }

void NuklearUiBackend::Render(void* commandBuffer)
{
    if (!m_impl || !m_renderer || !m_renderBridge) return;
    m_renderBridge->EndHiddenFrame();
    if (ImDrawData* data = m_impl->Convert(static_cast<float>(m_renderer->GetWidth()),
                                           static_cast<float>(m_renderer->GetHeight())))
        m_renderBridge->RenderDrawData(data, commandBuffer);
}

void NuklearUiBackend::EndFrame() {}

void NuklearUiBackend::DrawEditor(EditorState& state, PlayState playState,
    GameBuildManager* buildManager)
{
    if (!m_impl || !m_renderer) return;
    nk_context* m_context = &m_impl->context;
    const float width = static_cast<float>(m_renderer->GetWidth());
    const float height = static_cast<float>(m_renderer->GetHeight());
    constexpr float toolbarHeight = 76.f;
    const float consoleHeight = std::max(150.f, height * 0.24f);
    const float leftWidth = std::max(220.f, width * 0.20f);
    const float rightWidth = std::max(260.f, width * 0.25f);
    const float middleWidth = std::max(100.f, width - leftWidth - rightWidth);
    const float workspaceHeight = std::max(100.f, height - toolbarHeight - consoleHeight);

    const struct nk_rect toolbarBounds = nk_rect(0, 0, width, toolbarHeight);
    nk_window_set_bounds(m_context, "Editor Toolbar", toolbarBounds);
    if (nk_begin(m_context, "Editor Toolbar", toolbarBounds,
        NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR))
    {
        const bool isBusy = playState == PlayState::Building ||
                            playState == PlayState::Playing ||
                            playState == PlayState::Paused;
        auto createPanel = [&](const char* type)
        {
            ViewFactory* factory = state.GetViewFactory();
            if (!factory) return;
            if (ViewFactory::IsSingleton(type))
            {
                const std::string titlePrefix = std::string(type) + " ";
                for (auto& existing : state.GetPanels())
                {
                    if (!existing || existing->GetTitle().rfind(titlePrefix, 0) != 0) continue;
                    existing->SetOpen(true);
                    if (dynamic_cast<HierarchyView*>(existing.get()) ||
                        dynamic_cast<AssetsExplorerView*>(existing.get()))
                        m_activeLeftTab = existing.get();
                    return;
                }
            }
            auto panel = factory->Create(type);
            if (panel)
            {
                IEditorPanel* created = panel.get();
                state.GetPanels().push_back(std::move(panel));
                if (created->NeedsRender()) m_activeCenterTab = created;
                else if (dynamic_cast<HierarchyView*>(created) ||
                         dynamic_cast<AssetsExplorerView*>(created))
                    m_activeLeftTab = created;
            }
        };

        nk_menubar_begin(m_context);
        nk_layout_row_begin(m_context, NK_STATIC, 24.f, 3);
        nk_layout_row_push(m_context, 55.f);
        if (nk_menu_begin_label(m_context, "File", NK_TEXT_LEFT, nk_vec2(285.f, 255.f)))
        {
            nk_layout_row_dynamic(m_context, 25.f, 1);
            if (nk_menu_item_label(m_context, "Save All        Ctrl+S", NK_TEXT_LEFT))
                state.SaveScene();

            if (isBusy) nk_widget_disable_begin(m_context);
            if (nk_menu_item_label(m_context, "Build           Ctrl+B", NK_TEXT_LEFT) && buildManager)
                buildManager->StartBuild(PostBuildAction::Nothing);
            if (nk_menu_item_label(m_context, "Build and Run in Editor", NK_TEXT_LEFT) && buildManager)
                buildManager->StartBuild(PostBuildAction::PlayInEditor);
            if (nk_menu_item_label(m_context, "Build and Run Standalone", NK_TEXT_LEFT) && buildManager)
                buildManager->StartBuild(PostBuildAction::LaunchStandalone);
            if (isBusy) nk_widget_disable_end(m_context);

            if (nk_menu_item_label(m_context, "Project Preferences", NK_TEXT_LEFT))
                state.SetShowPreferences(true);
            if (nk_menu_item_label(m_context, "Exit", NK_TEXT_LEFT))
                PostQuitMessage(0);
            nk_menu_end(m_context);
        }

        nk_layout_row_push(m_context, 60.f);
        if (nk_menu_begin_label(m_context, "Views", NK_TEXT_LEFT, nk_vec2(210.f, 190.f)))
        {
            nk_layout_row_dynamic(m_context, 25.f, 1);
            ViewFactory* factory = state.GetViewFactory();
            const bool no3D = !factory || !factory->CanCreate3DView();
            if (no3D) nk_widget_disable_begin(m_context);
            if (nk_menu_item_label(m_context, "Scene", NK_TEXT_LEFT)) createPanel("Scene");
            if (nk_menu_item_label(m_context, "Game", NK_TEXT_LEFT)) createPanel("Game");
            if (no3D) nk_widget_disable_end(m_context);
            if (nk_menu_item_label(m_context, "Hierarchy", NK_TEXT_LEFT)) createPanel("Hierarchy");
            if (nk_menu_item_label(m_context, "Properties", NK_TEXT_LEFT)) createPanel("Properties");
            if (nk_menu_item_label(m_context, "Assets", NK_TEXT_LEFT)) createPanel("Assets");
            if (nk_menu_item_label(m_context, "Console", NK_TEXT_LEFT)) createPanel("Console");
            nk_menu_end(m_context);
        }

        nk_layout_row_push(m_context, 45.f);
        if (nk_menu_begin_label(m_context, "UI", NK_TEXT_LEFT, nk_vec2(235.f, 80.f)))
        {
            nk_layout_row_dynamic(m_context, 25.f, 1);
            nk_widget_disable_begin(m_context);
            nk_menu_item_label(m_context, "Nuklear (active)", NK_TEXT_LEFT);
            nk_widget_disable_end(m_context);
            if (nk_menu_item_label(m_context, "Switch to Dear ImGui  Ctrl+Shift+U", NK_TEXT_LEFT))
                RequestSwitch(EditorUiKind::ImGui);
            nk_menu_end(m_context);
        }
        nk_layout_row_end(m_context);
        nk_menubar_end(m_context);

        nk_layout_row_dynamic(m_context, 30.f, 6);
        if (nk_button_label(m_context, "Save")) state.SaveScene();
        if (isBusy) nk_widget_disable_begin(m_context);
        if (nk_button_label(m_context, "Build") && buildManager) buildManager->StartBuild(PostBuildAction::Nothing);
        if (isBusy) nk_widget_disable_end(m_context);
        if (playState == PlayState::Stopped || playState == PlayState::BuildFailed)
        {
            if (nk_button_label(m_context, "Play") && buildManager) buildManager->StartBuild(PostBuildAction::PlayInEditor);
        }
        else if (nk_button_label(m_context, "Stop") && buildManager) buildManager->Stop();
        if (playState == PlayState::Playing)
        {
            if (nk_button_label(m_context, "Pause") && buildManager) buildManager->Pause();
        }
        else if (playState == PlayState::Paused)
        {
            if (nk_button_label(m_context, "Resume") && buildManager) buildManager->Resume();
        }
        if (nk_button_label(m_context, "Use ImGui")) RequestSwitch(EditorUiKind::ImGui);
        nk_label(m_context, "UI: Nuklear", NK_TEXT_RIGHT);
    }
    nk_end(m_context);

    std::vector<IEditorPanel*> leftTabs;
    std::vector<IEditorPanel*> centerTabs;
    IEditorPanel* propertiesPanel = nullptr;
    IEditorPanel* consolePanel = nullptr;
    for (auto& panel : state.GetPanels())
    {
        if (!panel || !panel->IsOpen()) continue;
        if (dynamic_cast<PropertiesView*>(panel.get())) propertiesPanel = panel.get();
        else if (dynamic_cast<ConsoleView*>(panel.get())) consolePanel = panel.get();
        else if (panel->NeedsRender()) centerTabs.push_back(panel.get());
        else leftTabs.push_back(panel.get());
    }

    auto ensureActive = [](IEditorPanel*& active, const std::vector<IEditorPanel*>& tabs)
    {
        if (std::find(tabs.begin(), tabs.end(), active) == tabs.end())
            active = tabs.empty() ? nullptr : tabs.front();
    };
    auto drawTabs = [&](const char* hostName, float x, float y, float w,
                        const std::vector<IEditorPanel*>& tabs, IEditorPanel*& active)
    {
        if (tabs.empty()) return;
        const struct nk_rect bounds = nk_rect(x, y, w, 32.f);
        nk_window_set_bounds(m_context, hostName, bounds);
        if (nk_begin(m_context, hostName, bounds, NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR))
        {
            nk_layout_row_dynamic(m_context, 24.f, static_cast<int>(tabs.size()));
            for (IEditorPanel* panel : tabs)
            {
                int selected = panel == active ? 1 : 0;
                if (nk_selectable_label(m_context, panel->GetTitle().c_str(), NK_TEXT_CENTERED, &selected))
                    active = panel;
            }
        }
        nk_end(m_context);
    };

    constexpr float tabHeight = 32.f;
    ensureActive(m_activeLeftTab, leftTabs);
    ensureActive(m_activeCenterTab, centerTabs);
    drawTabs("Left Dock Tabs", 0.f, toolbarHeight, leftWidth, leftTabs, m_activeLeftTab);
    drawTabs("Center Dock Tabs", leftWidth, toolbarHeight, middleWidth, centerTabs, m_activeCenterTab);

    if (m_activeLeftTab)
    {
        m_editorUi.SetNextWindowRect(0.f, toolbarHeight + tabHeight, leftWidth,
                                     workspaceHeight - tabHeight);
        m_activeLeftTab->DrawPanel(m_editorUi);
    }
    if (m_activeCenterTab)
    {
        m_editorUi.SetNextWindowRect(leftWidth, toolbarHeight + tabHeight, middleWidth,
                                     workspaceHeight - tabHeight);
        m_activeCenterTab->DrawPanel(m_editorUi);
    }
    if (propertiesPanel)
    {
        m_editorUi.SetNextWindowRect(leftWidth + middleWidth, toolbarHeight,
                                     rightWidth, workspaceHeight);
        propertiesPanel->DrawPanel(m_editorUi);
    }
    if (consolePanel)
    {
        m_editorUi.SetNextWindowRect(0.f, toolbarHeight + workspaceHeight,
                                     width, consoleHeight);
        consolePanel->DrawPanel(m_editorUi);
    }
    bool showPreferences = state.IsShowingPreferences();
    if (showPreferences && state.GetPreferences())
    {
        m_editorUi.SetNextWindowRect((width - 600.f) * .5f, 60.f, 600.f,
            std::max(400.f, height - 120.f));
        state.GetPreferences()->DrawWindow(m_editorUi, showPreferences);
        state.SetShowPreferences(showPreferences);
    }
}

void NuklearUiBackend::DrawHierarchy(EditorState& state, float x, float y, float w, float h)
{
    nk_context* m_context = &m_impl->context;
    if (nk_begin(m_context, "Hierarchy", nk_rect(x, y, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_SCROLL_AUTO_HIDE))
    {
        auto* hierarchy = FindPanel<HierarchyView>(state);
        Scene* scene = state.GetScene();
        if (!scene || !hierarchy) nk_label(m_context, "No scene loaded", NK_TEXT_LEFT);
        else for (const auto& object : scene->GetObjects()) if (!object->Parent) DrawObjectRow(object.get(), 0, hierarchy);
    }
    nk_end(m_context);
}

void NuklearUiBackend::DrawObjectRow(Object* object, int depth, HierarchyView* hierarchy)
{
    if (!object) return;
    nk_context* m_context = &m_impl->context;
    nk_layout_row_begin(m_context, NK_STATIC, 24.f, 2);
    nk_layout_row_push(m_context, static_cast<float>(depth * 14)); nk_spacing(m_context, 1);
    nk_layout_row_push(m_context, 190.f);
    std::string label = object == hierarchy->GetSelectedObject() ? "> " : "  ";
    label += object->name.empty() ? "(unnamed)" : object->name;
    if (nk_button_label(m_context, label.c_str())) hierarchy->SetSelectedObject(object);
    nk_layout_row_end(m_context);
    for (Object* child : object->Children) DrawObjectRow(child, depth + 1, hierarchy);
}

void NuklearUiBackend::DrawProperties(EditorState& state, float x, float y, float w, float h)
{
    nk_context* m_context = &m_impl->context;
    if (nk_begin(m_context, "Properties", nk_rect(x, y, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_SCROLL_AUTO_HIDE))
    {
        auto* properties = FindPanel<PropertiesView>(state);
        Object* object = properties ? properties->GetSelectedObject() : nullptr;
        if (!object) nk_label(m_context, "No object selected", NK_TEXT_LEFT);
        else
        {
            nk_layout_row_dynamic(m_context, 24.f, 1);
            nk_label(m_context, object->name.c_str(), NK_TEXT_LEFT);
            nk_property_float(m_context, "Position X", -10000.f, &object->transform.position.x, 10000.f, 0.1f, 0.01f);
            nk_property_float(m_context, "Position Y", -10000.f, &object->transform.position.y, 10000.f, 0.1f, 0.01f);
            nk_property_float(m_context, "Position Z", -10000.f, &object->transform.position.z, 10000.f, 0.1f, 0.01f);
            nk_property_float(m_context, "Rotation X", -6.283f, &object->transform.rotation.x, 6.283f, 0.01f, 0.005f);
            nk_property_float(m_context, "Rotation Y", -6.283f, &object->transform.rotation.y, 6.283f, 0.01f, 0.005f);
            nk_property_float(m_context, "Rotation Z", -6.283f, &object->transform.rotation.z, 6.283f, 0.01f, 0.005f);
            nk_property_float(m_context, "Scale X", 0.001f, &object->transform.scale.x, 1000.f, 0.01f, 0.005f);
            nk_property_float(m_context, "Scale Y", 0.001f, &object->transform.scale.y, 1000.f, 0.01f, 0.005f);
            nk_property_float(m_context, "Scale Z", 0.001f, &object->transform.scale.z, 1000.f, 0.01f, 0.005f);
        }
    }
    nk_end(m_context);
}

void NuklearUiBackend::DrawViewport(EditorState& state, float x, float y, float w, float h)
{
    nk_context* m_context = &m_impl->context;
    if (nk_begin(m_context, "Scene", nk_rect(x, y, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR))
    {
        View* sceneView = nullptr;
        for (auto& panel : state.GetPanels())
            if (panel && panel->NeedsRender() && panel->GetTitle().find("Scene") == 0) { sceneView = dynamic_cast<View*>(panel.get()); break; }
        const struct nk_rect content = nk_window_get_content_region(m_context);
        nk_layout_row_dynamic(m_context, std::max(1.f, content.h), 1);
        if (sceneView && sceneView->GetUiTextureHandle()) nk_image(m_context, nk_image_ptr(sceneView->GetUiTextureHandle()));
        else nk_label(m_context, "Scene render surface unavailable", NK_TEXT_CENTERED);
    }
    nk_end(m_context);
}

void NuklearUiBackend::DrawConsole(EditorState& state, float x, float y, float w, float h)
{
    nk_context* m_context = &m_impl->context;
    if (nk_begin(m_context, "Console", nk_rect(x, y, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_SCROLL_AUTO_HIDE))
    {
        ConsoleView* console = state.GetConsole();
        nk_layout_row_dynamic(m_context, 22.f, 1);
        if (console) for (const auto& entry : console->GetEntries()) nk_label(m_context, entry.message.c_str(), NK_TEXT_LEFT);
    }
    nk_end(m_context);
}
