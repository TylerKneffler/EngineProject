#include "Core/Compoonents/UI/UIImage.h"

#include "Core/Compoonents/Materials/Texture.h"
#include <algorithm>
#include <filesystem>

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

namespace Engine::Components
{
namespace
{
std::string ResolveImagePath(const std::string& requested)
{
    if (requested.empty()) return {};
    const std::filesystem::path path(requested);
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error))
        return path.lexically_normal().generic_string();

    const std::filesystem::path relative =
        path.lexically_relative(std::filesystem::path("Assets"));
    if (!relative.empty() && *relative.begin() != "..")
    {
        const std::filesystem::path bundled =
            std::filesystem::path(ENGINE_ASSETS_PATH) / relative;
        if (std::filesystem::is_regular_file(bundled, error))
            return bundled.lexically_normal().generic_string();
    }

    const std::filesystem::path engine =
        std::filesystem::path(ENGINE_ASSETS_PATH) / path;
    if (std::filesystem::is_regular_file(engine, error))
        return engine.lexically_normal().generic_string();
    return path.lexically_normal().generic_string();
}
}

UIImage::UIImage()
{
    SetTypeName(COMPONENT_TYPE_NAME(UIImage));
    singlecomponent = true;
    RegisterField("sourcePath", sourcePath);
    RegisterField("fitMode", fitMode);
    RegisterField("color", color);
    RegisterField("alpha", alpha);
    RegisterField("fillAmount", fillAmount);
    RegisterField("fillDirection", fillDirection);
    RegisterField("flipX", flipX);
    RegisterField("flipY", flipY);
}

void UIImage::Deserialize(const JsonValue& value)
{
    Component::Deserialize(value);
    alpha = std::clamp(alpha, 0.f, 1.f);
    fillAmount = std::clamp(fillAmount, 0.f, 1.f);
    m_texture.reset();
    m_loadedPath.clear();
}

void UIImage::OnAfterDeserialize(IGraphicsProvider* graphicsProvider)
{
    Prepare(graphicsProvider);
}

bool UIImage::Prepare(IGraphicsProvider* graphicsProvider)
{
    if (!graphicsProvider || sourcePath.empty()) return false;
    const std::string resolved = ResolveImagePath(sourcePath);
    if (!m_texture || m_loadedPath != resolved)
    {
        m_loadedPath = resolved;
        m_texture = Texture::Acquire(resolved, true);
    }
    return m_texture && m_texture->Prepare(graphicsProvider);
}
}
