#include "Core/Renderers/UIRenderer.h"

#include "Core/UI/UILayout.h"
#include "Core/Compoonents/UI/UIText.h"
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
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <vector>

#ifndef ENGINE_SHADERS_PATH
#define ENGINE_SHADERS_PATH "Engine/Core/Shaders/"
#endif

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace
{
constexpr int kFirstGlyph = 32;
constexpr int kLastGlyph = 126;
constexpr int kAtlasSize = 1024;
constexpr float kAtlasFontHeight = 48.f;

struct Glyph
{
    float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
    float xOffset = 0.f, yOffset = 0.f;
    float width = 0.f, height = 0.f;
    float advance = 0.f;
};

struct FontAtlas
{
    std::shared_ptr<IGraphicsTexture> texture;
    std::array<Glyph, kLastGlyph - kFirstGlyph + 1> glyphs{};
    float ascent = 0.f;
    float lineHeight = kAtlasFontHeight;
};

struct UIVertex
{
    glm::vec2 position{};
    glm::vec2 uv{};
    glm::vec4 color{ 1.f };
};

struct DrawSegment
{
    FontAtlas* atlas = nullptr;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
};

UIRect Intersect(const UIRect& first, const UIRect& second)
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
        if (std::filesystem::is_regular_file(path)) return path;
        const auto enginePath = std::filesystem::path(ENGINE_ASSETS_PATH) / path;
        if (std::filesystem::is_regular_file(enginePath)) return enginePath;
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
    for (unsigned char character : value)
    {
        const int codepoint = character >= kFirstGlyph && character <= kLastGlyph
            ? character : '?';
        width += atlas.glyphs[codepoint - kFirstGlyph].advance * scale;
    }
    return width;
}

std::vector<std::string> WrapText(const UIText& text, const FontAtlas& atlas,
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
    float& u0, float& v0, float& u1, float& v1, const UIRect& clip)
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
    IGraphicsProvider* provider = nullptr;
    std::unique_ptr<IPipelineState> pipeline;
    std::unique_ptr<IGraphicsBuffer> vertexBuffer;
    std::size_t vertexCapacity = 0;
    std::unordered_map<std::string, std::unique_ptr<FontAtlas>> atlases;

    FontAtlas* GetAtlas(const std::string& requested)
    {
        const std::filesystem::path fontPath = ResolveFontPath(requested);
        if (fontPath.empty() || !provider || !provider->GetTextureFactory()) return nullptr;
        const std::string key = fontPath.lexically_normal().generic_string();
        if (auto found = atlases.find(key); found != atlases.end()) return found->second.get();

        const std::vector<unsigned char> fontData = ReadBinary(fontPath);
        if (fontData.empty()) return nullptr;
        stbtt_fontinfo font{};
        const int offset = stbtt_GetFontOffsetForIndex(fontData.data(), 0);
        if (offset < 0 || !stbtt_InitFont(&font, fontData.data(), offset)) return nullptr;

        auto atlas = std::make_unique<FontAtlas>();
        std::vector<uint8_t> pixels(kAtlasSize * kAtlasSize * 4, 255);
        for (std::size_t index = 0; index < pixels.size() / 4; ++index) pixels[index * 4 + 3] = 0;
        const float fontScale = stbtt_ScaleForPixelHeight(&font, kAtlasFontHeight);
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
        atlas->ascent = ascent * fontScale;
        atlas->lineHeight = (ascent - descent + lineGap) * fontScale;

        int penX = 1, penY = 1, rowHeight = 0;
        for (int codepoint = kFirstGlyph; codepoint <= kLastGlyph; ++codepoint)
        {
            Glyph& glyph = atlas->glyphs[codepoint - kFirstGlyph];
            int advance = 0, bearing = 0;
            stbtt_GetCodepointHMetrics(&font, codepoint, &advance, &bearing);
            glyph.advance = advance * fontScale;
            int width = 0, height = 0, xOffset = 0, yOffset = 0;
            unsigned char* sdf = stbtt_GetCodepointSDF(&font, fontScale, codepoint,
                5, 128, 32.f, &width, &height, &xOffset, &yOffset);
            if (!sdf || width <= 0 || height <= 0) continue;
            if (penX + width + 1 >= kAtlasSize)
            { penX = 1; penY += rowHeight + 1; rowHeight = 0; }
            if (penY + height + 1 >= kAtlasSize)
            { stbtt_FreeSDF(sdf, nullptr); return nullptr; }
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    pixels[((penY + y) * kAtlasSize + penX + x) * 4 + 3] = sdf[y * width + x];
            glyph.u0 = static_cast<float>(penX) / kAtlasSize;
            glyph.v0 = static_cast<float>(penY) / kAtlasSize;
            glyph.u1 = static_cast<float>(penX + width) / kAtlasSize;
            glyph.v1 = static_cast<float>(penY + height) / kAtlasSize;
            glyph.xOffset = static_cast<float>(xOffset);
            glyph.yOffset = static_cast<float>(yOffset);
            glyph.width = static_cast<float>(width);
            glyph.height = static_cast<float>(height);
            penX += width + 1;
            rowHeight = std::max(rowHeight, height);
            stbtt_FreeSDF(sdf, nullptr);
        }
        atlas->texture = provider->GetTextureFactory()->CreateTexture2D(
            kAtlasSize, kAtlasSize, pixels.data(), 1, GraphicsTextureFormat::Rgba8, false);
        if (!atlas->texture) return nullptr;
        FontAtlas* result = atlas.get();
        atlases.emplace(key, std::move(atlas));
        return result;
    }
};

UIRenderer::UIRenderer() : m_impl(new Impl()) {}
UIRenderer::~UIRenderer() { delete m_impl; }

void UIRenderer::Initialize(IGraphicsProvider* graphicsProvider)
{
    if (!m_impl || !graphicsProvider) return;
    m_impl->provider = graphicsProvider;
    auto* compiler = graphicsProvider->GetShaderCompiler();
    auto* factory = graphicsProvider->GetPipelineStateFactory();
    if (!compiler || !factory) return;
    const std::filesystem::path shader = std::filesystem::path(ENGINE_SHADERS_PATH) / "UI" / "UI.hlsl";
    auto vertexShader = compiler->CompileFromFile(shader.string().c_str(), "VSMain",
        IShaderCompiler::CompileProfile::VS_5_0);
    auto pixelShader = compiler->CompileFromFile(shader.string().c_str(), "PSMain",
        IShaderCompiler::CompileProfile::PS_5_0);
    if (!vertexShader || !pixelShader) return;
    IPipelineStateBuilder::VertexElement layout[] = {
        { "POSITION", 0, 16, 0, 0, false },
        { "TEXCOORD", 0, 16, 0, 8, false },
        { "COLOR", 0, 2, 0, 16, false }
    };
    auto builder = factory->CreateBuilder();
    if (!builder) return;
    m_impl->pipeline = builder->SetVertexShader(vertexShader.get())
        .SetPixelShader(pixelShader.get()).SetFillMode(false).SetCullMode(false)
        .SetFrontCounterClockwise(true).SetDepthClipEnable(false).SetBlendEnable(true)
        .SetSrcBlend(4).SetDestBlend(5).SetBlendOp(0)
        .SetSrcBlendAlpha(1).SetDestBlendAlpha(0).SetBlendOpAlpha(0)
        .SetDepthEnable(false).SetDepthWriteEnable(false).SetDepthFunc(7)
        .SetInputLayout(layout, 3)
        .SetPrimitiveTopology(IPipelineStateBuilder::PrimitiveTopology::TriangleList)
        .SetRenderTargetFormat(28, 40).Build();
}

bool UIRenderer::IsReady() const { return m_impl && m_impl->pipeline; }

void UIRenderer::Render(Scene& scene, IGraphicsContext* context, float viewportAspect)
{
    if (!IsReady() || !context || !m_impl->provider) return;
    const std::vector<UITextLayout> items = UILayout::Resolve(scene, viewportAspect);
    std::vector<UIVertex> vertices;
    std::vector<DrawSegment> segments;
    for (const UITextLayout& item : items)
    {
        if (!item.layout || !item.text || item.text->text.empty()) continue;
        FontAtlas* atlas = m_impl->GetAtlas(item.text->fontPath);
        if (!atlas) continue;
        const float scale = std::max(1.f, item.text->fontSize) / kAtlasFontHeight;
        const UIRect& rect = item.layout->GetComputedRect();
        UIRect clip = item.layout->GetComputedClipRect();
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
            for (unsigned char character : lines[lineIndex])
            {
                const int codepoint = character >= kFirstGlyph && character <= kLastGlyph ? character : '?';
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
            }
        }
        const uint32_t count = static_cast<uint32_t>(vertices.size()) - firstVertex;
        if (!count) continue;
        if (!segments.empty() && segments.back().atlas == atlas &&
            segments.back().firstVertex + segments.back().vertexCount == firstVertex)
            segments.back().vertexCount += count;
        else segments.push_back({ atlas, firstVertex, count });
    }
    if (vertices.empty()) return;

    if (!m_impl->vertexBuffer || m_impl->vertexCapacity < vertices.size())
    {
        m_impl->vertexCapacity = std::max<std::size_t>(vertices.size(),
            std::max<std::size_t>(1024, m_impl->vertexCapacity * 2));
        m_impl->vertexBuffer = m_impl->provider->GetBufferFactory()->CreateBuffer(
            IGraphicsBuffer::Usage::VertexBuffer, IGraphicsBuffer::AccessMode::Upload,
            m_impl->vertexCapacity * sizeof(UIVertex));
    }
    if (!m_impl->vertexBuffer) return;
    if (void* mapped = m_impl->vertexBuffer->Map())
    {
        std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(UIVertex));
        m_impl->vertexBuffer->Unmap();
        m_impl->vertexBuffer->FlushMappedWrites();
    }
    else return;

    context->SetPipeline(m_impl->pipeline.get());
    context->SetVertexBuffer(0, m_impl->vertexBuffer.get(), sizeof(UIVertex));
    for (const DrawSegment& segment : segments)
    {
        context->SetTexture(0, segment.atlas->texture.get());
        context->DrawInstanced(segment.vertexCount, 1, segment.firstVertex, 0);
    }
}
