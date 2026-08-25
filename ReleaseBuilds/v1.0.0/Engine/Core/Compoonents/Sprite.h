#pragma once

#include "Core/Component.h"
#include "Core/Compoonents/Mesh.h"
#include <memory>

namespace Engine::Components
{
class Texture;
class SpriteAnimationManager;

// Sprite is rendering-only. Playback and clip selection belong to the
// explicitly 2D SpriteAnimationManager component.
class Sprite : public Engine::Core::Component
{
public:
    using Vertex = Engine::Model::Vertex;
    using IGraphicsBuffer = Engine::Graphics::IGraphicsBuffer;

    struct RenderData
    {
        IGraphicsBuffer* vertexBuffer = nullptr;
        const Texture* texture = nullptr;
        glm::vec4 uvRect{ 0.f, 0.f, 1.f, 1.f };
        glm::vec2 worldSize{ 1.f, 1.f };
    };

    Sprite();

    PROPERTY(Inspector, EditAnywhere, Category = "Sprite")
    std::string animationManager = "SpriteAnimationManager";
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite")
    ComponentReference animationManagerReference { "SpriteAnimationManager" };
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite")
    int sortingLayer = 0;
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite", ClampMin = "0.01")
    float pixelsPerUnit = 100.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite | Color")
    glm::vec3 tint { 1.f, 1.f, 1.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite | Color")
    float alpha = 1.f;

    void Deserialize(const JsonValue& value) override;
    void OnAfterDeserialize(IGraphicsProvider* graphicsProvider) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    void SetAnimationManager(SpriteAnimationManager* manager);
    // Resolves the animation-manager component once and captures everything
    // the renderer needs for this snapshot.
    bool PrepareRenderData(IGraphicsProvider* graphicsProvider, RenderData& data);
    bool Prepare(IGraphicsProvider* graphicsProvider);
    bool IsReady() const;
    IGraphicsBuffer* GetGraphicsBuffer() const { return m_vertexBuffer.get(); }
    uint32_t GetVertexCount() const { return 6; }
    uint32_t GetVertexStride() const { return sizeof(Vertex); }
    glm::vec4 GetUvRect() const;
    glm::vec2 GetWorldSize() const;
    const Texture* GetTexture() const;

private:
    SpriteAnimationManager* ResolveAnimationManager() const;

    mutable SpriteAnimationManager* m_animationManager = nullptr;
    std::unique_ptr<IGraphicsBuffer> m_vertexBuffer;
};
}
