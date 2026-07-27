#include "Material.h"
#include "Materials/Texture.h"
#include "Core/Serialization/Json.h"
#include <filesystem>
#include <fstream>

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

JsonValue Material::Serialize() const
{
    JsonValue node = JsonValue::MakeObject();
    node.Set("type",     JsonValue(std::string("Material")));
    if (!m_filePath.empty())
    {
        node.Set("file", JsonValue(m_filePath));
        return node;
    }
    node.Set("diffuse",  J3(diffuseColor));
    node.Set("ambient",  J3(ambientColor));
    node.Set("specular", J3(specularColor));
    node.Set("shininess",JsonValue(shininess));
    return node;
}

void Material::Deserialize(const JsonValue& v)
{
    if (v.Has("file"))
    {
        LoadFromFile(v["file"].AsString());
        return;
    }
    diffuseColor  = from3(v["diffuse"],  diffuseColor);
    ambientColor  = from3(v["ambient"],  ambientColor);
    specularColor = from3(v["specular"], specularColor);
    if (v.Has("shininess")) shininess = v["shininess"].AsFloat();
}

bool Material::LoadFromFile(const std::string& path)
{
    try
    {
        const JsonValue root = JsonParseFile(path);
        diffuseColor = from3(root["baseColor"], diffuseColor);
        emissiveColor = from3(root["emissive"], emissiveColor);
        if (root.Has("metallic")) metallicFactor = root["metallic"].AsFloat();
        if (root.Has("roughness")) roughnessFactor = root["roughness"].AsFloat();
        if (root.Has("baseColorAlpha")) baseColorAlpha = root["baseColorAlpha"].AsFloat();
        if (root.Has("alphaMode")) alphaMode = root["alphaMode"].AsString();
        if (root.Has("alphaCutoff")) alphaCutoff = root["alphaCutoff"].AsFloat();
        if (root.Has("normalScale")) normalScale = root["normalScale"].AsFloat();
        if (root.Has("occlusionStrength")) occlusionStrength = root["occlusionStrength"].AsFloat();
        if (root.Has("doubleSided")) doubleSided = root["doubleSided"].AsBool();
        if (root.Has("unlit")) unlit = root["unlit"].AsBool();
        if (root.Has("baseColorTexture")) SetBaseColorTexture(
            ResolveTexturePath(path, root["baseColorTexture"].AsString()));
        if (root.Has("metallicRoughnessTexture")) SetMetallicRoughnessTexture(
            ResolveTexturePath(path, root["metallicRoughnessTexture"].AsString()));
        if (root.Has("normalTexture")) SetNormalTexture(
            ResolveTexturePath(path, root["normalTexture"].AsString()));
        if (root.Has("occlusionTexture")) SetOcclusionTexture(
            ResolveTexturePath(path, root["occlusionTexture"].AsString()));
        if (root.Has("emissiveTexture")) SetEmissiveTexture(
            ResolveTexturePath(path, root["emissiveTexture"].AsString()));
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
    root.Set("emissive", J3(emissiveColor));
    root.Set("metallic", JsonValue(metallicFactor));
    root.Set("roughness", JsonValue(roughnessFactor));
    root.Set("baseColorAlpha", JsonValue(baseColorAlpha));
    root.Set("alphaMode", JsonValue(alphaMode));
    root.Set("alphaCutoff", JsonValue(alphaCutoff));
    root.Set("normalScale", JsonValue(normalScale));
    root.Set("occlusionStrength", JsonValue(occlusionStrength));
    root.Set("doubleSided", JsonValue(doubleSided));
    root.Set("unlit", JsonValue(unlit));
    if (baseColorTexture) root.Set("baseColorTexture", JsonValue(TexturePath(baseColorTexture)));
    if (metallicRoughnessTexture) root.Set("metallicRoughnessTexture", JsonValue(TexturePath(metallicRoughnessTexture)));
    if (normalTexture) root.Set("normalTexture", JsonValue(TexturePath(normalTexture)));
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
    baseColorTexture = path.empty() ? nullptr : Texture::Acquire(path);
}

void Material::SetMetallicRoughnessTexture(const std::string& path)
{
    metallicRoughnessTexture = path.empty() ? nullptr : Texture::Acquire(path);
}

void Material::SetNormalTexture(const std::string& path)
{
    normalTexture = path.empty() ? nullptr : Texture::Acquire(path);
}

void Material::SetOcclusionTexture(const std::string& path)
{
    occlusionTexture = path.empty() ? nullptr : Texture::Acquire(path);
}

void Material::SetEmissiveTexture(const std::string& path)
{
    emissiveTexture = path.empty() ? nullptr : Texture::Acquire(path);
}

void Material::PrepareTextures(IGraphicsProvider* graphicsProvider)
{
    for (const auto& texture : {
        baseColorTexture, metallicRoughnessTexture, normalTexture,
        occlusionTexture, emissiveTexture })
        if (texture)
            texture->Prepare(graphicsProvider);
}
