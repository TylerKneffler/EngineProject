#include "Material.h"
#include "Materials/Texture.h"
#include "Core/Serialization/Json.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

Material::Material()
{
    SetTypeName(COMPONENT_TYPE_NAME(Material));
    RegisterField("diffuse", diffuseColor);
    RegisterField("ambient", ambientColor);
    RegisterField("specular", specularColor);
    RegisterField("shininess", shininess);
    RegisterField("metallicFactor", metallicFactor);
    RegisterField("roughnessFactor", roughnessFactor);
    RegisterField("baseColorAlpha", baseColorAlpha);
    RegisterField("alphaCutoff", alphaCutoff);
    RegisterField("normalScale", normalScale);
    RegisterField("heightScale", heightScale);
    RegisterField("heightMinSteps", heightMinSteps);
    RegisterField("heightMaxSteps", heightMaxSteps);
    RegisterField("occlusionStrength", occlusionStrength);
    RegisterField("doubleSided", doubleSided);
    RegisterField("unlit", unlit);
    RegisterField("alphaMode", alphaMode);
    RegisterField("emissiveColor", emissiveColor);
    RegisterField("baseColorUvSet", baseColorUvSet);
    RegisterField("metallicRoughnessUvSet", metallicRoughnessUvSet);
    RegisterField("normalUvSet", normalUvSet);
    RegisterField("occlusionUvSet", occlusionUvSet);
    RegisterField("emissiveUvSet", emissiveUvSet);
    RegisterField("heightUvSet", heightUvSet);
}

namespace
{
    JsonValue J3(const glm::vec3& v)
    {
        return JsonValue::MakeArray().Push(JsonValue(v.x)).Push(JsonValue(v.y)).Push(JsonValue(v.z));
    }
    glm::vec3 from3(const JsonValue& v, glm::vec3 def = {})
    {
        if (!v.IsArray() || v.ArraySize() < 3) return def;
        return { v.ArrayAt(0).AsFloat(), v.ArrayAt(1).AsFloat(), v.ArrayAt(2).AsFloat() };
    }

    std::string ResolveTexturePath(
        const std::string& materialPath,
        const std::string& texturePath)
    {
        if (texturePath.empty())
            return {};
        const std::filesystem::path requested(texturePath);
        if (requested.is_absolute() || std::filesystem::exists(requested))
            return requested.lexically_normal().generic_string();
        const std::filesystem::path besideMaterial =
            std::filesystem::path(materialPath).parent_path() / requested;
        if (std::filesystem::exists(besideMaterial))
            return besideMaterial.lexically_normal().generic_string();
        return requested.lexically_normal().generic_string();
    }
}

bool Material::LoadFromFile(const std::string& path)
{
    try
    {
        const JsonValue root = JsonParseFile(path);
        baseColorTexture.reset();
        metallicRoughnessTexture.reset();
        normalTexture.reset();
        heightTexture.reset();
        occlusionTexture.reset();
        emissiveTexture.reset();
        diffuseColor = from3(root["baseColor"], diffuseColor);
        ambientColor = from3(root["ambientColor"], ambientColor);
        specularColor = from3(root["specularColor"], specularColor);
        emissiveColor = from3(root["emissive"], emissiveColor);
        if (root.Has("shininess")) shininess = root["shininess"].AsFloat();
        if (root.Has("metallic")) metallicFactor = root["metallic"].AsFloat();
        if (root.Has("roughness")) roughnessFactor = root["roughness"].AsFloat();
        if (root.Has("baseColorAlpha")) baseColorAlpha = root["baseColorAlpha"].AsFloat();
        if (root.Has("alphaMode")) alphaMode = root["alphaMode"].AsString();
        if (root.Has("alphaCutoff")) alphaCutoff = root["alphaCutoff"].AsFloat();
        if (root.Has("normalScale")) normalScale = root["normalScale"].AsFloat();
        if (root.Has("heightScale")) heightScale = root["heightScale"].AsFloat();
        if (root.Has("heightMinSteps")) heightMinSteps = root["heightMinSteps"].AsFloat();
        if (root.Has("heightMaxSteps")) heightMaxSteps = root["heightMaxSteps"].AsFloat();
        if (root.Has("occlusionStrength")) occlusionStrength = root["occlusionStrength"].AsFloat();
        if (root.Has("doubleSided")) doubleSided = root["doubleSided"].AsBool();
        if (root.Has("unlit")) unlit = root["unlit"].AsBool();
        if (root.Has("baseColorUvSet")) baseColorUvSet = root["baseColorUvSet"].AsInt();
        if (root.Has("metallicRoughnessUvSet")) metallicRoughnessUvSet = root["metallicRoughnessUvSet"].AsInt();
        if (root.Has("normalUvSet")) normalUvSet = root["normalUvSet"].AsInt();
        if (root.Has("occlusionUvSet")) occlusionUvSet = root["occlusionUvSet"].AsInt();
        if (root.Has("emissiveUvSet")) emissiveUvSet = root["emissiveUvSet"].AsInt();
        if (root.Has("heightUvSet")) heightUvSet = root["heightUvSet"].AsInt();
        if (root.Has("baseColorTexture")) SetBaseColorTexture(
            ResolveTexturePath(path, root["baseColorTexture"].AsString()));
        if (root.Has("metallicRoughnessTexture")) SetMetallicRoughnessTexture(
            ResolveTexturePath(path, root["metallicRoughnessTexture"].AsString()));
        if (root.Has("normalTexture")) SetNormalTexture(
            ResolveTexturePath(path, root["normalTexture"].AsString()));
        if (root.Has("heightTexture")) SetHeightTexture(
            ResolveTexturePath(path, root["heightTexture"].AsString()));
        if (root.Has("occlusionTexture")) SetOcclusionTexture(
            ResolveTexturePath(path, root["occlusionTexture"].AsString()));
        if (root.Has("emissiveTexture")) SetEmissiveTexture(
            ResolveTexturePath(path, root["emissiveTexture"].AsString()));
        Validate();
        m_filePath = path;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool Material::SaveToFile(const std::string& path) const
{
    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue(1));
    root.Set("baseColor", J3(diffuseColor));
    root.Set("ambientColor", J3(ambientColor));
    root.Set("specularColor", J3(specularColor));
    root.Set("emissive", J3(emissiveColor));
    root.Set("shininess", JsonValue(shininess));
    root.Set("metallic", JsonValue(metallicFactor));
    root.Set("roughness", JsonValue(roughnessFactor));
    root.Set("baseColorAlpha", JsonValue(baseColorAlpha));
    root.Set("alphaMode", JsonValue(alphaMode));
    root.Set("alphaCutoff", JsonValue(alphaCutoff));
    root.Set("normalScale", JsonValue(normalScale));
    root.Set("heightScale", JsonValue(heightScale));
    root.Set("heightMinSteps", JsonValue(heightMinSteps));
    root.Set("heightMaxSteps", JsonValue(heightMaxSteps));
    root.Set("occlusionStrength", JsonValue(occlusionStrength));
    root.Set("doubleSided", JsonValue(doubleSided));
    root.Set("unlit", JsonValue(unlit));
    root.Set("baseColorUvSet", JsonValue(baseColorUvSet));
    root.Set("metallicRoughnessUvSet", JsonValue(metallicRoughnessUvSet));
    root.Set("normalUvSet", JsonValue(normalUvSet));
    root.Set("occlusionUvSet", JsonValue(occlusionUvSet));
    root.Set("emissiveUvSet", JsonValue(emissiveUvSet));
    root.Set("heightUvSet", JsonValue(heightUvSet));
    if (baseColorTexture) root.Set("baseColorTexture", JsonValue(TexturePath(baseColorTexture)));
    if (metallicRoughnessTexture) root.Set("metallicRoughnessTexture", JsonValue(TexturePath(metallicRoughnessTexture)));
    if (normalTexture) root.Set("normalTexture", JsonValue(TexturePath(normalTexture)));
    if (heightTexture) root.Set("heightTexture", JsonValue(TexturePath(heightTexture)));
    if (occlusionTexture) root.Set("occlusionTexture", JsonValue(TexturePath(occlusionTexture)));
    if (emissiveTexture) root.Set("emissiveTexture", JsonValue(TexturePath(emissiveTexture)));
    std::ofstream file(path);
    if (!file)
        return false;
    file << JsonWrite(root);
    return file.good();
}

std::string Material::TexturePath(const std::shared_ptr<Texture>& texture)
{
    return texture ? texture->GetFilePath() : std::string{};
}

void Material::SetBaseColorTexture(const std::string& path)
{
    baseColorTexture = path.empty() ? nullptr : Texture::Acquire(path, true);
}

void Material::SetMetallicRoughnessTexture(const std::string& path)
{
    metallicRoughnessTexture = path.empty() ? nullptr : Texture::Acquire(path, false);
}

void Material::SetNormalTexture(const std::string& path)
{
    normalTexture = path.empty() ? nullptr : Texture::Acquire(path, false);
}

void Material::SetHeightTexture(const std::string& path)
{
    heightTexture = path.empty() ? nullptr : Texture::Acquire(path, false);
}

void Material::SetOcclusionTexture(const std::string& path)
{
    occlusionTexture = path.empty() ? nullptr : Texture::Acquire(path, false);
}

void Material::SetEmissiveTexture(const std::string& path)
{
    emissiveTexture = path.empty() ? nullptr : Texture::Acquire(path, true);
}

void Material::PrepareTextures(IGraphicsProvider* graphicsProvider)
{
    for (const auto& texture : {
        baseColorTexture, metallicRoughnessTexture, normalTexture,
        heightTexture, occlusionTexture, emissiveTexture })
        if (texture)
            texture->Prepare(graphicsProvider);
}

MaterialAlphaMode Material::GetAlphaMode() const
{
    std::string mode = alphaMode;
    std::transform(mode.begin(), mode.end(), mode.begin(),
        [](unsigned char value)
        {
            return static_cast<char>(std::tolower(value));
        });
    if (mode == "mask")
        return MaterialAlphaMode::Mask;
    if (mode == "blend")
        return MaterialAlphaMode::Blend;
    return MaterialAlphaMode::Opaque;
}

void Material::Validate()
{
    metallicFactor = std::clamp(metallicFactor, 0.f, 1.f);
    roughnessFactor = std::clamp(roughnessFactor, 0.045f, 1.f);
    baseColorAlpha = std::clamp(baseColorAlpha, 0.f, 1.f);
    alphaCutoff = std::clamp(alphaCutoff, 0.f, 1.f);
    normalScale = std::max(normalScale, 0.f);
    heightScale = std::clamp(heightScale, 0.f, 0.2f);
    heightMinSteps = std::clamp(heightMinSteps, 4.f, 64.f);
    heightMaxSteps = std::clamp(heightMaxSteps, heightMinSteps, 64.f);
    occlusionStrength = std::clamp(occlusionStrength, 0.f, 1.f);
    baseColorUvSet = std::clamp(baseColorUvSet, 0, 1);
    metallicRoughnessUvSet = std::clamp(metallicRoughnessUvSet, 0, 1);
    normalUvSet = std::clamp(normalUvSet, 0, 1);
    occlusionUvSet = std::clamp(occlusionUvSet, 0, 1);
    emissiveUvSet = std::clamp(emissiveUvSet, 0, 1);
    heightUvSet = std::clamp(heightUvSet, 0, 1);
    switch (GetAlphaMode())
    {
    case MaterialAlphaMode::Mask: alphaMode = "Mask"; break;
    case MaterialAlphaMode::Blend: alphaMode = "Blend"; break;
    default: alphaMode = "Opaque"; break;
    }
}

JsonValue Material::Serialize() const
{
    JsonValue value = Component::Serialize();
    value.Set("materialAsset", JsonValue(m_filePath));
    value.Set("baseColorTexture", JsonValue(TexturePath(baseColorTexture)));
    value.Set("metallicRoughnessTexture", JsonValue(TexturePath(metallicRoughnessTexture)));
    value.Set("normalTexture", JsonValue(TexturePath(normalTexture)));
    value.Set("heightTexture", JsonValue(TexturePath(heightTexture)));
    value.Set("occlusionTexture", JsonValue(TexturePath(occlusionTexture)));
    value.Set("emissiveTexture", JsonValue(TexturePath(emissiveTexture)));
    return value;
}

void Material::Deserialize(const JsonValue& value)
{
    if (value.Has("materialAsset") && !value["materialAsset"].AsString().empty())
        LoadFromFile(value["materialAsset"].AsString());
    Component::Deserialize(value);
    if (value.Has("baseColorTexture"))
        SetBaseColorTexture(value["baseColorTexture"].AsString());
    if (value.Has("metallicRoughnessTexture"))
        SetMetallicRoughnessTexture(value["metallicRoughnessTexture"].AsString());
    if (value.Has("normalTexture"))
        SetNormalTexture(value["normalTexture"].AsString());
    if (value.Has("heightTexture"))
        SetHeightTexture(value["heightTexture"].AsString());
    if (value.Has("occlusionTexture"))
        SetOcclusionTexture(value["occlusionTexture"].AsString());
    if (value.Has("emissiveTexture"))
        SetEmissiveTexture(value["emissiveTexture"].AsString());
    Validate();
}
