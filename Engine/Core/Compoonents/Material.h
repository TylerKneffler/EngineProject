#pragma once
#include "Core/component.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>

class Texture;
class IGraphicsProvider;

class Material : public Component
{
public:
    Material() = default;
    ~Material() = default;

    // Phong shading properties — must match the CBData layout expected by the HLSL.
    glm::vec3 diffuseColor  { 1.f, 0.5f, 0.1f }; // base/albedo colour
    glm::vec3 ambientColor  { 0.1f, 0.1f, 0.1f };
    glm::vec3 specularColor { 1.f, 1.f, 1.f };
    float     shininess     { 32.f };
    float     metallicFactor { 1.f };
    float     roughnessFactor { 1.f };
    float     baseColorAlpha { 1.f };
    float     alphaCutoff { 0.5f };
    float     normalScale { 1.f };
    float     occlusionStrength { 1.f };
    bool      doubleSided { false };
    bool      unlit { false };
    std::string alphaMode { "Opaque" };
    glm::vec3 emissiveColor { 0.f, 0.f, 0.f };
    std::shared_ptr<Texture> baseColorTexture;
    std::shared_ptr<Texture> metallicRoughnessTexture;
    std::shared_ptr<Texture> normalTexture;
    std::shared_ptr<Texture> occlusionTexture;
    std::shared_ptr<Texture> emissiveTexture;

    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;
    const std::string& GetFilePath() const { return m_filePath; }
    void SetBaseColorTexture(const std::string& path);
    void SetMetallicRoughnessTexture(const std::string& path);
    void SetNormalTexture(const std::string& path);
    void SetOcclusionTexture(const std::string& path);
    void SetEmissiveTexture(const std::string& path);
    void PrepareTextures(IGraphicsProvider* graphicsProvider);

    // Serialization
    std::string GetTypeName() const override { return "Material"; }
    JsonValue   Serialize()   const override;
    void        Deserialize(const JsonValue& v) override;

private:
    static std::string TexturePath(const std::shared_ptr<Texture>& texture);
    std::string m_filePath;
};
