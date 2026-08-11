#include "SpriteAnimationManager.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cstring>

SpriteAnimationManager::SpriteAnimationManager()
{
    SetTypeName(COMPONENT_TYPE_NAME(SpriteAnimationManager));
    singlecomponent = true;
    RegisterField("animationFile", animationFile);
    RegisterField("playing", playing);
    RegisterField("frame", frame);
}

bool SpriteAnimationManager::LoadFromFile(const std::string& path)
{
    SpriteAnimationAsset loaded;
    if (!loaded.Load(path))
        return false;
    animationFile = path;
    m_asset = std::move(loaded);
    m_loadedAnimationPath = path;
    frame = std::max(frame, 0);
    playing = m_asset.autoplay;
    m_loadedImagePath.clear();
    m_texture.reset();
    if (!m_sheet.Load(m_asset.ResolveSpriteSheet()))
        return false;
    SelectTexture();
    return true;
}

void SpriteAnimationManager::Start()
{
    m_lastFrameTime = std::chrono::steady_clock::now();
}

void SpriteAnimationManager::Update()
{
    const SpriteSheetAnimation* current = CurrentAnimation();
    if (!playing || m_asset.speed <= 0.f || !current || current->frames.size() < 2)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (m_lastFrameTime.time_since_epoch().count() == 0)
        m_lastFrameTime = now;
    const SpriteSheetFrame* currentFrame = GetCurrentFrame();
    const float duration = (currentFrame ? currentFrame->duration : 0.1f) / m_asset.speed;
    if (std::chrono::duration<float>(now - m_lastFrameTime).count() < duration)
        return;
    m_lastFrameTime = now;
    ++frame;
    if (frame >= static_cast<int>(current->frames.size()))
    {
        frame = m_asset.loop ? 0 : static_cast<int>(current->frames.size()) - 1;
        if (!m_asset.loop)
            playing = false;
    }
    SelectTexture();
}

void SpriteAnimationManager::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    frame = std::max(frame, 0);
    m_loadedAnimationPath.clear();
}

void SpriteAnimationManager::OnAfterDeserialize(IGraphicsProvider* graphicsProvider)
{
    Prepare(graphicsProvider);
}

bool SpriteAnimationManager::DrawProperties(IEditorUi& ui)
{
    const std::string previousFile = animationFile;
    bool changed = Component::DrawProperties(ui);
    if (animationFile != previousFile)
        LoadFromFile(animationFile);
    if (m_loadedAnimationPath != animationFile && !animationFile.empty())
        LoadFromFile(animationFile);
    if (m_loadedAnimationPath.empty())
        return changed;

    ui.Separator();
    ui.Label("Sprite Animation Asset");
    char sheet[512]{};
    strncpy_s(sheet, sizeof(sheet), m_asset.spriteSheetFile.c_str(), _TRUNCATE);
    if (ui.InputText("Sprite Sheet", sheet, sizeof(sheet)))
    {
        m_asset.spriteSheetFile = sheet;
        SaveEditedAsset();
        changed = true;
    }
    char animationName[256]{};
    strncpy_s(animationName, sizeof(animationName), m_asset.animation.c_str(), _TRUNCATE);
    if (ui.InputText("Animation", animationName, sizeof(animationName)))
    {
        m_asset.animation = animationName;
        SaveEditedAsset();
        changed = true;
    }
    float speed = m_asset.speed;
    if (ui.DragFloat("Speed", &speed, 0.05f, 0.f, 20.f))
    {
        m_asset.speed = std::max(speed, 0.f);
        SaveEditedAsset();
        changed = true;
    }
    bool loop = m_asset.loop;
    if (ui.Checkbox("Loop", &loop))
    {
        m_asset.loop = loop;
        SaveEditedAsset();
        changed = true;
    }
    bool autoplay = m_asset.autoplay;
    if (ui.Checkbox("Autoplay", &autoplay))
    {
        m_asset.autoplay = autoplay;
        SaveEditedAsset();
        changed = true;
    }
    return changed;
}

bool SpriteAnimationManager::SaveEditedAsset()
{
    if (!m_asset.Save())
        return false;
    m_sheet.Load(m_asset.ResolveSpriteSheet());
    frame = 0;
    m_loadedImagePath.clear();
    SelectTexture();
    return true;
}

bool SpriteAnimationManager::Prepare(IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider || animationFile.empty())
        return false;
    if (m_loadedAnimationPath != animationFile)
    {
        // Scene/prefab state is authoritative after deserialization. Loading
        // the asset initializes autoplay only for a newly assigned clip.
        const bool serializedPlaying = playing;
        if (!LoadFromFile(animationFile))
            return false;
        playing = serializedPlaying;
    }
    SelectTexture();
    return m_texture && m_texture->Prepare(graphicsProvider);
}

const SpriteSheetAnimation* SpriteAnimationManager::CurrentAnimation() const
{
    const SpriteSheetAnimation* selected = m_sheet.FindAnimation(m_asset.animation);
    return selected ? selected : m_sheet.GetDefaultAnimation();
}

const SpriteSheetFrame* SpriteAnimationManager::GetCurrentFrame() const
{
    const SpriteSheetAnimation* selected = CurrentAnimation();
    if (!selected || selected->frames.empty())
        return nullptr;
    const int index = std::clamp(frame, 0, static_cast<int>(selected->frames.size()) - 1);
    return &selected->frames[static_cast<size_t>(index)];
}

void SpriteAnimationManager::SelectTexture()
{
    const SpriteSheetFrame* selected = GetCurrentFrame();
    const std::string image = selected ? selected->image : std::string{};
    if (image == m_loadedImagePath)
        return;
    m_loadedImagePath = image;
    m_texture = image.empty() ? nullptr : Texture::Acquire(image, true);
}

bool SpriteAnimationManager::IsReady() const
{
    return m_texture && m_texture->GetGraphicsTexture();
}

const Texture* SpriteAnimationManager::GetTexture() const
{
    return m_texture.get();
}
