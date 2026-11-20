#pragma once
#include "Core/Model/TextureLodSettings.h"
#include "Core/Components/Component.h"
#include "Core/Components/PropertyMacros.h"
#include <glm/glm.hpp>
#include <memory>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine::Components
{
class Texture;
enum class MaterialAlphaMode : int
{
    Opaque,
    Mask,
    Blend
};

class Material : public Engine::Core::Component
{
public:
    using JsonValue = Engine::Serialization::JsonValue;
    using IGraphicsProvider = Engine::Graphics::IGraphicsProvider;
    Material();
    bool RequiresUpdate() const override { return false; }
    ~Material() = default;

    // One surface shader per material. Empty selects the built-in surface.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Shader")
    std::string shader;
    std::string ResolveShaderPath(const std::string& path) const;
    // Ordered candidates, including absent higher-priority locations for watching.
    std::vector<std::filesystem::path> ShaderSearchPaths(const std::string& path) const;
    bool DrawShaderProperties(Engine::Core::IInspectorUi& ui,
        const void* context = nullptr);
    // Runtime diagnostic displayed by the inspector; never serialized.
    std::string shaderError;

    // Phong shading properties — must match the CBData layout expected by the HLSL.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Color")
    glm::vec3 diffuseColor  { 1.f, 0.5f, 0.1f }; // base/albedo colour
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Color")
    bool useVertexColors = true;
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Color")
    glm::vec3 ambientColor  { 0.1f, 0.1f, 0.1f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Color")
    glm::vec3 specularColor { 1.f, 1.f, 1.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties", Range = "1.0, 256.0")
    float     shininess     { 32.f };

    // Select the specular lighting model independently of the source format.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    std::string specularModel { "MetallicRoughness" };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR", Range = "0.0, 1.0")
    float     metallicFactor { 0.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR", Range = "0.0, 1.0")
    float     roughnessFactor { 1.f };

    // Forward-shader preview of subsurface lighting. Radius is authored in
    // scene units; a future screen-space diffusion pass can use the same data.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Subsurface")
    bool subsurfaceEnabled { false };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | Subsurface", Range = "0.0, 1.0")
    float subsurfaceWeight { 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | Subsurface")
    glm::vec3 subsurfaceRadius { 0.004f, 0.002f, 0.001f };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | Subsurface")
    glm::vec3 subsurfaceColor { 1.f, 0.45f, 0.35f };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | Subsurface", Range = "0.0, 1.0")
    float subsurfaceThickness { 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | Subsurface", Range = "0.0, 1.0")
    float subsurfaceTransmission { 0.f };

    // Fractional coverage for masked foliage. The renderer uses stable
    // stochastic coverage instead of alpha blending and transparent sorting.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Alpha", Range = "0.0, 1.0")
    float alphaCoverageTransmission { 0.f };

    // Masked cards thin out as they turn edge-on to the camera, hiding the
    // smeared texels of low-LOD cross and horizontal planes. Uses the flat
    // triangle normal, so it suits flat cards, not round masked geometry.
    // 0 disables; 1 removes cards within ~6 degrees of edge-on and fades them
    // in fully by ~27 degrees. Camera views only: shadows are unaffected.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Alpha", Range = "0.0, 2.0")
    float alphaEdgeFade { 0.f };

    // View-baked impostor cards (a tree's far level): every vertical card of
    // the mesh is a plane through the object's up axis whose normal is the
    // direction its view was baked from. The vertex shader turns each card
    // about that axis to face the camera and shows only the one or two views
    // nearest the camera direction, cross-faded through vertex alpha, so the
    // object never shows a card edge-on or a crossed-plane seam. Horizontal
    // (top-down) cards appear only when looking steeply down. Needs Mask
    // alpha and vertex colors (white) for the cross-fade.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Alpha")
    bool impostorBillboard { false };

    // World-space triplanar mapping for rock and cliff props: base color,
    // metallic-roughness, occlusion and normal textures are projected along
    // the three world axes and blended by the surface normal, so scaled or
    // rotated placements never stretch the texture and props of any size share
    // one texel density. Metres per texture repeat; 0 uses the mesh UVs.
    // Opaque and masked surfaces; parallax is skipped while it is on.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR", Range = "0.0, 64.0")
    float triplanarScale { 0.f };

    // Masked foliage: varies hue (yellow-green to blue-green) and value with
    // world-space noise at crown (~9 m) and stand (~70 m) scales, so forests
    // are not one flat green. Near leaf cards, impostors and forest clusters
    // that share the amount vary identically. 0 disables; 1 is about
    // +-20% value and a clear hue spread.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Alpha", Range = "0.0, 1.0")
    float foliageColorVariation { 0.f };

    // Masked leaves (.md/todo.md Foliage Phase 1): sunlight passing through thin
    // leaves when the sun is behind them, and a soft wrapped diffuse. On
    // meshes cooked by tools/cook_foliage_lighting.py the baked crown normal
    // is kept on both card faces and uv1 adds canopy occlusion, so a crown
    // shades as one volume. 0 disables; 1 suits a typical broadleaf.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Alpha", Range = "0.0, 2.0")
    float foliageTranslucency { 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR", Range = "0.0, 4.0")
    float     environmentDiffuseStrength { 1.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR", Range = "0.0, 4.0")
    float     reflectionStrength { 1.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | Reflections")
    bool      useCustomReflectionEnvironment { false };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | Reflections")
    std::string reflectionEnvironmentTexture;

    PROPERTY(Inspector, EditAnywhere, Category = "Material | Reflections", Range = "-16.0, 16.0")
    float     reflectionEnvironmentExposure { 0.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | Reflections", Range = "-360.0, 360.0")
    float     reflectionEnvironmentRotation { 0.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    float     baseColorAlpha { 1.f };
    // PMX texture morph factor, applied only to the sampled base texture.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Color")
    glm::vec3 baseColorTextureTint { 1.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Color")
    float baseColorTextureTintAlpha { 1.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Material | MMD")
    glm::vec4 edgeColor { 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Material | MMD")
    float edgeWidth { 0.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Material | MMD")
    bool edgeEnabled { false };
    PROPERTY(Inspector, EditAnywhere, Category = "Material | MMD")
    glm::vec4 sphereTint { 1.f };
    PROPERTY(Inspector, EditAnywhere, Category = "Material | MMD")
    glm::vec4 toonTint { 1.f };
    std::shared_ptr<Texture> sphereTexture;
    std::shared_ptr<Texture> toonTexture;
    int sphereMode = 0; // PMX: 1 multiply, 2 add, 3 additional UV.
    int builtinToon = -1;
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    float     alphaCutoff { 0.5f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    float     normalScale { 1.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR", Range = "0.0, 0.2")
    float     heightScale { 0.05f };

    // Height textures are white = raised by default. Enable for depth maps
    // authored white = recessed (some relief/POM tutorials use this form).
    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR")
    bool      heightIsDepth { false };

    // Soft relief shadows from directional lights (POM only, fades by 15 m).
    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR")
    bool      parallaxSelfShadow { false };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR", Range = "0.0, 1.0")
    float     parallaxShadowStrength { 1.f };

    // Discard pixels whose parallax-shifted UV leaves 0..1 (decals, trims).
    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR")
    bool      parallaxClampUv { false };

    // Write the relief depth (opaque, non-terrain draws) so props sink into
    // the visible surface. Costs early-Z precision for this draw.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR")
    bool      parallaxDepthOffset { false };

    // Editor-only, not serialized: shade with the per-pixel parallax fetch
    // count (blue 0 .. red 32) instead of lighting.
    bool      parallaxCostView { false };

    // Optional terrain-only paint transition driven by heightTexture.r.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Terrain", Range = "0.0, 1.0")
    float     terrainHeightBlendStrength { 0.f };

    // Linear RGB average of the layer albedo's lowest mip for distant terrain.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Terrain")
    glm::vec3 terrainMacroColor { 0.f };

    // Terrain layer repeats relative to the terrain's texture tiling: 2 packs
    // the texture twice as densely (small pebbles), 0.5 spreads it (rock).
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Terrain", Range = "0.05, 16.0")
    float     terrainUvScale { 1.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR", Range = "4.0, 64.0")
    float     heightMinSteps { 8.f };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | PBR", Range = "4.0, 64.0")
    float     heightMaxSteps { 32.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    float     occlusionStrength { 1.f };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    bool      doubleSided { false };
    // The material's custom shader animates with scene time (read from
    // ObjectData.grassAnimation.x). Off by default so static draws keep
    // identical per-frame object data.
    bool      shaderUsesTime { false };

    bool overrideTextureLod = false;
    Engine::Model::TextureLodSettings textureLod;

    // Opaque and masked mesh materials cast realtime shadows by default.
    // Blended materials remain excluded by the shadow pipeline.
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Shadows")
    bool      castsShadows { true };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    bool      unlit { false };

    PROPERTY(Inspector, EditAnywhere, Category = "Material | Lighting")
    int       lightingChannels { -1 };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Properties")
    std::string alphaMode { "Opaque" };
    
    PROPERTY(Inspector, EditAnywhere, Category = "Material | Color")
    glm::vec3 emissiveColor { 0.f, 0.f, 0.f };
    std::shared_ptr<Texture> baseColorTexture;
    std::shared_ptr<Texture> metallicRoughnessTexture;
    std::shared_ptr<Texture> normalTexture;
    std::shared_ptr<Texture> heightTexture;
    std::shared_ptr<Texture> occlusionTexture;
    std::shared_ptr<Texture> emissiveTexture;
    std::shared_ptr<Texture> subsurfaceThicknessTexture;
    std::shared_ptr<Texture> reflectionEnvironmentMap;
    // 0-4 select XY of primary/additional UV1-4; 5-9 select their ZW.
    int baseColorUvSet = 0;
    int metallicRoughnessUvSet = 0;
    int normalUvSet = 0;
    int occlusionUvSet = 0;
    int emissiveUvSet = 0;
    int heightUvSet = 0;

    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;
    const std::string& GetFilePath() const { return m_filePath; }
    void SetBaseColorTexture(const std::string& path);
    void SetSphereTexture(const std::string& path);
    void SetToonTexture(const std::string& path);
    void SetMetallicRoughnessTexture(const std::string& path);
    
    void SetNormalTexture(const std::string& path);
    void SetHeightTexture(const std::string& path);
    void SetOcclusionTexture(const std::string& path);
    void SetEmissiveTexture(const std::string& path);
    void SetSubsurfaceThicknessTexture(const std::string& path);
    void SetReflectionEnvironmentTexture(const std::string& path);
    void PrepareTextures(IGraphicsProvider* graphicsProvider);
    // Read-only: false when PrepareTextures would do nothing (same textures,
    // all resident for this provider). Safe to call from worker threads for
    // different materials.
    bool NeedsTexturePreparation(const IGraphicsProvider* graphicsProvider) const;
    // Shader prewarm memo for the renderer: a material whose shader, asset
    // path and terrain use are unchanged since it was prewarmed under the
    // same scene shader epoch skips the per-frame prewarm request.
    struct ShaderPrewarmEpoch
    {
        const void* scene = nullptr;
        int64_t sourceGeneration = 0;
        uint64_t bytecodeEpoch = 0;
        bool operator==(const ShaderPrewarmEpoch& other) const
        { return scene == other.scene && sourceGeneration == other.sourceGeneration &&
            bytecodeEpoch == other.bytecodeEpoch; }
    };
    // Read-only; safe from worker threads for different materials.
    bool NeedsShaderPrewarm(const ShaderPrewarmEpoch& epoch, bool terrain) const;
    void MarkShaderPrewarmed(const ShaderPrewarmEpoch& epoch, bool terrain);
    MaterialAlphaMode GetAlphaMode() const;
    void Validate();
    // Copy an already resolved material into an independent component. Texture
    // resources keep shared ownership; owner/lifecycle/field bindings stay local.
    // File deserialization still resolves current asset identity normally.
    void CopyResolvedStateFrom(const Material& source,
        const JsonValue* serializedSource = nullptr);
    // Material asset per prefab detail level: entry i applies at LOD i + 1;
    // an empty or missing entry keeps this material. See Mesh::lodMeshes.
    std::vector<std::string> lodMaterials;
    // The material this object draws with at detail level `lod`: this
    // component for level 0 or an empty entry, otherwise the lodMaterials
    // asset, loaded on first request and owned here. A missing asset falls
    // back to this material.
    Material* GetLodMaterial(int lod);
    JsonValue Serialize() const override;
    void Deserialize(const JsonValue& value) override;
    bool DrawProperties(Engine::Core::IInspectorUi& ui) override;
    void OnAfterDeserialize(IGraphicsProvider* graphicsProvider) override
    {
        if (graphicsProvider && !DeferGraphicsPreparation(graphicsProvider))
            PrepareGraphicsResources(graphicsProvider);
    }
    void PrepareGraphicsResources(IGraphicsProvider* graphicsProvider) override
    { PrepareTextures(graphicsProvider); }

private:
    bool m_validationCached=false;
    std::array<float,31> m_validatedFloats{};
    std::array<int,6> m_validatedUvSets{};
    std::array<const Texture*,10> m_preparedTextures{};
    IGraphicsProvider* m_preparedTextureProvider=nullptr;
    bool m_texturesCached=false;
    struct
    {
        ShaderPrewarmEpoch epoch;
        std::string shader, file;
        bool terrain = false, valid = false;
    } m_shaderPrewarm;
    static std::string TexturePath(const std::shared_ptr<Texture>& texture);
    std::string m_filePath;
    struct LodMaterialInstance
    {
        std::string path;
        std::unique_ptr<Material> material; // null when the asset failed to load
    };
    std::vector<LodMaterialInstance> m_lodMaterialInstances;
};
}
