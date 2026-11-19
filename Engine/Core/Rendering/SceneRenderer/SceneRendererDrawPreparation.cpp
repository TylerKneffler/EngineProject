#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"

#include "Core/Components/Materials/Material.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Sprite/Sprite.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainBatchAppearance.h"
#include "Core/Components/Terrain/TerrainLayerArrays.h"
#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Rendering/Lighting/EnvironmentMap.h"
#include "Core/Rendering/MaterialCachePolicy.h"
#include "Core/Rendering/RenderUtilities.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace Engine::Rendering
{
using Scene = Engine::Scene::Scene;
ObjectLightingPolicy SceneRendererDrawPreparation::ResolveLightingPolicy(
    const Engine::Model::DistanceLightingSettings& settings,
    float lightingDistanceSquared,
    bool bakedStaticReceiver)
{
    ObjectLightingPolicy policy;
    if (settings.enabled && !settings.bands.empty())
    {
        const float distance = std::sqrt(lightingDistanceSquared);
        policy.beyondMaximumDistance = distance > settings.maximumDistance;
        for (const auto& candidate : settings.bands)
        {
            policy.band = &candidate;
            if (distance <= candidate.endDistance)
                break;
        }
    }
    policy.allowNormalMapping = !policy.beyondMaximumDistance &&
        (!policy.band || policy.band->normalMapping);
    policy.allowParallaxMapping = !policy.beyondMaximumDistance &&
        (!policy.band || policy.band->parallaxMapping);
    policy.allowSubsurfaceScattering = !policy.beyondMaximumDistance &&
        (!policy.band || policy.band->subsurfaceScattering);
    policy.allowEnvironmentDiffuse = !policy.beyondMaximumDistance &&
        (!policy.band || policy.band->environmentDiffuse);
    policy.allowReflections = !policy.beyondMaximumDistance &&
        (!policy.band || policy.band->reflections);
    policy.allowRealtimeShadows = !bakedStaticReceiver &&
        !policy.beyondMaximumDistance &&
        (!policy.band || policy.band->realtimeShadows);
    policy.minimumProjectedPixels = !policy.beyondMaximumDistance && policy.band
        ? std::max(0.f, policy.band->minimumProjectedPixels) : 0.f;
    return policy;
}

bool SceneRendererDrawPreparation::IsBelowProjectedSizeThreshold(
    const SceneRenderer::ViewRenderItem& item,
    const Engine::Components::Mesh* mesh,
    bool hasSprite,
    bool belongsToPreview,
    bool shadowOnly,
    const Engine::Components::Camera* camera,
    uint32_t viewportHeight,
    float minimumProjectedPixels)
{
    if (shadowOnly || minimumProjectedPixels <= 0.f || !mesh ||
        !mesh->HasBounds() || hasSprite || belongsToPreview)
        return false;
    const auto bounds = Engine::Rendering::TransformBounds(
        mesh->GetBoundsMin(), mesh->GetBoundsMax(), item.source->world);
    const glm::vec3 extent = (bounds.maximum - bounds.minimum) * 0.5f;
    const float radius = glm::length(extent);
    if (radius <= 0.f)
        return false;
    const float centerDistance = std::sqrt(
        std::max(0.f, item.cameraDistanceSquared));
    const float distance = std::max(0.001f, centerDistance - radius);
    float projectedDiameter = 0.f;
    if (camera->orthographic)
    {
        const float halfHeight = std::max(0.01f, camera->orthographicSize);
        projectedDiameter = radius * static_cast<float>(viewportHeight) / halfHeight;
    }
    else
    {
        const float halfFov = glm::radians(std::clamp(camera->fov, 1.f, 179.f)) * 0.5f;
        const float tanHalfFov = std::tan(halfFov);
        if (tanHalfFov > 0.f)
            projectedDiameter = radius * static_cast<float>(viewportHeight) /
                (tanHalfFov * distance);
    }
    return projectedDiameter > 0.f &&
        projectedDiameter < minimumProjectedPixels;
}

Engine::Components::Mesh* SceneRendererDrawPreparation::ScreenLodPolicy(
    SceneRenderer& renderer,
    const PreparedDraw& draw,
    bool hasPortalViews,
    bool includeEditorVisuals)
{
    Scene& scene = renderer.GetScene();
    auto* source = draw.object
        ? draw.object->GetComponent<Engine::Components::Mesh>() : nullptr;
    const bool editorSelection = includeEditorVisuals &&
        (draw.object == renderer.GetViewOverlays().selectedObject ||
            source == renderer.GetViewOverlays().selectedMesh ||
            source == renderer.GetViewOverlays().weightPaintMesh);
    const bool eligible = source && source->screenLodSelection &&
        !source->distanceLodOverride && !hasPortalViews &&
        !scene.IsMode2D() && !draw.blended && !draw.preview &&
        !draw.pmxOutline && draw.mesh && draw.mesh->HasBounds() &&
        !draw.mesh->HasMorphTargets() && !draw.mesh->HasSurfaceOffsets() &&
        draw.objectData.skinParams.y <= 0.f &&
        draw.vertexBuffer == draw.mesh->GetGraphicsBuffer() &&
        !draw.spatialManipulator && !draw.traversalChartPortal &&
        !editorSelection &&
        (!draw.material || IsFastFoliageSurface(draw.material) ||
            draw.material->shader.empty()) &&
        (draw.gpuLod >= 0 || (draw.drawData.flags & 0x00100000u));
    return eligible ? source : nullptr;
}

void SceneRendererDrawPreparation::BindObjectGeometry(
    PreparedDraw& draw,
    const SceneRenderer::FrameRenderItem& item,
    bool useSourceChart,
    bool useSoftSurface)
{
    auto* mesh = item.mesh;
    auto* sprite = item.sprite;
    draw.vertexBuffer = sprite
        ? item.spriteVertexBuffer
        : (!useSourceChart && item.warpedVertexBuffer
            ? item.warpedVertexBuffer
            : (mesh ? (useSoftSurface ? mesh->GetPosedVertexBuffer()
                : mesh->GetGraphicsBuffer()) : nullptr));
    draw.vertexStride = sprite
        ? sprite->GetVertexStride() : mesh->GetVertexStride();
    draw.vertexCount = sprite
        ? sprite->GetVertexCount() : mesh->GetVertexCount();
    draw.shadowMesh = mesh;
    if (!sprite && mesh && !mesh->IsInstanced() &&
        draw.gpuLod > 0 && mesh->gpuLodSelection)
    {
        if (auto* runtimeShadowMesh = mesh->GetRuntimeLodMesh(draw.gpuLod))
            draw.shadowMesh = runtimeShadowMesh;
    }
    draw.shadowVertexBuffer = sprite
        ? draw.vertexBuffer
        : (draw.shadowMesh ? draw.shadowMesh->GetGraphicsBuffer()
            : draw.vertexBuffer);
    draw.shadowVertexStride = sprite
        ? draw.vertexStride
        : (draw.shadowMesh ? draw.shadowMesh->GetVertexStride()
            : draw.vertexStride);
    draw.shadowVertexCount = sprite
        ? draw.vertexCount
        : (draw.shadowMesh ? draw.shadowMesh->GetVertexCount()
            : draw.vertexCount);
    const bool usesDirectMeshBuffer = !sprite && !useSourceChart &&
        !item.warpedVertexBuffer;
    if (usesDirectMeshBuffer && mesh && mesh->GetIndexBuffer())
    {
        draw.indexBuffer = mesh->GetIndexBuffer();
        draw.indexCount = mesh->GetIndexCount();
    }
    if (usesDirectMeshBuffer && draw.shadowMesh &&
        draw.shadowMesh->GetIndexBuffer())
    {
        draw.shadowIndexBuffer = draw.shadowMesh->GetIndexBuffer();
        draw.shadowIndexCount = draw.shadowMesh->GetIndexCount();
    }
}

ResolvedMaterialRecord SceneRendererDrawPreparation::ResolveMaterialRecord(
    SceneRenderer& renderer,
    Engine::Components::Material& material,
    const ObjectGPUData& environmentSeed,
    const std::array<const Engine::Graphics::IGraphicsTexture*, 30>& initialTextures,
    const glm::vec3& cameraPosition,
    bool allowNormalMapping,
    bool allowParallaxMapping,
    bool allowSubsurfaceScattering,
    bool allowEnvironmentDiffuse,
    bool allowReflections)
{
    Scene& scene = renderer.GetScene();
    scene.AssertOwnerThread();
    ObjectGPUData data{};
    auto textures = initialTextures;
    bool doubleSided = false;
    auto alphaMode = Engine::Components::MaterialAlphaMode::Opaque;
    data.environmentParams = environmentSeed.environmentParams;
    data.reflectionEnvironmentParams = environmentSeed.reflectionEnvironmentParams;
    const uint32_t variant = (allowNormalMapping ? 1u : 0u) |
        (allowParallaxMapping ? 2u : 0u) |
        (allowEnvironmentDiffuse ? 4u : 0u) |
        (allowReflections ? 8u : 0u) |
        (allowSubsurfaceScattering ? 16u : 0u);
    // With the render material cache off, the record is built into a scratch
    // entry that is never reused (MaterialCacheSettings::renderMaterialCache).
    SceneRenderer::CachedRenderMaterial uncached;
    const bool useCache =
        Engine::Rendering::MaterialCachePolicy::Get().RenderMaterialCacheEnabled();
    auto& cached = useCache
        ? renderer.m_renderMaterialCache[&material][variant] : uncached;
    if (useCache && cached.frame == renderer.m_materialPreparationSerial &&
        cached.revision == material.GetConfigurationRevision() &&
        cached.data.size() == sizeof(ObjectGPUData))
    {
        std::memcpy(&data, cached.data.data(), sizeof(data));
        textures = cached.textures;
        doubleSided = cached.doubleSided;
        alphaMode = static_cast<Engine::Components::MaterialAlphaMode>(cached.alphaMode);
        data.viewPositionAlphaCutoff = glm::vec4(cameraPosition,
            data.viewPositionAlphaCutoff.w);
    }
    else
    {
        auto* mat = &material;
        alphaMode = material.GetAlphaMode();
        doubleSided = material.doubleSided;
        uint32_t textureFlags = material.useVertexColors ? 0u : 4096u;
        auto prepareTexture = [&](uint32_t slot,
            const std::shared_ptr<Engine::Components::Texture>& texture,
            uint32_t flag)
        {
            const auto* graphicsTexture = texture
                ? texture->GetGraphicsTexture() : nullptr;
            textures[slot] = graphicsTexture;
            if (graphicsTexture) textureFlags |= flag;
        };
        prepareTexture(0, mat->baseColorTexture, 1u);
        prepareTexture(10, mat->sphereTexture, 256u);
        prepareTexture(11, mat->toonTexture, 512u);
        prepareTexture(1, mat->metallicRoughnessTexture, 2u);
        if (allowNormalMapping) prepareTexture(2, mat->normalTexture, 4u);
        prepareTexture(3, mat->occlusionTexture, 8u);
        prepareTexture(4, mat->emissiveTexture, 16u);
        if (allowSubsurfaceScattering && mat->subsurfaceEnabled &&
            mat->subsurfaceWeight > 0.f && !mat->unlit &&
            alphaMode != Engine::Components::MaterialAlphaMode::Blend)
            prepareTexture(9, mat->subsurfaceThicknessTexture, 128u);
        if (allowParallaxMapping) prepareTexture(5, mat->heightTexture, 64u);
        if (alphaMode == Engine::Components::MaterialAlphaMode::Mask)
            textureFlags |= 32u;
        // Camera-facing view-baked impostor cards (Object.hlsl VSMain).
        if (mat->impostorBillboard) textureFlags |= 1024u;

        data.baseColor = glm::vec4(mat->diffuseColor, mat->baseColorAlpha);
        data.pmxTextureTint = glm::vec4(mat->baseColorTextureTint,
            mat->baseColorTextureTintAlpha);
        data.pmxSphereTint = mat->sphereTint;
        data.pmxToonTint = mat->toonTint;
        data.pmxEdgeColor = mat->edgeColor;
        data.pmxParams = { mat->edgeWidth, static_cast<float>(mat->sphereMode),
            static_cast<float>(mat->builtinToon), mat->edgeEnabled ? 1.f : 0.f };
        data.ambientUnlit = glm::vec4(scene.settings.ambientColor + mat->ambientColor,
            mat->unlit ? 1.f : 0.f);
        data.emissiveOcclusion = glm::vec4(mat->emissiveColor, mat->occlusionStrength);
        data.materialParams = { mat->metallicFactor, mat->roughnessFactor,
            mat->normalScale, static_cast<float>(textureFlags) };
        if (mat->specularModel == "BlinnPhong")
            data.blinnPhongSpecularShininess = glm::vec4(mat->specularColor,
                std::max(mat->shininess, 0.f) + 1.f);
        if (allowSubsurfaceScattering && mat->subsurfaceEnabled && !mat->unlit &&
            alphaMode != Engine::Components::MaterialAlphaMode::Blend)
        {
            data.subsurfaceColorWeight = glm::vec4(mat->subsurfaceColor,
                mat->subsurfaceWeight);
            data.subsurfaceRadiusThickness = glm::vec4(mat->subsurfaceRadius,
                mat->subsurfaceThickness);
            data.subsurfaceTransmission.x = mat->subsurfaceTransmission;
        }
        data.viewPositionAlphaCutoff = glm::vec4(cameraPosition, mat->alphaCutoff);
        // w: parallax flags in the integer part (1 = height texture stores
        // depth, 2 = relief self-shadow, 4 = clip outside 0..1 UV, 8 = depth
        // offset, 16 = editor cost view) and the self-shadow strength * 0.5
        // in the fraction (see Object.hlsl).
        const int parallaxFlags = (mat->heightIsDepth ? 1 : 0) |
            (mat->parallaxSelfShadow ? 2 : 0) | (mat->parallaxClampUv ? 4 : 0) |
            (mat->parallaxDepthOffset ? 8 : 0) | (mat->parallaxCostView ? 16 : 0);
        data.parallaxParams = { mat->heightScale, mat->heightMinSteps,
            mat->heightMaxSteps, static_cast<float>(parallaxFlags) +
                std::clamp(mat->parallaxShadowStrength, 0.f, 1.f) * 0.5f };
        data.environmentParams.z = allowEnvironmentDiffuse
            ? mat->environmentDiffuseStrength : 0.f;
        data.environmentParams.w = allowReflections ? mat->reflectionStrength : 0.f;
        if (allowReflections)
        if (const auto environment = renderer.ResolveReflectionEnvironment(*mat))
        {
            data.reflectionEnvironmentParams = {
                std::exp2(std::clamp(mat->reflectionEnvironmentExposure, -16.f, 16.f)),
                glm::radians(mat->reflectionEnvironmentRotation), 1.f, 0.f };
            const auto& sh = environment->GetRadianceSH();
            std::copy(sh.begin(), sh.end(), data.reflectionEnvironmentSH);
            if (environment->GetSpecularTexture())
            {
                textures[6] = environment->GetSpecularTexture();
                data.reflectionEnvironmentParams.w = 1.f;
            }
        }
        data.textureUvSets0 = { float(mat->baseColorUvSet),
            float(mat->metallicRoughnessUvSet), float(mat->normalUvSet),
            float(mat->occlusionUvSet) };
        // z: triplanar metres per repeat, w: foliage colour variation
        // (Object.hlsl); terrain draws reuse these lanes for their tiling
        // and runtime lighting after the material record is applied.
        data.textureUvSets1 = { float(mat->emissiveUvSet),
            float(mat->heightUvSet), std::max(0.f, mat->triplanarScale),
            std::clamp(mat->foliageColorVariation, 0.f, 1.f) };
        cached.data.resize(sizeof(data));
        std::memcpy(cached.data.data(), &data, sizeof(data));
        cached.textures = textures;
        cached.doubleSided = doubleSided;
        cached.alphaMode = uint32_t(alphaMode);
        cached.revision = material.GetConfigurationRevision();
        cached.frame = renderer.m_materialPreparationSerial;
    }
    return { data, textures, doubleSided, alphaMode };
}

void SceneRendererDrawPreparation::ApplyMaterialRecord(
    const ResolvedMaterialRecord& material,
    const glm::vec3& cameraPosition,
    const glm::vec3& irradiance,
    ObjectGPUData& objectData)
{
    const auto& data = material.data;
    objectData.ambientUnlit = data.ambientUnlit;
    objectData.baseColor = data.baseColor;
    objectData.blinnPhongSpecularShininess = data.blinnPhongSpecularShininess;
    objectData.emissiveOcclusion = data.emissiveOcclusion;
    objectData.environmentParams = data.environmentParams;
    objectData.materialParams = data.materialParams;
    objectData.parallaxParams = data.parallaxParams;
    objectData.pmxEdgeColor = data.pmxEdgeColor;
    objectData.pmxParams = data.pmxParams;
    objectData.pmxSphereTint = data.pmxSphereTint;
    objectData.pmxTextureTint = data.pmxTextureTint;
    objectData.pmxToonTint = data.pmxToonTint;
    objectData.reflectionEnvironmentParams = data.reflectionEnvironmentParams;
    std::copy(std::begin(data.reflectionEnvironmentSH),
        std::end(data.reflectionEnvironmentSH),
        objectData.reflectionEnvironmentSH);
    objectData.subsurfaceColorWeight = data.subsurfaceColorWeight;
    objectData.subsurfaceRadiusThickness = data.subsurfaceRadiusThickness;
    objectData.subsurfaceTransmission = data.subsurfaceTransmission;
    objectData.textureUvSets0 = data.textureUvSets0;
    objectData.textureUvSets1 = data.textureUvSets1;
    objectData.viewPositionAlphaCutoff = data.viewPositionAlphaCutoff;
    objectData.ambientUnlit += glm::vec4(irradiance, 0.f);
    objectData.viewPositionAlphaCutoff = glm::vec4(cameraPosition,
        data.viewPositionAlphaCutoff.w);
}

void SceneRendererDrawPreparation::PrepareTerrainMaterial(
    SceneRenderer& renderer,
    const SceneRenderer::FrameRenderItem& item,
    PreparedDraw& draw,
    ObjectGPUData& objectData,
    bool includeEditorVisuals,
    bool forceUnlitMode,
    TerrainMaterialState& state)
{
    auto* mesh = item.mesh;
    auto* object = item.object;
    // Only terrain cells and batches carry layer materials; skip the
    // component scans for every other draw.
    if (!mesh || mesh->UsesTerrainVertexFormat() || !item.terrainSurface)
        return;
    auto* terrain = object->GetComponent<Engine::Components::Terrain>();
    auto* batch = object->GetComponent<Engine::Components::TerrainBatchAppearance>();
    if (!terrain && !batch)
        return;

    const auto& materials = terrain ? terrain->GetLayerMaterials() : batch->materials;
    bool textured = false;
    for (uint32_t slot = 0; slot < 6; ++slot)
        draw.textures[slot] = nullptr;
    // Masks: w albedo, x normal, y roughness or AO, z height blend. Slices a
    // layer lacks hold neutral values, so the masks only skip fetches.
    for (size_t layer = 0; layer < std::min<size_t>(materials.size(), 6); ++layer)
    {
        const auto* material = materials[layer].get();
        const uint32_t bit = 1u << layer;
        objectData.terrainLayerParams[layer] = {
            material ? material->roughnessFactor : 1.f,
            material ? material->normalScale : 1.f,
            material ? material->terrainHeightBlendStrength : 0.f,
            material ? material->terrainUvScale : 1.f };
        if (!material) continue;
        objectData.terrainMacroColors[layer] = glm::vec4(
            glm::length(material->terrainMacroColor) > 0.f
                ? material->terrainMacroColor : material->diffuseColor, 0.f);
        if (material->baseColorTexture) objectData.terrainMapMasks.w |= bit;
        if (material->normalTexture) objectData.terrainMapMasks.x |= bit;
        if (material->metallicRoughnessTexture || material->occlusionTexture)
            objectData.terrainMapMasks.y |= bit;
        if (material->heightTexture && material->terrainHeightBlendStrength > 0.f)
            objectData.terrainMapMasks.z |= bit;
        textured |= material->baseColorTexture || material->normalTexture ||
            material->metallicRoughnessTexture || material->occlusionTexture ||
            material->heightTexture;
    }
    // Layers are shaded from three Texture2DArrays (slots 12-14, slice = layer).
    // Until they are built the shader shows each layer's mean color.
    const auto* arrays = textured ? Engine::Components::TerrainLayerArrays::Acquire(
        materials, renderer.m_graphicsProvider) : nullptr;
    if (arrays)
    {
        draw.textures[12] = arrays->albedoHeight.get();
        draw.textures[13] = arrays->normal.get();
        draw.textures[14] = arrays->roughAo.get();
        draw.textureSlotCount = std::max(draw.textureSlotCount, 15u);
        // Measured means replace the material's flat color when no macro
        // color was authored.
        for (size_t layer = 0; layer < std::min<size_t>(materials.size(), 6); ++layer)
            if (const auto* material = materials[layer].get();
                material && glm::length(material->terrainMacroColor) <= 0.f &&
                (arrays->albedoMask & (1u << layer)))
                objectData.terrainMacroColors[layer] =
                    glm::vec4(arrays->meanAlbedo[layer], 0.f);
        // Relief depth per layer (macro color w), only where a height map exists.
        for (size_t layer = 0; layer < std::min<size_t>(materials.size(), 6); ++layer)
            if (const auto* material = materials[layer].get();
                material && (arrays->heightMask & (1u << layer)))
                objectData.terrainMacroColors[layer].w = std::max(material->heightScale, 0.f);
    }

    objectData.baseColor = glm::vec4(1.f);
    objectData.materialParams.w = textured ? 2048.f : 0.f;
    if (textured && arrays) objectData.materialParams.w += 262144.f;
    const auto* reliefManager = object->GetComponentInParent<Engine::Components::TerrainManager>();
    if (textured && arrays &&
        (reliefManager ? reliefManager->reliefMapping : (terrain && terrain->reliefMapping)))
    {
        objectData.materialParams.w += 1048576.f;
        if (reliefManager ? reliefManager->reliefShadows : (terrain && terrain->reliefShadows))
            objectData.materialParams.w += 2097152.f;
    }
    // Paint map (slot 15) replaces the vertex weights where a cell has one.
    if (terrain && textured)
        if (const auto* paint = terrain->GetPaintTexture(renderer.m_graphicsProvider))
        {
            draw.textures[15] = paint;
            draw.textureSlotCount = std::max(draw.textureSlotCount, 16u);
            objectData.materialParams.w += 524288.f;
        }
    auto* terrainManager = reliefManager;
    if (!includeEditorVisuals && !forceUnlitMode && terrainManager &&
        terrainManager->fastRuntimeLighting)
    {
        objectData.materialParams.w += batch && batch->distantVisual
            ? 16384.f : 8192.f;
        objectData.textureUvSets1.w = static_cast<float>(
            std::max(terrainManager->loadedRadius, terrainManager->farRadius)) *
            std::max(terrainManager->cellsX * terrainManager->cellWidth,
                terrainManager->cellsY * terrainManager->cellDepth);
    }
    objectData.textureUvSets1.z = terrain ? terrain->textureTiling : batch->textureTiling;
    if (terrainManager && !includeEditorVisuals)
        objectData.textureUvSets1.x = std::max(
            terrainManager->cellsX * terrainManager->cellWidth,
            terrainManager->cellsY * terrainManager->cellDepth);
    // A standalone terrain maps world X/Z over its own extent, which also
    // enables the cliff side projections.
    if (!terrainManager && terrain)
        objectData.textureUvSets1.x = std::max(terrain->cellsX * terrain->cellWidth,
            terrain->cellsY * terrain->cellDepth);
    // Hex-tiling: the manager's stochastic variation switch; always on for
    // standalone terrain.
    if (!terrainManager || terrainManager->stochasticTerrainVariation)
        objectData.materialParams.w += 65536.f;
    if (terrainManager && terrainManager->terrainMacroColorEnabled)
        objectData.materialParams.w += 131072.f;
    // Macro color map: terrain leaves material slots 0-5 empty (slot 4 holds
    // the long-range sun shadow), so the map borrows slot 5.
    if (terrainManager)
        if (const auto macro = const_cast<Engine::Components::TerrainManager*>(
                terrainManager)->GetMacroColorMap(renderer.m_graphicsProvider); macro.texture)
        {
            draw.textures[5] = macro.texture;
            draw.textureSlotCount = std::max(draw.textureSlotCount, 6u);
            // Its normals (slot 2, also unused by terrain) light distant tiles.
            if (macro.normalTexture) draw.textures[2] = macro.normalTexture;
            objectData.terrainMacroMap = macro.mapping;
            objectData.terrainMacroParams = glm::vec4(terrainManager->macroColorNear,
                std::max(terrainManager->macroColorFar, terrainManager->macroColorNear + 1.f),
                // w: 0 without normals, else 1 + canopy relief.
                1.f, macro.normalTexture
                    ? 1.f + std::clamp(terrainManager->canopyRelief, 0.f, 1.f) : 0.f);
        }
    if (!terrainManager || !terrainManager->measurementGrid)
        return;

    if (!state.measurementGridTexture)
    {
        state.measurementGridTexture = Engine::Components::Texture::Acquire(
            Engine::Components::Mesh::ResolveFilePath(
                "Assets/Textures/Terrain/measurement_grid.png"));
        state.measurementGridReady = state.measurementGridTexture &&
            state.measurementGridTexture->Prepare(renderer.m_graphicsProvider);
    }
    if (!state.measurementGridReady)
        return;
    draw.textures[0] = state.measurementGridTexture->GetGraphicsTexture();
    objectData.materialParams.w = 32768.f;
    objectData.textureUvSets1.x = terrainManager->cellsX * terrainManager->cellWidth;
    objectData.textureUvSets1.y = terrainManager->cellsY * terrainManager->cellDepth;
}
}
