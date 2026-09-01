#include "Core/Renderers/UIRenderer.h"

#include "Core/UI/UILayout.h"
#include "Core/Compoonents/UI/Canvas.h"
#include "Core/Compoonents/UI/UIButton.h"
#include "Core/Compoonents/UI/UIImage.h"
#include "Core/Compoonents/UI/UIObject.h"
#include "Core/Compoonents/UI/UIText.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Graphics/IShader.h"
#include "Core/Scene/Scene.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#endif

#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif


namespace Engine::Renderers
{
namespace
{
constexpr int kFirstGlyph = 32;
constexpr int kLastGlyph = 126;
constexpr int kGlyphCount = kLastGlyph - kFirstGlyph + 1;
constexpr int kAtlasSize = 2048;
constexpr unsigned int kFontOversampling = 2;

struct Glyph
{
    float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
    float xOffset = 0.f, yOffset = 0.f;
    float width = 0.f, height = 0.f;
    float advance = 0.f;
};

struct FontAtlas
{
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> texture;
    std::array<Glyph, kGlyphCount> glyphs{};
    std::array<float, kGlyphCount * kGlyphCount> kerning{};
    float ascent = 0.f;
    float lineHeight = 16.f;
    float pixelHeight = 16.f;

    float Kerning(int first, int second) const
    {
        if (first < kFirstGlyph || first > kLastGlyph ||
            second < kFirstGlyph || second > kLastGlyph)
            return 0.f;
        return kerning[(first - kFirstGlyph) * kGlyphCount +
            (second - kFirstGlyph)];
    }
};

struct UIVertex
{
    glm::vec2 position{};
    glm::vec2 uv{};
    glm::vec4 color{ 1.f };
};

struct DrawSegment
{
    Engine::Graphics::IGraphicsTexture* texture = nullptr;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    bool fontSdf = false;
};

Engine::Model::UIRect Intersect(const Engine::Model::UIRect& first,
    const Engine::Model::UIRect& second)
{
    const float left = std::max(first.x, second.x);
    const float top = std::max(first.y, second.y);
    const float right = std::min(first.x + first.width, second.x + second.width);
    const float bottom = std::min(first.y + first.height, second.y + second.height);
    return { left, top, std::max(0.f, right - left), std::max(0.f, bottom - top) };
}

std::filesystem::path ResolveFontPath(const std::string& requested)
{
    if (!requested.empty())
    {
        std::filesystem::path path(requested);
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension == ".ttf" || extension == ".otf")
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(path, error)) return path;

            const std::filesystem::path relative =
                path.lexically_relative(std::filesystem::path("Assets"));
            if (!relative.empty() && *relative.begin() != "..")
            {
                const auto bundled =
                    std::filesystem::path(ENGINE_ASSETS_PATH) / relative;
                if (std::filesystem::is_regular_file(bundled, error))
                    return bundled;
            }

            const auto enginePath = std::filesystem::path(ENGINE_ASSETS_PATH) / path;
            if (std::filesystem::is_regular_file(enginePath, error)) return enginePath;
        }
    }
#ifdef _WIN32
    const std::filesystem::path segoe("C:/Windows/Fonts/segoeui.ttf");
    if (std::filesystem::is_regular_file(segoe)) return segoe;
    const std::filesystem::path arial("C:/Windows/Fonts/arial.ttf");
    if (std::filesystem::is_regular_file(arial)) return arial;
#endif
    return {};
}

std::vector<unsigned char> ReadBinary(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const std::streamsize size = input.tellg();
    if (size <= 0) return {};
    std::vector<unsigned char> data(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(data.data()), size);
    return input ? data : std::vector<unsigned char>{};
}

float Measure(const std::string& value, const FontAtlas& atlas, float scale)
{
    float width = 0.f;
    int previous = -1;
    for (unsigned char character : value)
    {
        const int codepoint = character >= kFirstGlyph && character <= kLastGlyph
            ? character : '?';
        if (previous >= 0)
            width += atlas.Kerning(previous, codepoint) * scale;
        width += atlas.glyphs[codepoint - kFirstGlyph].advance * scale;
        previous = codepoint;
    }
    return width;
}

std::vector<std::string> WrapText(const Engine::Components::UIText& text, const FontAtlas& atlas,
    float maxWidth, float scale)
{
    std::vector<std::string> lines;
    std::string line;
    std::string word;
    auto flushWord = [&]()
    {
        if (word.empty()) return;
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (text.wordWrap && !line.empty() && Measure(candidate, atlas, scale) > maxWidth)
        {
            lines.push_back(line);
            line = word;
        }
        else line = candidate;
        word.clear();
    };
    for (char character : text.text)
    {
        if (character == '\n')
        {
            flushWord();
            lines.push_back(line);
            line.clear();
        }
        else if (character == ' ' || character == '\t') flushWord();
        else word.push_back(character);
    }
    flushWord();
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}

bool ClipQuad(float& x0, float& y0, float& x1, float& y1,
    float& u0, float& v0, float& u1, float& v1, const Engine::Model::UIRect& clip)
{
    if (x1 <= clip.x || y1 <= clip.y || x0 >= clip.x + clip.width ||
        y0 >= clip.y + clip.height) return false;
    const float originalX0 = x0, originalY0 = y0;
    const float originalX1 = x1, originalY1 = y1;
    const float originalU0 = u0, originalV0 = v0;
    const float originalU1 = u1, originalV1 = v1;
    const float originalWidth = originalX1 - originalX0;
    const float originalHeight = originalY1 - originalY0;
    if (originalWidth <= 0.f || originalHeight <= 0.f) return false;
    x0 = std::max(originalX0, clip.x);
    y0 = std::max(originalY0, clip.y);
    x1 = std::min(originalX1, clip.x + clip.width);
    y1 = std::min(originalY1, clip.y + clip.height);
    u0 = originalU0 + (originalU1 - originalU0) * ((x0 - originalX0) / originalWidth);
    u1 = originalU0 + (originalU1 - originalU0) * ((x1 - originalX0) / originalWidth);
    v0 = originalV0 + (originalV1 - originalV0) * ((y0 - originalY0) / originalHeight);
    v1 = originalV0 + (originalV1 - originalV0) * ((y1 - originalY0) / originalHeight);
    return x1 > x0 && y1 > y0;
}

glm::vec2 ToNdc(float x, float y, const glm::vec2& canvas)
{
    return { x / canvas.x * 2.f - 1.f, 1.f - y / canvas.y * 2.f };
}

void AddQuad(std::vector<UIVertex>& vertices, float x0, float y0, float x1, float y1,
    float u0, float v0, float u1, float v1, const glm::vec4& color,
    const glm::vec2& canvas)
{
    const glm::vec2 topLeft = ToNdc(x0, y0, canvas);
    const glm::vec2 topRight = ToNdc(x1, y0, canvas);
    const glm::vec2 bottomLeft = ToNdc(x0, y1, canvas);
    const glm::vec2 bottomRight = ToNdc(x1, y1, canvas);
    vertices.insert(vertices.end(), {
        { topLeft, {u0,v0}, color }, { topRight, {u1,v0}, color },
        { bottomRight, {u1,v1}, color }, { topLeft, {u0,v0}, color },
        { bottomRight, {u1,v1}, color }, { bottomLeft, {u0,v1}, color }
    });
}
}

struct UIRenderer::Impl
{
    Engine::Graphics::IGraphicsProvider* provider = nullptr;
    std::unique_ptr<Engine::Graphics::IPipelineState> imagePipeline;
    std::unique_ptr<Engine::Graphics::IPipelineState> fontPipeline;
    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> vertexBuffer;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> whiteTexture;
    std::size_t vertexCapacity = 0;
    std::unordered_map<std::string, std::unique_ptr<FontAtlas>> atlases;
    glm::vec2 pointerPosition{};
    glm::vec2 pointerViewportSize{ 1.f };
    bool hasExternalPointerInput = false;
    bool externalPointerHovered = false;
    bool externalPointerDown = false;

    FontAtlas* GetAtlas(const std::string& requested, float requestedPixelHeight)
    {
        const std::filesystem::path fontPath = ResolveFontPath(requested);
        if (fontPath.empty() || !provider || !provider->GetTextureFactory()) return nullptr;
        const int pixelHeight = std::clamp(
            static_cast<int>(std::lround(requestedPixelHeight)), 8, 256);
        const std::string key = fontPath.lexically_normal().generic_string() +
            "#" + std::to_string(pixelHeight);
        if (auto found = atlases.find(key); found != atlases.end()) return found->second.get();

        const std::vector<unsigned char> fontData = ReadBinary(fontPath);
        if (fontData.empty()) return nullptr;
        stbtt_fontinfo font{};
        const int offset = stbtt_GetFontOffsetForIndex(fontData.data(), 0);
        if (offset < 0 || !stbtt_InitFont(&font, fontData.data(), offset)) return nullptr;

        auto atlas = std::make_unique<FontAtlas>();
        atlas->pixelHeight = static_cast<float>(pixelHeight);
        std::vector<uint8_t> coverage(kAtlasSize * kAtlasSize, 0);
        std::array<stbtt_packedchar, kGlyphCount> packed{};
        stbtt_pack_context packing{};
        if (!stbtt_PackBegin(&packing, coverage.data(), kAtlasSize,
            kAtlasSize, 0, 2, nullptr))
            return nullptr;
        stbtt_PackSetOversampling(&packing,
            kFontOversampling, kFontOversampling);
        const int packedSuccessfully = stbtt_PackFontRange(&packing,
            fontData.data(), 0, static_cast<float>(pixelHeight),
            kFirstGlyph, kGlyphCount, packed.data());
        stbtt_PackEnd(&packing);
        if (!packedSuccessfully)
            return nullptr;

        const float fontScale = stbtt_ScaleForPixelHeight(
            &font, static_cast<float>(pixelHeight));
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
        atlas->ascent = ascent * fontScale;
        atlas->lineHeight = (ascent - descent + lineGap) * fontScale;

        for (int codepoint = kFirstGlyph; codepoint <= kLastGlyph; ++codepoint)
        {
            Glyph& glyph = atlas->glyphs[codepoint - kFirstGlyph];
            const stbtt_packedchar& source = packed[codepoint - kFirstGlyph];
            glyph.u0 = static_cast<float>(source.x0) / kAtlasSize;
            glyph.v0 = static_cast<float>(source.y0) / kAtlasSize;
            glyph.u1 = static_cast<float>(source.x1) / kAtlasSize;
            glyph.v1 = static_cast<float>(source.y1) / kAtlasSize;
            glyph.xOffset = source.xoff;
            glyph.yOffset = source.yoff;
            glyph.width = source.xoff2 - source.xoff;
            glyph.height = source.yoff2 - source.yoff;
            glyph.advance = source.xadvance;
        }

        for (int first = kFirstGlyph; first <= kLastGlyph; ++first)
            for (int second = kFirstGlyph; second <= kLastGlyph; ++second)
                atlas->kerning[(first - kFirstGlyph) * kGlyphCount +
                    (second - kFirstGlyph)] = fontScale *
                        stbtt_GetCodepointKernAdvance(&font, first, second);

        std::vector<uint8_t> pixels(kAtlasSize * kAtlasSize * 4, 255);
        for (std::size_t index = 0; index < coverage.size(); ++index)
            pixels[index * 4 + 3] = coverage[index];
        atlas->texture = provider->GetTextureFactory()->CreateTexture2D(
            kAtlasSize, kAtlasSize, pixels.data(), 1, Engine::Graphics::GraphicsTextureFormat::Rgba8, false);
        if (!atlas->texture) return nullptr;
        FontAtlas* result = atlas.get();
        atlases.emplace(key, std::move(atlas));
        return result;
    }
};

UIRenderer::UIRenderer() : m_impl(new Impl()) {}
UIRenderer::~UIRenderer() { delete m_impl; }

void UIRenderer::Initialize(Engine::Graphics::IGraphicsProvider* graphicsProvider)
{
    if (!m_impl || !graphicsProvider) return;
    m_impl->provider = graphicsProvider;
    auto* compiler = graphicsProvider->GetShaderCompiler();
    auto* factory = graphicsProvider->GetPipelineStateFactory();
    if (!compiler || !factory) return;
    const std::filesystem::path shader = std::filesystem::path(ENGINE_SHADERS_PATH) / "UI" / "UI.hlsl";
    const std::filesystem::path fontShader =
        std::filesystem::path(ENGINE_SHADERS_PATH) / "UIFont" / "UIFont.hlsl";
    auto vertexShader = compiler->CompileFromFile(shader.string().c_str(), "VSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::VS_5_0);
    auto imagePixelShader = compiler->CompileFromFile(shader.string().c_str(), "PSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    auto fontPixelShader = compiler->CompileFromFile(fontShader.string().c_str(), "PSMain",
        Engine::Graphics::IShaderCompiler::CompileProfile::PS_5_0);
    if (!vertexShader || !imagePixelShader || !fontPixelShader) return;
    Engine::Graphics::IPipelineStateBuilder::VertexElement layout[] = {
        { "POSITION", 0, 16, 0, 0, false },
        { "TEXCOORD", 0, 16, 0, 8, false },
        { "COLOR", 0, 2, 0, 16, false }
    };
    const auto buildPipeline = [&](const Engine::Graphics::IShader* pixelShader)
    {
        auto builder = factory->CreateBuilder();
        if (!builder) return std::unique_ptr<Engine::Graphics::IPipelineState>{};
        return builder->SetVertexShader(vertexShader.get())
            .SetPixelShader(pixelShader).SetFillMode(false).SetCullMode(false)
            .SetFrontCounterClockwise(false).SetDepthClipEnable(false).SetBlendEnable(true)
            .SetSrcBlend(4).SetDestBlend(5).SetBlendOp(0)
            .SetSrcBlendAlpha(1).SetDestBlendAlpha(0).SetBlendOpAlpha(0)
            .SetDepthEnable(false).SetDepthWriteEnable(false).SetDepthFunc(7)
            .SetInputLayout(layout, 3)
            .SetPrimitiveTopology(Engine::Graphics::IPipelineStateBuilder::PrimitiveTopology::TriangleList)
            .SetRenderTargetFormat(28, 20).Build();
    };
    m_impl->imagePipeline = buildPipeline(imagePixelShader.get());
    m_impl->fontPipeline = buildPipeline(fontPixelShader.get());

    auto* textureFactory = graphicsProvider->GetTextureFactory();
    if (textureFactory)
    {
        const uint8_t whitePixel[4] = { 255, 255, 255, 255 };
        m_impl->whiteTexture = textureFactory->CreateTexture2D(
            1, 1, whitePixel, 1,
            Engine::Graphics::GraphicsTextureFormat::Rgba8, false);
    }
}

bool UIRenderer::IsReady() const
{
    return m_impl && m_impl->imagePipeline && m_impl->fontPipeline;
}

void UIRenderer::SetPointerInput(float x, float y, float viewportWidth,
    float viewportHeight, bool hovered, bool mouseDown)
{
    if (!m_impl) return;
    m_impl->pointerPosition = { x, y };
    m_impl->pointerViewportSize = {
        std::max(1.f, viewportWidth), std::max(1.f, viewportHeight) };
    m_impl->hasExternalPointerInput = true;
    m_impl->externalPointerHovered = hovered;
    m_impl->externalPointerDown = mouseDown;
}

void UIRenderer::Render(Engine::Scene::Scene& scene,
    Engine::Graphics::IGraphicsContext* context, float viewportAspect)
{
    if (!IsReady() || !context || !m_impl->provider) return;
    const std::vector<Engine::Model::UITextLayout> items = Engine::UI::UILayout::Resolve(scene, viewportAspect);

    glm::vec2 mouseClientPosition = m_impl->pointerPosition;
    glm::vec2 mouseClientSize = m_impl->pointerViewportSize;
    bool hasMousePosition = false;
    bool mouseDown = m_impl->externalPointerDown;
    if (m_impl->hasExternalPointerInput)
        hasMousePosition = m_impl->externalPointerHovered;
#ifdef _WIN32
    if (!m_impl->hasExternalPointerInput)
    {
        if (HWND hwnd = GetForegroundWindow())
        {
            POINT cursor{};
            if (GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor))
            {
                RECT client{};
                if (GetClientRect(hwnd, &client))
                {
                    const float width = static_cast<float>(std::max(1L, client.right - client.left));
                    const float height = static_cast<float>(std::max(1L, client.bottom - client.top));
                    mouseClientPosition.x = static_cast<float>(cursor.x);
                    mouseClientPosition.y = static_cast<float>(cursor.y);
                    mouseClientSize = { width, height };
                    hasMousePosition = true;
                }
            }
        }
        mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    }
#endif

    std::vector<UIVertex> vertices;
    std::vector<DrawSegment> segments;

    if (m_impl->whiteTexture)
    {
        std::vector<std::pair<Engine::Components::Canvas*, glm::vec2>> canvasSizes;
        for (const auto& candidate : scene.GetObjects())
        {
            Engine::Core::Object* object = candidate.get();
            Engine::Components::Canvas* canvas = object && object->IsEnabledInHierarchy()
                ? object->GetComponent<Engine::Components::Canvas>() : nullptr;
            if (!canvas)
                continue;
            bool isNestedCanvas = false;
            for (Engine::Core::Object* ancestor = object->Parent; ancestor; ancestor = ancestor->Parent)
            {
                if (ancestor->GetComponent<Engine::Components::Canvas>())
                {
                    isNestedCanvas = true;
                    break;
                }
            }
            if (!isNestedCanvas)
                canvasSizes.emplace_back(canvas, canvas->GetLogicalSize(viewportAspect));
        }

        // Images are backgrounds in the retained UI stack: they render before
        // button state overlays and text while respecting the computed rect
        // and parent clip from UIObject.
        for (const auto& candidate : scene.GetObjects())
        {
            Engine::Core::Object* object = candidate.get();
            if (!object || !object->IsEnabledInHierarchy()) continue;
            auto* layout = object->GetComponent<Engine::Components::UIObject>();
            auto* image = object->GetComponent<Engine::Components::UIImage>();
            if (!layout || !image || !layout->visible ||
                image->fillAmount <= 0.f || !image->Prepare(m_impl->provider))
                continue;
            const Engine::Components::Texture* texture = image->GetTexture();
            if (!texture || !texture->GetGraphicsTexture() ||
                texture->GetWidth() == 0 || texture->GetHeight() == 0)
                continue;

            Engine::Components::Canvas* canvas = nullptr;
            for (Engine::Core::Object* current = object; current; current = current->Parent)
            {
                canvas = current->GetComponent<Engine::Components::Canvas>();
                if (canvas) break;
            }
            if (!canvas) continue;
            glm::vec2 canvasSize(1.f);
            for (const auto& entry : canvasSizes)
                if (entry.first == canvas) { canvasSize = entry.second; break; }

            const Engine::Model::UIRect& rect = layout->GetComputedRect();
            if (rect.width <= 0.f || rect.height <= 0.f) continue;
            float x0 = rect.x, y0 = rect.y;
            float x1 = rect.x + rect.width, y1 = rect.y + rect.height;
            float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
            std::string fit = image->fitMode;
            std::transform(fit.begin(), fit.end(), fit.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            const float sourceAspect = static_cast<float>(texture->GetWidth()) /
                static_cast<float>(texture->GetHeight());
            const float targetAspect = rect.height > 0.f ? rect.width / rect.height : sourceAspect;
            if (fit == "contain")
            {
                if (sourceAspect > targetAspect)
                {
                    const float height = rect.width / sourceAspect;
                    y0 += (rect.height - height) * 0.5f;
                    y1 = y0 + height;
                }
                else
                {
                    const float width = rect.height * sourceAspect;
                    x0 += (rect.width - width) * 0.5f;
                    x1 = x0 + width;
                }
            }
            else if (fit == "cover")
            {
                if (sourceAspect > targetAspect)
                {
                    const float visible = targetAspect / sourceAspect;
                    u0 = (1.f - visible) * 0.5f;
                    u1 = u0 + visible;
                }
                else
                {
                    const float visible = sourceAspect / targetAspect;
                    v0 = (1.f - visible) * 0.5f;
                    v1 = v0 + visible;
                }
            }
            else if (fit == "native" || fit == "none")
            {
                const float width = static_cast<float>(texture->GetWidth());
                const float height = static_cast<float>(texture->GetHeight());
                x0 += (rect.width - width) * 0.5f;
                y0 += (rect.height - height) * 0.5f;
                x1 = x0 + width;
                y1 = y0 + height;
            }

            const float amount = std::clamp(image->fillAmount, 0.f, 1.f);
            std::string direction = image->fillDirection;
            std::transform(direction.begin(), direction.end(), direction.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (direction == "righttoleft")
            {
                x0 = x1 - (x1 - x0) * amount;
                u0 = u1 - (u1 - u0) * amount;
            }
            else if (direction == "toptobottom")
            {
                y1 = y0 + (y1 - y0) * amount;
                v1 = v0 + (v1 - v0) * amount;
            }
            else if (direction == "bottomtotop")
            {
                y0 = y1 - (y1 - y0) * amount;
                v0 = v1 - (v1 - v0) * amount;
            }
            else
            {
                x1 = x0 + (x1 - x0) * amount;
                u1 = u0 + (u1 - u0) * amount;
            }
            if (image->flipX) std::swap(u0, u1);
            if (image->flipY) std::swap(v0, v1);
            if (!ClipQuad(x0, y0, x1, y1, u0, v0, u1, v1,
                layout->GetComputedClipRect()))
                continue;

            const uint32_t first = static_cast<uint32_t>(vertices.size());
            AddQuad(vertices, x0, y0, x1, y1, u0, v0, u1, v1,
                glm::vec4(image->color, std::clamp(image->alpha, 0.f, 1.f)), canvasSize);
            auto* graphicsTexture = const_cast<Engine::Graphics::IGraphicsTexture*>(
                texture->GetGraphicsTexture());
            if (!segments.empty() && !segments.back().fontSdf &&
                segments.back().texture == graphicsTexture &&
                segments.back().firstVertex + segments.back().vertexCount == first)
                segments.back().vertexCount += 6;
            else
                segments.push_back({ graphicsTexture, first, 6, false });
        }

        const uint32_t firstButtonVertex = static_cast<uint32_t>(vertices.size());
        for (const auto& candidate : scene.GetObjects())
        {
            Engine::Core::Object* object = candidate.get();
            if (!object || !object->IsEnabledInHierarchy())
                continue;
            auto* layout = object->GetComponent<Engine::Components::UIObject>();
            auto* button = object->GetComponent<Engine::Components::UIButton>();
            if (!layout || !button || !layout->visible)
                continue;

            Engine::Components::Canvas* canvas = nullptr;
            for (Engine::Core::Object* current = object; current; current = current->Parent)
            {
                canvas = current->GetComponent<Engine::Components::Canvas>();
                if (canvas)
                    break;
            }
            if (!canvas)
                continue;

            glm::vec2 canvasSize(1.f);
            for (const auto& entry : canvasSizes)
            {
                if (entry.first == canvas)
                {
                    canvasSize = entry.second;
                    break;
                }
            }

            const Engine::Model::UIRect& rect = layout->GetComputedRect();
            const Engine::Model::UIRect& clip = layout->GetComputedClipRect();
            bool hovered = false;
            if (hasMousePosition)
            {
                const glm::vec2 mousePosition(
                    mouseClientPosition.x * (canvasSize.x / std::max(1.f, mouseClientSize.x)),
                    mouseClientPosition.y * (canvasSize.y / std::max(1.f, mouseClientSize.y)));
                hovered = mousePosition.x >= rect.x && mousePosition.x <= rect.x + rect.width &&
                    mousePosition.y >= rect.y && mousePosition.y <= rect.y + rect.height &&
                    mousePosition.x >= clip.x && mousePosition.x <= clip.x + clip.width &&
                    mousePosition.y >= clip.y && mousePosition.y <= clip.y + clip.height;
            }
            button->UpdateInteraction(hovered, mouseDown);

            const glm::vec3 rgb = !button->interactable
                ? button->disabledColor
                : (button->IsPressed()
                    ? button->pressedColor
                    : (button->IsHovered() ? button->hoverColor : button->normalColor));
            const glm::vec4 color(rgb, std::clamp(button->alpha, 0.f, 1.f));

            float x0 = rect.x;
            float y0 = rect.y;
            float x1 = rect.x + rect.width;
            float y1 = rect.y + rect.height;
            float u0 = 0.f;
            float v0 = 0.f;
            float u1 = 1.f;
            float v1 = 1.f;
            if (!ClipQuad(x0, y0, x1, y1, u0, v0, u1, v1, clip))
                continue;
            AddQuad(vertices, x0, y0, x1, y1, u0, v0, u1, v1, color, canvasSize);
        }

        const uint32_t buttonCount = static_cast<uint32_t>(vertices.size()) - firstButtonVertex;
        if (buttonCount > 0)
            segments.push_back({ m_impl->whiteTexture.get(), firstButtonVertex, buttonCount, false });
    }

    for (const Engine::Model::UITextLayout& item : items)
    {
        if (!item.layout || !item.text || item.text->text.empty()) continue;
        FontAtlas* atlas = m_impl->GetAtlas(
            item.text->fontPath, item.text->fontSize);
        if (!atlas) continue;
        const float scale = std::max(1.f, item.text->fontSize) /
            atlas->pixelHeight;
        const Engine::Model::UIRect& rect = item.layout->GetComputedRect();
        Engine::Model::UIRect clip = item.layout->GetComputedClipRect();
        if (item.text->overflow != "Visible") clip = Intersect(clip, rect);
        std::vector<std::string> lines = WrapText(*item.text, *atlas, rect.width, scale);
        const float lineHeight = atlas->lineHeight * scale * std::max(0.1f, item.text->lineSpacing);
        if (item.text->overflow != "Visible")
        {
            const std::size_t maximumLines = static_cast<std::size_t>(
                std::max(0.f, std::floor(rect.height / std::max(1.f, lineHeight))));
            if (lines.size() > maximumLines)
            {
                lines.resize(maximumLines);
                if (item.text->overflow == "Ellipsis" && !lines.empty())
                {
                    std::string& last = lines.back();
                    while (!last.empty() && Measure(last + "...", *atlas, scale) > rect.width)
                        last.pop_back();
                    last += "...";
                }
            }
        }
        const float blockHeight = lines.size() * lineHeight;
        float top = rect.y;
        if (item.text->verticalAlignment == "Center") top += (rect.height - blockHeight) * 0.5f;
        else if (item.text->verticalAlignment == "Bottom") top += rect.height - blockHeight;
        const glm::vec4 color(item.text->color, std::clamp(item.text->alpha, 0.f, 1.f));
        const uint32_t firstVertex = static_cast<uint32_t>(vertices.size());
        for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex)
        {
            const float lineWidth = Measure(lines[lineIndex], *atlas, scale);
            float cursor = rect.x;
            if (item.text->horizontalAlignment == "Center") cursor += (rect.width - lineWidth) * 0.5f;
            else if (item.text->horizontalAlignment == "Right") cursor += rect.width - lineWidth;
            const float baseline = top + lineIndex * lineHeight + atlas->ascent * scale;
            int previous = -1;
            for (unsigned char character : lines[lineIndex])
            {
                const int codepoint = character >= kFirstGlyph && character <= kLastGlyph ? character : '?';
                if (previous >= 0)
                    cursor += atlas->Kerning(previous, codepoint) * scale;
                const Glyph& glyph = atlas->glyphs[codepoint - kFirstGlyph];
                float x0 = cursor + glyph.xOffset * scale;
                float y0 = baseline + glyph.yOffset * scale;
                float x1 = x0 + glyph.width * scale;
                float y1 = y0 + glyph.height * scale;
                float u0 = glyph.u0, v0 = glyph.v0, u1 = glyph.u1, v1 = glyph.v1;
                if (item.text->overflow == "Visible" || ClipQuad(x0, y0, x1, y1,
                    u0, v0, u1, v1, clip))
                    AddQuad(vertices, x0, y0, x1, y1, u0, v0, u1, v1, color, item.canvasSize);
                cursor += glyph.advance * scale;
                previous = codepoint;
            }
        }
        const uint32_t count = static_cast<uint32_t>(vertices.size()) - firstVertex;
        if (!count) continue;
        if (!segments.empty() && segments.back().fontSdf &&
            segments.back().texture == atlas->texture.get() &&
            segments.back().firstVertex + segments.back().vertexCount == firstVertex)
            segments.back().vertexCount += count;
        else segments.push_back({ atlas->texture.get(), firstVertex, count, true });
    }
    if (vertices.empty()) return;

    if (!m_impl->vertexBuffer || m_impl->vertexCapacity < vertices.size())
    {
        m_impl->vertexCapacity = std::max<std::size_t>(vertices.size(),
            std::max<std::size_t>(1024, m_impl->vertexCapacity * 2));
        m_impl->vertexBuffer = m_impl->provider->GetBufferFactory()->CreateBuffer(
            Engine::Graphics::IGraphicsBuffer::Usage::VertexBuffer, Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
            m_impl->vertexCapacity * sizeof(UIVertex));
    }
    if (!m_impl->vertexBuffer) return;
    if (void* mapped = m_impl->vertexBuffer->Map())
    {
        std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(UIVertex));
        m_impl->vertexBuffer->Unmap();
    }
    else return;

    context->SetVertexBuffer(0, m_impl->vertexBuffer.get(), sizeof(UIVertex));
    bool activeFontPipeline = false;
    bool hasActivePipeline = false;
    for (const DrawSegment& segment : segments)
    {
        if (!hasActivePipeline || activeFontPipeline != segment.fontSdf)
        {
            activeFontPipeline = segment.fontSdf;
            hasActivePipeline = true;
            context->SetPipeline(activeFontPipeline
                ? m_impl->fontPipeline.get() : m_impl->imagePipeline.get());
        }
        context->SetTexture(0, segment.texture);
        context->DrawInstanced(segment.vertexCount, 1, segment.firstVertex, 0);
    }
}
}
