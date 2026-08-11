#include "Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Object.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>

Sprite::Sprite()
{
    SetTypeName(COMPONENT_TYPE_NAME(Sprite));
    singlecomponent = true;
    RegisterField("animationManager", animationManager);
    RegisterField("sortingLayer", sortingLayer);
    RegisterField("pixelsPerUnit", pixelsPerUnit);
    RegisterField("tint", tint);
    RegisterField("alpha", alpha);
}

void Sprite::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    pixelsPerUnit = std::max(pixelsPerUnit, 0.01f);
    alpha = std::clamp(alpha, 0.f, 1.f);
    m_animationManager = nullptr;
}

void Sprite::OnAfterDeserialize(IGraphicsProvider* graphicsProvider)
{
    Prepare(graphicsProvider);
}

bool Sprite::DrawProperties(IEditorUi& ui)
{
    bool changed = false;
    SpriteAnimationManager* manager = ResolveAnimationManager();
    ui.ValueLabel("Animation Manager", manager ? manager->GetTypeName().c_str() : "(drop SpriteAnimationManager here)");
    if (ui.BeginDragDropTarget())
    {
        size_t size = 0;
        const void* data = ui.AcceptDragDropPayload("ENGINE_COMPONENT_REORDER", &size);
        if (data && size == sizeof(Component*))
        {
            Component* component = *static_cast<Component* const*>(data);
            auto* dropped = dynamic_cast<SpriteAnimationManager*>(component);
            if (dropped && dropped->Owner == Owner)
            {
                SetAnimationManager(dropped);
                changed = true;
            }
        }
        ui.EndDragDropTarget();
    }
    if (manager)
    {
        ui.SameLine();
        if (ui.Button("Clear"))
        {
            animationManager.clear();
            m_animationManager = nullptr;
            changed = true;
        }
    }
    changed = ui.SliderInt("Sorting Layer", &sortingLayer, -1000, 1000) || changed;
    changed = ui.DragFloat("Pixels Per Unit", &pixelsPerUnit, 0.5f, 0.01f, 10000.f) || changed;
    changed = ui.ColorEdit3("Tint", &tint.x) || changed;
    changed = ui.DragFloat("Alpha", &alpha, 0.01f, 0.f, 1.f) || changed;
    pixelsPerUnit = std::max(pixelsPerUnit, 0.01f);
    alpha = std::clamp(alpha, 0.f, 1.f);
    return changed;
}

void Sprite::SetAnimationManager(SpriteAnimationManager* manager)
{
    m_animationManager = manager && manager->Owner == Owner ? manager : nullptr;
    animationManager = m_animationManager ? m_animationManager->GetTypeName() : std::string{};
}

SpriteAnimationManager* Sprite::ResolveAnimationManager() const
{
    if (!Owner || animationManager.empty())
    {
        m_animationManager = nullptr;
        return nullptr;
    }
    // Resolve on demand so deleting or replacing the single manager component
    // cannot leave the renderer holding a stale pointer.
    m_animationManager = Owner->GetComponent<SpriteAnimationManager>();
    return m_animationManager;
}

bool Sprite::Prepare(IGraphicsProvider* graphicsProvider)
{
    SpriteAnimationManager* manager = ResolveAnimationManager();
    if (!graphicsProvider || !manager || !manager->Prepare(graphicsProvider))
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
            IGraphicsBuffer::Usage::VertexBuffer, IGraphicsBuffer::AccessMode::Upload,
            sizeof(vertices), vertices);
    }
    return IsReady();
}

bool Sprite::IsReady() const
{
    const SpriteAnimationManager* manager = ResolveAnimationManager();
    return m_vertexBuffer && manager && manager->IsReady();
}

glm::vec4 Sprite::GetUvRect() const
{
    const SpriteAnimationManager* manager = ResolveAnimationManager();
    const SpriteSheetFrame* selected = manager ? manager->GetCurrentFrame() : nullptr;
    const Texture* texture = manager ? manager->GetTexture() : nullptr;
    if (!selected || !texture || texture->GetWidth() == 0 || texture->GetHeight() == 0)
        return { 0.f, 0.f, 1.f, 1.f };
    const float width = selected->width > 0.f ? selected->width : static_cast<float>(texture->GetWidth());
    const float height = selected->height > 0.f ? selected->height : static_cast<float>(texture->GetHeight());
    return { selected->x / texture->GetWidth(), selected->y / texture->GetHeight(),
        width / texture->GetWidth(), height / texture->GetHeight() };
}

glm::vec2 Sprite::GetWorldSize() const
{
    const SpriteAnimationManager* manager = ResolveAnimationManager();
    const SpriteSheetFrame* selected = manager ? manager->GetCurrentFrame() : nullptr;
    const Texture* texture = manager ? manager->GetTexture() : nullptr;
    if (!selected || !texture)
        return { 1.f, 1.f };
    const float width = selected->width > 0.f ? selected->width : static_cast<float>(texture->GetWidth());
    const float height = selected->height > 0.f ? selected->height : static_cast<float>(texture->GetHeight());
    return { width / std::max(pixelsPerUnit, 0.01f), height / std::max(pixelsPerUnit, 0.01f) };
}

const Texture* Sprite::GetTexture() const
{
    const SpriteAnimationManager* manager = ResolveAnimationManager();
    return manager ? manager->GetTexture() : nullptr;
}
