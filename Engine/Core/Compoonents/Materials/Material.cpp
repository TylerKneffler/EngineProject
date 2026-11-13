#include "Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Serialization/Json.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace Engine::Components
{
Material::Material()
{
    SetTypeName(COMPONENT_TYPE_NAME(Material));
    RegisterEditorFieldGroup("shader", "Shader");
    RegisterField("diffuse", diffuseColor, "Colors");
    RegisterField("useVertexColors", useVertexColors, "Colors");
    RegisterField("ambient", ambientColor, "Colors");
    RegisterField("specular", specularColor, "Colors");
    RegisterField("emissiveColor", emissiveColor, "Colors");
    RegisterField("shininess", shininess, "Surface");
    RegisterField("specularModel", specularModel, "Surface");
    RegisterField("baseColorAlpha", baseColorAlpha, "Surface");
    RegisterField("baseColorTextureTint", baseColorTextureTint, "Colors");
    RegisterField("baseColorTextureTintAlpha", baseColorTextureTintAlpha, "Colors");
    RegisterField("edgeWidth", edgeWidth, "MMD");
    RegisterField("edgeEnabled", edgeEnabled, "MMD");
    RegisterField("sphereMode", sphereMode, "MMD");
    RegisterField("builtinToon", builtinToon, "MMD");
    RegisterField("alphaMode", alphaMode, "Surface");
    RegisterField("alphaCutoff", alphaCutoff, "Surface");
    RegisterField("alphaCoverageTransmission", alphaCoverageTransmission, "Alpha");
    RegisterField("doubleSided", doubleSided, "Surface");
    RegisterField("overrideTextureLod", overrideTextureLod, "Texture LOD");
    RegisterField("textureLodEnabled", textureLod.enabled, "Texture LOD");
    RegisterField("textureLodNearDistance", textureLod.nearDistance, "Texture LOD");
    RegisterField("textureLodFarDistance", textureLod.farDistance, "Texture LOD");
    RegisterField("textureLodMipBias", textureLod.mipBias, "Texture LOD");
    RegisterField("textureLodSimplifyShading", textureLod.simplifyShading, "Texture LOD");
    RegisterField("textureLodDisableAlphaAtFar", textureLod.disableAlphaAtFar, "Texture LOD");
    RegisterField("textureLodAlphaMipBias", textureLod.alphaMipBias, "Texture LOD");
    RegisterField("castsShadows", castsShadows, "Shadows");
    RegisterField("unlit", unlit, "Surface");
    RegisterField("lightingChannels", lightingChannels, "Lighting");
    RegisterField("metallicFactor", metallicFactor, "PBR");
    RegisterField("roughnessFactor", roughnessFactor, "PBR");
    RegisterField("subsurfaceEnabled", subsurfaceEnabled, "Subsurface");
    RegisterField("subsurfaceWeight", subsurfaceWeight, "Subsurface");
    RegisterField("subsurfaceRadius", subsurfaceRadius, "Subsurface");
    RegisterField("subsurfaceColor", subsurfaceColor, "Subsurface");
    RegisterField("subsurfaceThickness", subsurfaceThickness, "Subsurface");
    RegisterField("subsurfaceTransmission", subsurfaceTransmission, "Subsurface");
    RegisterField("environmentDiffuseStrength", environmentDiffuseStrength, "PBR");
    RegisterField("occlusionStrength", occlusionStrength, "PBR");
    RegisterField("reflectionStrength", reflectionStrength, "Reflections");
    RegisterField("useCustomReflectionEnvironment", useCustomReflectionEnvironment, "Reflections");
    RegisterField("reflectionEnvironmentTexture", reflectionEnvironmentTexture, "Reflections");
    RegisterField("reflectionEnvironmentExposure", reflectionEnvironmentExposure, "Reflections");
    RegisterField("reflectionEnvironmentRotation", reflectionEnvironmentRotation, "Reflections");
    RegisterField("normalScale", normalScale, "Height and Normal", false);
    RegisterField("heightScale", heightScale, "Height and Normal", false);
    RegisterField("terrainHeightBlendStrength", terrainHeightBlendStrength,
        "Terrain", false);
    RegisterField("terrainMacroColor", terrainMacroColor, "Terrain", false);
    RegisterField("heightMinSteps", heightMinSteps, "Height and Normal", false);
    RegisterField("heightMaxSteps", heightMaxSteps, "Height and Normal", false);
    RegisterField("baseColorUvSet", baseColorUvSet, "Texture Coordinates", false);
    RegisterField("metallicRoughnessUvSet", metallicRoughnessUvSet, "Texture Coordinates", false);
    RegisterField("normalUvSet", normalUvSet, "Texture Coordinates", false);
    RegisterField("occlusionUvSet", occlusionUvSet, "Texture Coordinates", false);
    RegisterField("emissiveUvSet", emissiveUvSet, "Texture Coordinates", false);
    RegisterField("heightUvSet", heightUvSet, "Texture Coordinates", false);
    RegisterEditorFieldGroup("materialAsset", "Asset Link", false);
    RegisterEditorFieldGroup("baseColorTexture", "Textures");
    RegisterEditorFieldGroup("sphereTexture", "Textures");
    RegisterEditorFieldGroup("toonTexture", "Textures");
    RegisterEditorFieldGroup("metallicRoughnessTexture", "Textures");
    RegisterEditorFieldGroup("normalTexture", "Textures");
    RegisterEditorFieldGroup("heightTexture", "Textures");
    RegisterEditorFieldGroup("occlusionTexture", "Textures");
    RegisterEditorFieldGroup("emissiveTexture", "Textures");
    RegisterEditorFieldGroup("subsurfaceThicknessTexture", "Subsurface");
}

namespace
{
#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

    std::string ReadSurfaceShader(const Material::JsonValue& value)
    {
        if (value.Has("shader")) return value["shader"].AsString();
        // Older files stored ordered draw passes. Preserve the first enabled
        // surface; overlays are not additional material surfaces.
        const auto& list = value["shaders"];
        for (size_t i = 0; i < list.ArraySize(); ++i)
        {
            const auto& entry = list.ArrayAt(i);
            if (entry.IsString()) return entry.AsString();
            if (entry.IsObject() && (!entry.Has("enabled") || entry["enabled"].AsBool()))
                return entry["path"].AsString();
        }
        return {};
    }

    Engine::Serialization::JsonValue J3(const glm::vec3& v)
    {
        return Engine::Serialization::JsonValue::MakeArray()
            .Push(Engine::Serialization::JsonValue(v.x))
            .Push(Engine::Serialization::JsonValue(v.y))
            .Push(Engine::Serialization::JsonValue(v.z));
    }
    glm::vec3 from3(const Engine::Serialization::JsonValue& v, glm::vec3 def = {})
    {
        if (!v.IsArray() || v.ArraySize() < 3) return def;
        return { v.ArrayAt(0).AsFloat(), v.ArrayAt(1).AsFloat(), v.ArrayAt(2).AsFloat() };
    }
    Engine::Serialization::JsonValue J4(const glm::vec4& v)
    {
        return J3(glm::vec3(v)).Push(Engine::Serialization::JsonValue(v.w));
    }
    glm::vec4 from4(const Engine::Serialization::JsonValue& v, glm::vec4 def)
    {
        if (!v.IsArray() || v.ArraySize() < 4) return def;
        return { v.ArrayAt(0).AsFloat(), v.ArrayAt(1).AsFloat(),
            v.ArrayAt(2).AsFloat(), v.ArrayAt(3).AsFloat() };
    }

    std::string ResolveTexturePath(
        const std::string& materialPath,
        const std::string& texturePath)
    {
        if (texturePath.empty())
            return {};
        const std::filesystem::path requested(texturePath);
        const std::string resolved = Mesh::ResolveFilePath(texturePath);
        if (std::filesystem::exists(resolved))
            return resolved;
        if (requested.is_absolute())
            return requested.lexically_normal().generic_string();
        const std::filesystem::path projectAssets("Assets");
        const std::filesystem::path relativeAsset =
            requested.lexically_relative(projectAssets);
        if (!relativeAsset.empty() && *relativeAsset.begin() != "..")
        {
            const std::filesystem::path engineAsset =
                std::filesystem::path(ENGINE_ASSETS_PATH) / relativeAsset;
            if (std::filesystem::exists(engineAsset))
                return engineAsset.lexically_normal().generic_string();
        }
        const std::filesystem::path besideMaterial =
            std::filesystem::path(materialPath).parent_path() / requested;
        if (std::filesystem::exists(besideMaterial))
            return besideMaterial.lexically_normal().generic_string();
        return requested.lexically_normal().generic_string();
    }

    std::string ResolveTexturePath(const std::string& texturePath)
    {
        return ResolveTexturePath({}, texturePath);
    }
}

bool Material::LoadFromFile(const std::string& path)
{
    try
    {
        const JsonValue root = Engine::Serialization::JsonParseFile(path);
        overrideTextureLod = false;
        textureLod = {};
        shader = ReadSurfaceShader(root);
        shaderError.clear();
        baseColorTexture.reset();
        metallicRoughnessTexture.reset();
        normalTexture.reset();
        heightTexture.reset();
        occlusionTexture.reset();
        emissiveTexture.reset();
        subsurfaceThicknessTexture.reset();
        reflectionEnvironmentMap.reset();
        useCustomReflectionEnvironment = false;
        reflectionEnvironmentTexture.clear();
        reflectionEnvironmentExposure = 0.f;
        reflectionEnvironmentRotation = 0.f;
        castsShadows = true;
        subsurfaceEnabled = false;
        subsurfaceWeight = 0.f;
        subsurfaceRadius = { 0.004f, 0.002f, 0.001f };
        subsurfaceColor = { 1.f, 0.45f, 0.35f };
        subsurfaceThickness = 0.f;
        subsurfaceTransmission = 0.f;
        specularModel = "MetallicRoughness";
        edgeColor = glm::vec4(0.f);
        edgeWidth = 0.f;
        edgeEnabled = false;
        sphereTint = glm::vec4(1.f);
        toonTint = glm::vec4(1.f);
        sphereMode = 0;
        builtinToon = -1;
        sphereTexture.reset();
        toonTexture.reset();
        diffuseColor = from3(root["baseColor"], diffuseColor);
        useVertexColors = !root.Has("useVertexColors") || root["useVertexColors"].AsBool();
        ambientColor = from3(root["ambientColor"], ambientColor);
        specularColor = from3(root["specularColor"], specularColor);
        emissiveColor = from3(root["emissive"], emissiveColor);
        if (root.Has("shininess")) shininess = root["shininess"].AsFloat();
        if (root.Has("specularModel")) specularModel = root["specularModel"].AsString();
        else if (root.Has("useMmdSpecular"))
            specularModel = (root["useMmdSpecular"].AsBool() ||
                root["useMmdSpecular"].AsString() == "true")
                ? "BlinnPhong" : "MetallicRoughness";
        if (root.Has("metallic")) metallicFactor = root["metallic"].AsFloat();
        if (root.Has("roughness")) roughnessFactor = root["roughness"].AsFloat();
        if (root.Has("subsurfaceEnabled")) subsurfaceEnabled = root["subsurfaceEnabled"].AsBool();
        if (root.Has("subsurfaceWeight")) subsurfaceWeight = root["subsurfaceWeight"].AsFloat();
        subsurfaceRadius = from3(root["subsurfaceRadius"], subsurfaceRadius);
        subsurfaceColor = from3(root["subsurfaceColor"], subsurfaceColor);
        if (root.Has("subsurfaceThickness")) subsurfaceThickness = root["subsurfaceThickness"].AsFloat();
        if (root.Has("subsurfaceTransmission")) subsurfaceTransmission = root["subsurfaceTransmission"].AsFloat();
        if (root.Has("environmentDiffuseStrength")) environmentDiffuseStrength = root["environmentDiffuseStrength"].AsFloat();
        if (root.Has("reflectionStrength")) reflectionStrength = root["reflectionStrength"].AsFloat();
        if (root.Has("useCustomReflectionEnvironment")) useCustomReflectionEnvironment = root["useCustomReflectionEnvironment"].AsBool();
        if (root.Has("reflectionEnvironmentExposure")) reflectionEnvironmentExposure = root["reflectionEnvironmentExposure"].AsFloat();
        if (root.Has("reflectionEnvironmentRotation")) reflectionEnvironmentRotation = root["reflectionEnvironmentRotation"].AsFloat();
        if (root.Has("baseColorAlpha")) baseColorAlpha = root["baseColorAlpha"].AsFloat();
        baseColorTextureTint = from3(root["baseColorTextureTint"],
            baseColorTextureTint);
        if (root.Has("baseColorTextureTintAlpha"))
            baseColorTextureTintAlpha = root["baseColorTextureTintAlpha"].AsFloat();
        edgeColor = from4(root["edgeColor"], edgeColor);
        if (root.Has("edgeWidth")) edgeWidth = root["edgeWidth"].AsFloat();
        if (root.Has("edgeEnabled")) edgeEnabled = root["edgeEnabled"].AsBool();
        sphereTint = from4(root["sphereTint"], sphereTint);
        toonTint = from4(root["toonTint"], toonTint);
        if (root.Has("sphereMode")) sphereMode = root["sphereMode"].AsInt();
        if (root.Has("builtinToon")) builtinToon = root["builtinToon"].AsInt();
        if (root.Has("alphaMode")) alphaMode = root["alphaMode"].AsString();
        if (root.Has("alphaCutoff")) alphaCutoff = root["alphaCutoff"].AsFloat();
        if (root.Has("overrideTextureLod")) overrideTextureLod = root["overrideTextureLod"].AsBool();
        if (root.Has("textureLodEnabled")) textureLod.enabled = root["textureLodEnabled"].AsBool();
        if (root.Has("textureLodNearDistance")) textureLod.nearDistance = root["textureLodNearDistance"].AsFloat();
        if (root.Has("textureLodFarDistance")) textureLod.farDistance = root["textureLodFarDistance"].AsFloat();
        if (root.Has("textureLodMipBias")) textureLod.mipBias = root["textureLodMipBias"].AsFloat();
        if (root.Has("textureLodSimplifyShading")) textureLod.simplifyShading = root["textureLodSimplifyShading"].AsBool();
        if (root.Has("textureLodDisableAlphaAtFar")) textureLod.disableAlphaAtFar = root["textureLodDisableAlphaAtFar"].AsBool();
        if (root.Has("textureLodAlphaMipBias")) textureLod.alphaMipBias = root["textureLodAlphaMipBias"].AsFloat();
        if (root.Has("normalScale")) normalScale = root["normalScale"].AsFloat();
        if (root.Has("heightScale")) heightScale = root["heightScale"].AsFloat();
        if (root.Has("terrainHeightBlendStrength"))
            terrainHeightBlendStrength = root["terrainHeightBlendStrength"].AsFloat();
        terrainMacroColor = from3(root["terrainMacroColor"], terrainMacroColor);
        if (root.Has("heightMinSteps")) heightMinSteps = root["heightMinSteps"].AsFloat();
        if (root.Has("heightMaxSteps")) heightMaxSteps = root["heightMaxSteps"].AsFloat();
        if (root.Has("occlusionStrength")) occlusionStrength = root["occlusionStrength"].AsFloat();
        if (root.Has("doubleSided")) doubleSided = root["doubleSided"].AsBool();
        if (root.Has("castsShadows")) castsShadows = root["castsShadows"].AsBool();
        if (root.Has("unlit")) unlit = root["unlit"].AsBool();
        if (root.Has("lightingChannels")) lightingChannels = root["lightingChannels"].AsInt();
        if (root.Has("baseColorUvSet")) baseColorUvSet = root["baseColorUvSet"].AsInt();
        if (root.Has("metallicRoughnessUvSet")) metallicRoughnessUvSet = root["metallicRoughnessUvSet"].AsInt();
        if (root.Has("normalUvSet")) normalUvSet = root["normalUvSet"].AsInt();
        if (root.Has("occlusionUvSet")) occlusionUvSet = root["occlusionUvSet"].AsInt();
        if (root.Has("emissiveUvSet")) emissiveUvSet = root["emissiveUvSet"].AsInt();
        if (root.Has("heightUvSet")) heightUvSet = root["heightUvSet"].AsInt();
        if (root.Has("baseColorTexture")) SetBaseColorTexture(
            ResolveTexturePath(path, root["baseColorTexture"].AsString()));
        if (root.Has("sphereTexture")) SetSphereTexture(
            ResolveTexturePath(path, root["sphereTexture"].AsString()));
        if (root.Has("toonTexture")) SetToonTexture(
            ResolveTexturePath(path, root["toonTexture"].AsString()));
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
        if (root.Has("subsurfaceThicknessTexture")) SetSubsurfaceThicknessTexture(
            ResolveTexturePath(path, root["subsurfaceThicknessTexture"].AsString()));
        if (root.Has("reflectionEnvironmentTexture")) SetReflectionEnvironmentTexture(
            ResolveTexturePath(path, root["reflectionEnvironmentTexture"].AsString()));
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
    root.Set("shader", JsonValue(shader));
    root.Set("baseColor", J3(diffuseColor));
    root.Set("useVertexColors", JsonValue(useVertexColors));
    root.Set("ambientColor", J3(ambientColor));
    root.Set("specularColor", J3(specularColor));
    root.Set("emissive", J3(emissiveColor));
    root.Set("shininess", JsonValue(shininess));
    root.Set("specularModel", JsonValue(specularModel));
    root.Set("metallic", JsonValue(metallicFactor));
    root.Set("roughness", JsonValue(roughnessFactor));
    root.Set("subsurfaceEnabled", JsonValue(subsurfaceEnabled));
    root.Set("subsurfaceWeight", JsonValue(subsurfaceWeight));
    root.Set("subsurfaceRadius", J3(subsurfaceRadius));
    root.Set("subsurfaceColor", J3(subsurfaceColor));
    root.Set("subsurfaceThickness", JsonValue(subsurfaceThickness));
    root.Set("subsurfaceTransmission", JsonValue(subsurfaceTransmission));
    root.Set("environmentDiffuseStrength", JsonValue(environmentDiffuseStrength));
    root.Set("reflectionStrength", JsonValue(reflectionStrength));
    root.Set("useCustomReflectionEnvironment", JsonValue(useCustomReflectionEnvironment));
    root.Set("reflectionEnvironmentTexture", JsonValue(reflectionEnvironmentTexture));
    root.Set("reflectionEnvironmentExposure", JsonValue(reflectionEnvironmentExposure));
    root.Set("reflectionEnvironmentRotation", JsonValue(reflectionEnvironmentRotation));
    root.Set("baseColorAlpha", JsonValue(baseColorAlpha));
    root.Set("baseColorTextureTint", J3(baseColorTextureTint));
    root.Set("baseColorTextureTintAlpha", JsonValue(baseColorTextureTintAlpha));
    root.Set("edgeColor", J4(edgeColor));
    root.Set("edgeWidth", JsonValue(edgeWidth));
    root.Set("edgeEnabled", JsonValue(edgeEnabled));
    root.Set("sphereTint", J4(sphereTint));
    root.Set("toonTint", J4(toonTint));
    root.Set("sphereMode", JsonValue(sphereMode));
    root.Set("builtinToon", JsonValue(builtinToon));
    root.Set("sphereTexture", JsonValue(TexturePath(sphereTexture)));
    root.Set("toonTexture", JsonValue(TexturePath(toonTexture)));
    root.Set("alphaMode", JsonValue(alphaMode));
    root.Set("alphaCutoff", JsonValue(alphaCutoff));
    root.Set("overrideTextureLod", JsonValue(overrideTextureLod));
    root.Set("textureLodEnabled", JsonValue(textureLod.enabled));
    root.Set("textureLodNearDistance", JsonValue(textureLod.nearDistance));
    root.Set("textureLodFarDistance", JsonValue(textureLod.farDistance));
    root.Set("textureLodMipBias", JsonValue(textureLod.mipBias));
    root.Set("textureLodSimplifyShading", JsonValue(textureLod.simplifyShading));
    root.Set("textureLodDisableAlphaAtFar", JsonValue(textureLod.disableAlphaAtFar));
    root.Set("textureLodAlphaMipBias", JsonValue(textureLod.alphaMipBias));
    root.Set("normalScale", JsonValue(normalScale));
    root.Set("heightScale", JsonValue(heightScale));
    root.Set("terrainHeightBlendStrength", JsonValue(terrainHeightBlendStrength));
    root.Set("terrainMacroColor", J3(terrainMacroColor));
    root.Set("heightMinSteps", JsonValue(heightMinSteps));
    root.Set("heightMaxSteps", JsonValue(heightMaxSteps));
    root.Set("occlusionStrength", JsonValue(occlusionStrength));
    root.Set("doubleSided", JsonValue(doubleSided));
    root.Set("castsShadows", JsonValue(castsShadows));
    root.Set("unlit", JsonValue(unlit));
    root.Set("lightingChannels", JsonValue(lightingChannels));
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
    if (subsurfaceThicknessTexture) root.Set("subsurfaceThicknessTexture",
        JsonValue(TexturePath(subsurfaceThicknessTexture)));
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

std::vector<std::filesystem::path> Material::ShaderSearchPaths(const std::string& path) const
{
    if(path.empty()) return {};
    const std::filesystem::path requested(path);
    std::vector<std::filesystem::path> result{requested};
    const auto generic=requested.generic_string();
    constexpr std::string_view marker="Engine/Core/Assets/";
    if(const auto root=generic.find(marker);root!=std::string::npos)
        result.push_back(std::filesystem::path(ENGINE_ASSETS_PATH)/generic.substr(root+marker.size()));
    const auto relative=requested.lexically_relative(std::filesystem::path("Assets"));
    if(!relative.empty() && *relative.begin()!="..")
        result.push_back(std::filesystem::path(ENGINE_ASSETS_PATH)/relative);
    if(!requested.is_absolute())
        result.push_back(std::filesystem::path(m_filePath).parent_path()/requested);
    return result;
}

std::string Material::ResolveShaderPath(const std::string& path) const
{
    if(path.empty()) return {};
    for(const auto& candidate:ShaderSearchPaths(path))
    {
        std::error_code error;
        if(std::filesystem::exists(candidate,error)) return candidate.lexically_normal().generic_string();
    }
    return std::filesystem::path(path).lexically_normal().generic_string();
}

bool Material::DrawShaderProperties(::Engine::Editor::IEditorUi& ui, const void* context)
{
    ui.PushId("surfaceShader");
    bool changed = DrawAssetPathPicker(ui, context ? context : this,
        "surfaceShader", "Shader file", "shader", shader);
    char buffer[4096]{};
    snprintf(buffer, sizeof(buffer), "%s", shader.c_str());
    if (ui.InputTextSubmit("Asset path", buffer, sizeof(buffer)))
    {
        std::string extension = std::filesystem::path(buffer).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!buffer[0] || extension == ".hlsl") { shader = buffer; changed = true; }
    }
    if (!shader.empty() && ui.Button("Use default shader")) { shader.clear(); changed = true; }
    if (!shaderError.empty()) ui.Label(shaderError.c_str());
    ui.PopId();
    return changed;
}

void Material::SetSphereTexture(const std::string& path)
{
    sphereTexture = path.empty() ? nullptr :
        Texture::Acquire(ResolveTexturePath(path), true);
}

void Material::SetToonTexture(const std::string& path)
{
    toonTexture = path.empty() ? nullptr :
        Texture::Acquire(ResolveTexturePath(path), true);
}

void Material::SetBaseColorTexture(const std::string& path)
{
    baseColorTexture = path.empty() ? nullptr :
        Texture::Acquire(ResolveTexturePath(path), true);
}

void Material::SetMetallicRoughnessTexture(const std::string& path)
{
    metallicRoughnessTexture = path.empty() ? nullptr :
        Texture::Acquire(ResolveTexturePath(path), false);
}

void Material::SetNormalTexture(const std::string& path)
{
    normalTexture = path.empty() ? nullptr :
        Texture::Acquire(ResolveTexturePath(path), false);
}

void Material::SetHeightTexture(const std::string& path)
{
    heightTexture = path.empty() ? nullptr :
        Texture::Acquire(ResolveTexturePath(path), false);
}

void Material::SetOcclusionTexture(const std::string& path)
{
    occlusionTexture = path.empty() ? nullptr :
        Texture::Acquire(ResolveTexturePath(path), false);
}

void Material::SetEmissiveTexture(const std::string& path)
{
    emissiveTexture = path.empty() ? nullptr :
        Texture::Acquire(ResolveTexturePath(path), true);
}

void Material::SetSubsurfaceThicknessTexture(const std::string& path)
{
    subsurfaceThicknessTexture = path.empty() ? nullptr :
        Texture::Acquire(ResolveTexturePath(path), false);
}

void Material::SetReflectionEnvironmentTexture(const std::string& path)
{
    reflectionEnvironmentTexture = ResolveTexturePath(path);
    reflectionEnvironmentMap = reflectionEnvironmentTexture.empty() ? nullptr :
        Texture::Acquire(reflectionEnvironmentTexture, false);
    if (reflectionEnvironmentMap) reflectionEnvironmentMap->RetainPixelsForCpu();
}

void Material::PrepareTextures(IGraphicsProvider* graphicsProvider)
{
    if (!reflectionEnvironmentTexture.empty() &&
        (!reflectionEnvironmentMap ||
            reflectionEnvironmentMap->GetFilePath() != reflectionEnvironmentTexture))
        SetReflectionEnvironmentTexture(reflectionEnvironmentTexture);
    const std::array<Texture*,10> textures={baseColorTexture.get(), metallicRoughnessTexture.get(),
        normalTexture.get(), heightTexture.get(), occlusionTexture.get(), emissiveTexture.get(),
        subsurfaceThicknessTexture.get(), sphereTexture.get(), toonTexture.get(), reflectionEnvironmentMap.get()};
    const bool resident=std::all_of(textures.begin(),textures.end(),[&](const Texture* texture) {
        return !texture || texture->IsPreparedFor(graphicsProvider);
    });
    if(m_texturesCached && m_preparedTextureProvider==graphicsProvider &&
        std::equal(textures.begin(),textures.end(),m_preparedTextures.begin()) && resident) return;
    // Raw pointers avoid copying ten shared owners every render preparation.
    // Pending, failed, replaced or invalidated resources still visit QueuePrepare.
    for(auto* texture:textures)
        if(texture) texture->QueuePrepare(graphicsProvider);
    std::copy(textures.begin(),textures.end(),m_preparedTextures.begin());
    m_preparedTextureProvider=graphicsProvider;
    m_texturesCached=resident;
}

MaterialAlphaMode Material::GetAlphaMode() const
{
    // Validation and authored assets use these canonical names. Avoid copying
    // and locale-folding them for every frame item, camera draw and shadow draw.
    // Keep the tolerant path for public-field edits and older mixed-case assets.
    if (alphaMode == "Opaque") return MaterialAlphaMode::Opaque;
    if (alphaMode == "Mask") return MaterialAlphaMode::Mask;
    if (alphaMode == "Blend") return MaterialAlphaMode::Blend;
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
    // Public fields can change without configuration notifications. Keep this
    // normalized-input snapshot in sync with every clamp below.
    const auto captureFloats=[&] { return std::array<float,29>{
        metallicFactor, roughnessFactor, subsurfaceWeight,
        subsurfaceRadius[0], subsurfaceRadius[1], subsurfaceRadius[2],
        subsurfaceColor[0], subsurfaceColor[1], subsurfaceColor[2],
        subsurfaceThickness, subsurfaceTransmission, environmentDiffuseStrength,
        reflectionStrength, reflectionEnvironmentExposure, baseColorAlpha, alphaCutoff,
        normalScale, heightScale, terrainHeightBlendStrength,
        terrainMacroColor[0], terrainMacroColor[1], terrainMacroColor[2],
        heightMinSteps, heightMaxSteps, occlusionStrength,
        textureLod.nearDistance, textureLod.farDistance, textureLod.mipBias, textureLod.alphaMipBias}; };
    const auto captureUvSets=[&] { return std::array<int,6>{baseColorUvSet,
        metallicRoughnessUvSet, normalUvSet, occlusionUvSet, emissiveUvSet, heightUvSet}; };
    if(m_validationCached && (specularModel=="BlinnPhong" || specularModel=="MetallicRoughness") &&
        (alphaMode=="Opaque" || alphaMode=="Mask" || alphaMode=="Blend") &&
        captureFloats()==m_validatedFloats && captureUvSets()==m_validatedUvSets) return;
    if (specularModel != "BlinnPhong")
        specularModel = "MetallicRoughness";
    metallicFactor = std::clamp(metallicFactor, 0.f, 1.f);
    roughnessFactor = std::clamp(roughnessFactor, 0.045f, 1.f);
    subsurfaceWeight = std::clamp(subsurfaceWeight, 0.f, 1.f);
    subsurfaceRadius = glm::clamp(subsurfaceRadius, glm::vec3(0.f), glm::vec3(1.f));
    subsurfaceColor = glm::clamp(subsurfaceColor, glm::vec3(0.f), glm::vec3(1.f));
    subsurfaceThickness = std::clamp(subsurfaceThickness, 0.f, 1.f);
    subsurfaceTransmission = std::clamp(subsurfaceTransmission, 0.f, 1.f);
    environmentDiffuseStrength = std::clamp(environmentDiffuseStrength, 0.f, 4.f);
    reflectionStrength = std::clamp(reflectionStrength, 0.f, 4.f);
    reflectionEnvironmentExposure = std::clamp(
        reflectionEnvironmentExposure, -16.f, 16.f);
    baseColorAlpha = std::clamp(baseColorAlpha, 0.f, 1.f);
    alphaCutoff = std::clamp(alphaCutoff, 0.f, 1.f);
    textureLod.Normalize();
    normalScale = std::max(normalScale, 0.f);
    heightScale = std::clamp(heightScale, 0.f, 0.2f);
    terrainHeightBlendStrength = std::clamp(terrainHeightBlendStrength, 0.f, 1.f);
    terrainMacroColor = glm::clamp(terrainMacroColor,
        glm::vec3(0.f), glm::vec3(1.f));
    heightMinSteps = std::clamp(heightMinSteps, 4.f, 64.f);
    heightMaxSteps = std::clamp(heightMaxSteps, heightMinSteps, 64.f);
    occlusionStrength = std::clamp(occlusionStrength, 0.f, 1.f);
    baseColorUvSet = std::clamp(baseColorUvSet, 0, 9);
    metallicRoughnessUvSet = std::clamp(metallicRoughnessUvSet, 0, 9);
    normalUvSet = std::clamp(normalUvSet, 0, 9);
    occlusionUvSet = std::clamp(occlusionUvSet, 0, 9);
    emissiveUvSet = std::clamp(emissiveUvSet, 0, 9);
    heightUvSet = std::clamp(heightUvSet, 0, 9);
    switch (GetAlphaMode())
    {
    case MaterialAlphaMode::Mask: alphaMode = "Mask"; break;
    case MaterialAlphaMode::Blend: alphaMode = "Blend"; break;
    default: alphaMode = "Opaque"; break;
    }
    m_validatedFloats=captureFloats();
    m_validatedUvSets=captureUvSets();
    m_validationCached=true;
}

Material::JsonValue Material::Serialize() const
{
    JsonValue value = Component::Serialize();
    value.Set("materialAsset", JsonValue(m_filePath));
    value.Set("shader", JsonValue(shader));
    value.Set("edgeColor", J4(edgeColor));
    value.Set("sphereTint", J4(sphereTint));
    value.Set("toonTint", J4(toonTint));
    value.Set("sphereTexture", JsonValue(TexturePath(sphereTexture)));
    value.Set("toonTexture", JsonValue(TexturePath(toonTexture)));
    value.Set("baseColorTexture", JsonValue(TexturePath(baseColorTexture)));
    value.Set("metallicRoughnessTexture", JsonValue(TexturePath(metallicRoughnessTexture)));
    value.Set("normalTexture", JsonValue(TexturePath(normalTexture)));
    value.Set("heightTexture", JsonValue(TexturePath(heightTexture)));
    value.Set("occlusionTexture", JsonValue(TexturePath(occlusionTexture)));
    value.Set("emissiveTexture", JsonValue(TexturePath(emissiveTexture)));
    value.Set("subsurfaceThicknessTexture",
        JsonValue(TexturePath(subsurfaceThicknessTexture)));
    value.Set("reflectionEnvironmentTexture", JsonValue(reflectionEnvironmentTexture));
    return value;
}

void Material::CopyResolvedStateFrom(const Material& source,
    const JsonValue* serializedSource)
{
    if (&source == this) return;
    // Registered fields write through this component's bindings, never copy the
    // source's callbacks, Owner or lifecycle state. Serialized textures are not
    // registered fields; retain the exact resolved resources below instead of
    // reloading the material file and resolving ten texture paths per batch.
    if (serializedSource) Component::Deserialize(*serializedSource);
    else Component::Deserialize(source.Serialize());
    m_filePath = source.m_filePath;
    shader = source.shader;
    shaderError.clear();
    edgeColor = source.edgeColor;
    sphereTint = source.sphereTint;
    toonTint = source.toonTint;
    baseColorTexture = source.baseColorTexture;
    metallicRoughnessTexture = source.metallicRoughnessTexture;
    normalTexture = source.normalTexture;
    heightTexture = source.heightTexture;
    occlusionTexture = source.occlusionTexture;
    emissiveTexture = source.emissiveTexture;
    subsurfaceThicknessTexture = source.subsurfaceThicknessTexture;
    sphereTexture = source.sphereTexture;
    toonTexture = source.toonTexture;
    reflectionEnvironmentMap = source.reflectionEnvironmentMap;
    m_validationCached = false;
    m_texturesCached = false;
    m_preparedTextures = {};
    m_preparedTextureProvider = nullptr;
    Validate();
}

void Material::Deserialize(const JsonValue& value)
{
    if (value.Has("materialAsset") && !value["materialAsset"].AsString().empty())
        LoadFromFile(value["materialAsset"].AsString());
    Component::Deserialize(value);
    if (value.Has("shader") || value.Has("shaders")) shader = ReadSurfaceShader(value);
    shaderError.clear();
    if (!value.Has("specularModel") && value.Has("useMmdSpecular"))
        specularModel = (value["useMmdSpecular"].AsBool() ||
            value["useMmdSpecular"].AsString() == "true")
            ? "BlinnPhong" : "MetallicRoughness";
    edgeColor = from4(value["edgeColor"], edgeColor);
    sphereTint = from4(value["sphereTint"], sphereTint);
    toonTint = from4(value["toonTint"], toonTint);
    if (value.Has("baseColorTexture"))
        SetBaseColorTexture(value["baseColorTexture"].AsString());
    if (value.Has("sphereTexture"))
        SetSphereTexture(value["sphereTexture"].AsString());
    if (value.Has("toonTexture"))
        SetToonTexture(value["toonTexture"].AsString());
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
    if (value.Has("subsurfaceThicknessTexture"))
        SetSubsurfaceThicknessTexture(value["subsurfaceThicknessTexture"].AsString());
    if (value.Has("reflectionEnvironmentTexture"))
        SetReflectionEnvironmentTexture(value["reflectionEnvironmentTexture"].AsString());
    Validate();
}
}
