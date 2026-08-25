#pragma once

#include "Core/Component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>

namespace Engine::Components
{
class Texture;

// Draws a texture inside the rect supplied by UIObject.
class UIImage final : public Engine::Core::Component
{
public:
    UIImage();

    PROPERTY(Inspector, EditAnywhere, Category = "UI | Image")
    std::string sourcePath;
    // Fill stretches to the rect, Contain letterboxes, Cover crops, Native
    // uses the texture's pixel dimensions.
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Image")
    std::string fitMode = "Fill";
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Image")
    glm::vec3 color { 1.f };
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Image", Range = "0, 1")
    float alpha = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Image", Range = "0, 1")
    float fillAmount = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Image")
    std::string fillDirection = "LeftToRight";
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Image")
    bool flipX = false;
    PROPERTY(Inspector, EditAnywhere, Category = "UI | Image")
    bool flipY = false;

    void Deserialize(const JsonValue& value) override;
    void OnAfterDeserialize(IGraphicsProvider* graphicsProvider) override;
    bool Prepare(IGraphicsProvider* graphicsProvider);
    const Texture* GetTexture() const { return m_texture.get(); }

private:
    std::shared_ptr<Texture> m_texture;
    std::string m_loadedPath;
};
}
