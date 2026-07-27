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

namespace
{
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

bool NuklearUiBackend::HandleMessage(void* nativeWindow, uint32_t message,
    uintptr_t wParam, intptr_t lParam)
{
    if (!m_impl) return false;
    return m_inputHandler.HandleMessage(
        m_impl->context, nativeWindow, message, wParam, lParam);
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

    nk_context& context = m_impl->context;
    const float width = static_cast<float>(m_renderer->GetWidth());
    const float height = static_cast<float>(m_renderer->GetHeight());
    const NuklearDockGeometry geometry = m_dockLayout.Update(context, width, height);

    m_toolbar.Draw(context, width, state, playState, buildManager,
        [this, &state](const char* type)
        {
            m_panelHost.OpenPanel(state, type);
        });
    m_panelHost.DrawDockedPanels(context, m_editorUi, state, geometry);
    m_dockLayout.DrawSplitters(context, geometry);
    m_panelHost.DrawPreferences(m_editorUi, state, width, height);
    m_panelHost.CleanupClosedPanels(state);
}
