#include "Sprite.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include <algorithm>

Sprite::Sprite()
{
    SetTypeName(COMPONENT_TYPE_NAME(Sprite));
    singlecomponent = true;
    RegisterField("spriteSheetFile", spriteSheetFile);
    RegisterField("animation", animation);
    RegisterField("frame", frame);
    RegisterField("playing", playing);
    RegisterField("sortingLayer", sortingLayer);
    RegisterField("pixelsPerUnit", pixelsPerUnit);
    RegisterField("tint", tint);
    RegisterField("alpha", alpha);
}

bool Sprite::LoadFromFile(const std::string& path)
{
    spriteSheetFile = path;
    m_loadedSheetPath.clear();
    return m_sheet.Load(path);
}

void Sprite::Start()
{
    m_lastFrameTime = std::chrono::steady_clock::now();
}

void Sprite::Update()
{
    const SpriteSheetAnimation* current = CurrentAnimation();
    if (!playing || !current || current->frames.size() < 2)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (m_lastFrameTime.time_since_epoch().count() == 0)
        m_lastFrameTime = now;
    const SpriteSheetFrame* currentFrame = CurrentFrame();
    const float duration = currentFrame ? currentFrame->duration : 0.1f;
    if (std::chrono::duration<float>(now - m_lastFrameTime).count() < duration)
        return;
    m_lastFrameTime = now;
    ++frame;
    if (frame >= static_cast<int>(current->frames.size()))
    {
        frame = current->loop ? 0 : static_cast<int>(current->frames.size()) - 1;
        if (!current->loop)
            playing = false;
    }
    SelectTexture();
}

void Sprite::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    m_loadedSheetPath.clear();
    frame = std::max(frame, 0);
    pixelsPerUnit = std::max(pixelsPerUnit, 0.01f);
    alpha = std::clamp(alpha, 0.f, 1.f);
}

void Sprite::OnAfterDeserialize(IGraphicsProvider* graphicsProvider)
{
    Prepare(graphicsProvider);
}

bool Sprite::Prepare(IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider || spriteSheetFile.empty())
        return false;
    if (m_loadedSheetPath != spriteSheetFile)
    {
        if (!m_sheet.Load(spriteSheetFile))
            return false;
        m_loadedSheetPath = spriteSheetFile;
        frame = std::max(frame, 0);
        SelectTexture();
    }
    else
        SelectTexture();
    if (!m_texture || !m_texture->Prepare(graphicsProvider))
        return false;
    if (!m_vertexBuffer)
    {
        const Vertex vertices[6] = {
            {{-.5f,-.5f,0.f},{0.f,0.f,-1.f},{0.f,1.f},{1.f,0.f,0.f,1.f}},
            {{ .5f, .5f,0.f},{0.f,0.f,-1.f},{1.f,0.f},{1.f,0.f,0.f,1.f}},
            {{-.5f, .5f,0.f},{0.f,0.f,-1.f},{0.f,0.f},{1.f,0.f,0.f,1.f}},
            {{-.5f,-.5f,0.f},{0.f,0.f,-1.f},{0.f,1.f},{1.f,0.f,0.f,1.f}},
            {{ .5f,-.5f,0.f},{0.f,0.f,-1.f},{1.f,1.f},{1.f,0.f,0.f,1.f}},
            {{ .5f, .5f,0.f},{0.f,0.f,-1.f},{1.f,0.f},{1.f,0.f,0.f,1.f}}
        };
        m_vertexBuffer = graphicsProvider->GetBufferFactory()->CreateBuffer(
            IGraphicsBuffer::Usage::VertexBuffer,
            IGraphicsBuffer::AccessMode::Upload,
            sizeof(vertices), vertices);
    }
    return IsReady();
}

const SpriteSheetAnimation* Sprite::CurrentAnimation() const
{
    const SpriteSheetAnimation* selected = m_sheet.FindAnimation(animation);
    return selected ? selected : m_sheet.GetDefaultAnimation();
}

const SpriteSheetFrame* Sprite::CurrentFrame() const
{
    const SpriteSheetAnimation* selected = CurrentAnimation();
    if (!selected || selected->frames.empty())
        return nullptr;
    const int index = std::clamp(frame, 0, static_cast<int>(selected->frames.size()) - 1);
    return &selected->frames[static_cast<size_t>(index)];
}

void Sprite::SelectTexture()
{
    const SpriteSheetFrame* selected = CurrentFrame();
    const std::string image = selected ? selected->image : std::string{};
    if (image == m_loadedImagePath)
        return;
    m_loadedImagePath = image;
    m_texture = image.empty() ? nullptr : Texture::Acquire(image, true);
}

bool Sprite::IsReady() const
{
    return m_vertexBuffer && m_texture && m_texture->GetGraphicsTexture();
}

glm::vec4 Sprite::GetUvRect() const
{
    const SpriteSheetFrame* selected = CurrentFrame();
    if (!selected || !m_texture || m_texture->GetWidth() == 0 || m_texture->GetHeight() == 0)
        return { 0.f, 0.f, 1.f, 1.f };
    const float width = selected->width > 0.f
        ? selected->width : static_cast<float>(m_texture->GetWidth());
    const float height = selected->height > 0.f
        ? selected->height : static_cast<float>(m_texture->GetHeight());
    return { selected->x / m_texture->GetWidth(), selected->y / m_texture->GetHeight(),
        width / m_texture->GetWidth(), height / m_texture->GetHeight() };
}

glm::vec2 Sprite::GetWorldSize() const
{
    const SpriteSheetFrame* selected = CurrentFrame();
    if (!selected || !m_texture)
        return { 1.f, 1.f };
    const float width = selected->width > 0.f
        ? selected->width : static_cast<float>(m_texture->GetWidth());
    const float height = selected->height > 0.f
        ? selected->height : static_cast<float>(m_texture->GetHeight());
    return { width / std::max(pixelsPerUnit, 0.01f),
        height / std::max(pixelsPerUnit, 0.01f) };
}

const Texture* Sprite::GetTexture() const
{
    return m_texture.get();
}
