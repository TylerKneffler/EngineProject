#pragma once

#include "Core/Script.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Rendering/Sprites/SpriteSheetAsset.h"
#include <chrono>
#include <memory>

class Texture;
class IGraphicsBuffer;

class Sprite : public Script
{
public:
    Sprite();

    PROPERTY(Inspector, EditAnywhere, Category = "Sprite")
    std::string spriteSheetFile;
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite")
    std::string animation;
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite")
    int frame = 0;
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite")
    bool playing = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite")
    int sortingLayer = 0;
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite", ClampMin = "0.01")
    float pixelsPerUnit = 100.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite | Color")
    glm::vec3 tint { 1.f, 1.f, 1.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite | Color")
    float alpha = 1.f;

    bool LoadFromFile(const std::string& path);
    void Start() override;
    void Update() override;
    void Deserialize(const JsonValue& value) override;
    void OnAfterDeserialize(IGraphicsProvider* graphicsProvider) override;
    bool Prepare(IGraphicsProvider* graphicsProvider);
    bool IsReady() const;
    IGraphicsBuffer* GetGraphicsBuffer() const { return m_vertexBuffer.get(); }
    uint32_t GetVertexCount() const { return 6; }
    uint32_t GetVertexStride() const { return sizeof(Vertex); }
    glm::vec4 GetUvRect() const;
    glm::vec2 GetWorldSize() const;
    const Texture* GetTexture() const;

private:
    const SpriteSheetAnimation* CurrentAnimation() const;
    const SpriteSheetFrame* CurrentFrame() const;
    void SelectTexture();

    SpriteSheetAsset m_sheet;
    std::shared_ptr<Texture> m_texture;
    std::string m_loadedSheetPath;
    std::string m_loadedImagePath;
    std::unique_ptr<IGraphicsBuffer> m_vertexBuffer;
    std::chrono::steady_clock::time_point m_lastFrameTime{};
};
