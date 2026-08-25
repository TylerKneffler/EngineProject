#pragma once

#include "Core/Component.h"
#include "Core/Rendering/Sprites/SpriteAnimationAsset.h"
#include "Core/Rendering/Sprites/SpriteSheetAsset.h"
#include <chrono>
#include <memory>

namespace Engine::Components
{
class Texture;

class SpriteAnimationManager : public Engine::Core::Component
{
public:
    using SpriteSheetFrame = Engine::Model::SpriteSheetFrame;
    using SpriteSheetAnimation = Engine::Model::SpriteSheetAnimation;
    using SpriteAnimationAsset = Engine::Rendering::SpriteAnimationAsset;
    using SpriteSheetAsset = Engine::Rendering::SpriteSheetAsset;

    SpriteAnimationManager();

    PROPERTY(Inspector, EditAnywhere, Category = "Sprite Animation")
    std::string animationFile;
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite Animation")
    bool playing = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Sprite Animation")
    int frame = 0;

    bool LoadFromFile(const std::string& path);
    void Start() override;
    void Update() override;
    void Deserialize(const JsonValue& value) override;
    void OnAfterDeserialize(IGraphicsProvider* graphicsProvider) override;
    bool DrawProperties(::Engine::Editor::IEditorUi& ui) override;
    bool Prepare(IGraphicsProvider* graphicsProvider);
    bool IsReady() const;
    const SpriteSheetFrame* GetCurrentFrame() const;
    const Texture* GetTexture() const;

private:
    const SpriteSheetAnimation* CurrentAnimation() const;
    void SelectTexture();
    bool SaveEditedAsset();

    SpriteAnimationAsset m_asset;
    SpriteSheetAsset m_sheet;
    std::shared_ptr<Texture> m_texture;
    std::string m_loadedAnimationPath;
    std::string m_loadedImagePath;
    std::chrono::steady_clock::time_point m_lastFrameTime{};
};
}
