#pragma once
#include "Core/component.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>

class Texture;
class IGraphicsProvider;

class Material : public Component
{
public:
    Material();
    ~Material() = default;

    // Phong shading properties — must match the CBData layout expected by the HLSL.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Color")
    glm::vec3 diffuseColor  { 1.f, 0.5f, 0.1f }; // base/albedo colour
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Color")
    glm::vec3 ambientColor  { 0.1f, 0.1f, 0.1f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Color")
    glm::vec3 specularColor { 1.f, 1.f, 1.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties", Range = "1.0, 256.0")
    float     shininess     { 32.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR", Range = "0.0, 1.0")
    float     metallicFactor { 0.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR", Range = "0.0, 1.0")
    float     roughnessFactor { 1.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    float     baseColorAlpha { 1.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    float     alphaCutoff { 0.5f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    float     normalScale { 1.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    float     occlusionStrength { 1.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    bool      doubleSided { false };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    bool      unlit { false };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    std::string alphaMode { "Opaque" };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Color")
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

private:
    static std::string TexturePath(const std::shared_ptr<Texture>& texture);
    std::string m_filePath;
};
