#include "Core/Rendering/SceneRenderer.h"
#include "Core/Rendering/SceneRenderer/SceneRendererFramePreparation.h"

#include "Core/Rendering/RenderJobs.h"
#include "Core/Rendering/RenderUtilities.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainBatchAppearance.h"
#include "Core/Components/Animation/SkinnedMesh.h"
#include "Core/Components/Animation/Skeleton.h"
#include "Core/Components/Sprite/Sprite.h"
#include "Core/Components/Particles/ParticleEmitter.h"
#include "Core/Components/SpatialUpdate.h"
#include "Core/Components/Physics/SpatialManipulator.h"
#include "Core/Model/MeshData.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Rendering/RenderBoundsCache.h"
#include "Core/Rendering/RenderSpatialIndex.h"
#include "Core/Features/Grass/GrassInteraction.h"
#include "Core/Rendering/TextureFootprint.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <unordered_map>

namespace Engine::Rendering
{
using Scene = Engine::Scene::Scene;
namespace
{
using PrepareClock = std::chrono::steady_clock;
struct PrepareItemTimer
{
    double& milliseconds;
    bool enabled;
    PrepareClock::time_point started;
    PrepareItemTimer(double& value, bool active)
        : milliseconds(value), enabled(active),
          started(active ? PrepareClock::now() : PrepareClock::time_point{}) {}
    ~PrepareItemTimer()
    {
        if (enabled) milliseconds += std::chrono::duration<double, std::milli>(
            PrepareClock::now() - started).count();
    }
};
}

Engine::Components::Material::ShaderPrewarmEpoch SceneRendererFramePreparation::ShaderPrewarmEpoch(
    const SceneRenderer& renderer)
{
    const Scene& scene = renderer.GetScene();
    return { &scene, renderer.m_materialShaderSources.Generation(), renderer.m_materialShaderBytecodeEpoch };
}

void SceneRendererFramePreparation::PrepareItemMaterial(SceneRenderer& renderer, SceneRenderer::FrameRenderItem& item,
    bool validated, bool texturesNeeded, bool shadersNeeded)
{
    Scene& scene = renderer.GetScene();
    if (!item.material) return;
    if (!validated)
    { PrepareItemTimer timer(renderer.m_lastPrepareTimings.itemValidationMs, scene.IsUpdateProfilingEnabled());
      item.material->Validate(); }
    if (!validated || texturesNeeded)
    { PrepareItemTimer timer(renderer.m_lastPrepareTimings.itemTexturesMs, scene.IsUpdateProfilingEnabled());
      item.material->PrepareTextures(renderer.m_graphicsProvider); }
    if (!validated || shadersNeeded)
    { PrepareItemTimer timer(renderer.m_lastPrepareTimings.itemShadersMs, scene.IsUpdateProfilingEnabled());
      renderer.PrewarmMaterialShaders(*item.material, item.terrainSurface);
      if (renderer.m_graphicsProvider && renderer.m_graphicsProvider->GetShaderCompiler())
          item.material->MarkShaderPrewarmed(ShaderPrewarmEpoch(renderer), item.terrainSurface); }
    item.castsShadows = item.material->castsShadows;
    item.blended = item.material->GetAlphaMode() == Engine::Components::MaterialAlphaMode::Blend;
}

void SceneRendererFramePreparation::PrepareFrameItems(
    SceneRenderer& renderer,
    uint64_t warpRevision,
    bool hasActiveWarpVolume, bool preparedTransforms,
    std::optional<Scene::SpatialWarpSnapshot>& warpSnapshot)
{
    Scene& scene = renderer.GetScene();
    const auto& frameComponentLookupScratch = renderer.m_frameComponentLookupScratch;
    std::unordered_map<Engine::Core::Object*,
        std::vector<Engine::Components::SpatialManipulator::TraversalRenderInstance>>
        traversalRenderInstances;
    for (const SceneRenderer::FrameComponentLookup& lookup : frameComponentLookupScratch)
    {
        if (!lookup.object)
            continue;
        if (auto* manipulator = lookup.spatialManipulator)
        {
            std::vector<Engine::Components::SpatialManipulator::TraversalRenderInstance> instances;
            manipulator->AppendTraversalRenderInstances(instances);
            for (const auto& instance : instances)
            {
                if (instance.object)
                    traversalRenderInstances[instance.object].push_back(instance);
            }
        }
    }

    uint32_t skinPaletteSlot = 0;
    uint32_t boneMatrixOffset = 0;
    const auto afterLookup = scene.IsUpdateProfilingEnabled()
        ? PrepareClock::now() : PrepareClock::time_point{};
    // Validation compares ~40 public fields spread over each large Material
    // (cache misses dominate). Every lookup entry is a different object and
    // components belong to exactly one object, so each Material appears at
    // most once here and the work is independent per entry. The read-only
    // texture residency check rides along; the serial loop then queues
    // texture preparation only for materials that need it.
    // Shader prewarm also skips materials unchanged since their last prewarm
    // under the current shader sources and bytecode cache.
    enum : uint8_t { NeedsTextures = 1, NeedsShaders = 2 };
    std::vector<uint8_t> materialWork(frameComponentLookupScratch.size(), 0);
    {
        PrepareItemTimer timer(renderer.m_lastPrepareTimings.itemValidationMs, scene.IsUpdateProfilingEnabled());
        const auto* provider = renderer.m_graphicsProvider;
        const auto epoch = ShaderPrewarmEpoch(renderer);
        Engine::Rendering::RenderJobs::Get().For(frameComponentLookupScratch.size(), 256, [&](size_t index)
        {
            const auto& lookup = frameComponentLookupScratch[index];
            if (auto* material = lookup.material)
            {
                material->Validate();
                const bool terrain = lookup.terrain || lookup.terrainBatch;
                materialWork[index] = uint8_t(
                    (material->NeedsTexturePreparation(provider) ? NeedsTextures : 0) |
                    (material->NeedsShaderPrewarm(epoch, terrain) ? NeedsShaders : 0));
            }
        });
    }
    const auto lookupIndex = [&](const SceneRenderer::FrameComponentLookup& lookup)
    { return static_cast<size_t>(&lookup - frameComponentLookupScratch.data()); };
    std::unordered_map<const Engine::Components::Skeleton*,
        std::vector<glm::mat4>> jointSkinMatrices;
    bool boneDataChanged = false;
    for (const SceneRenderer::FrameComponentLookup& lookup : frameComponentLookupScratch)
    {
        Engine::Core::Object* candidate = lookup.object;
        if (!candidate)
            continue;
        Engine::Components::Mesh* mesh = lookup.mesh;
        Engine::Components::Sprite* sprite = lookup.sprite;
        const auto worldRevision = [&]
        {
            return preparedTransforms ? renderer.SceneTransforms().ReadPreparedWorldRevision(candidate->transform) : candidate->transform.GetWorldRevision();
        };
        const bool staticCandidate = mesh && !sprite && !lookup.skinnedMesh &&
            !hasActiveWarpVolume && !renderer.GetViewOverlays().previewObject &&
            (!(mesh->gpuLodSelection || mesh->screenLodSelection) || mesh->IsInstanced()) &&
            !mesh->HasMorphTargets() && !mesh->HasSurfaceOffsets() &&
            !mesh->GetPreviousRuntimeLodMesh() &&
            traversalRenderInstances.find(candidate) == traversalRenderInstances.end();
        if (staticCandidate && candidate->IsEnabledInHierarchy() &&
            mesh->IsReady() && mesh->IsEditorVisible() &&
            mesh->IsAnimationVisible())
        {
            auto cached = renderer.m_staticRenderItemCache.find(candidate);
            if (cached != renderer.m_staticRenderItemCache.end())
            {
                auto& entry = cached->second;
                const auto& layer = candidate->transform.matrixLayer;
                const bool terrainCell = lookup.terrain != nullptr;
                const auto* terrainBatch = lookup.terrainBatch;
                if (entry.item.mesh == mesh && entry.item.material == lookup.material &&
                    entry.item.spatialManipulator == lookup.spatialManipulator &&
                    entry.item.bakedLighting == lookup.bakedLighting &&
                    entry.worldRevision == worldRevision() &&
                    entry.meshRevision == mesh->GetConfigurationRevision() &&
                    entry.geometryRevision == mesh->GetAuthoredGeometryRevision() &&
                    entry.instanceData == mesh->GetInstanceData().get() &&
                    entry.instanceRevision == (mesh->GetInstanceData() ? mesh->GetInstanceData()->revision : 0) &&
                    entry.instanceSource == mesh->GetInstanceSource() &&
                    entry.sourceGeometryRevision == (mesh->GetInstanceSource() ? mesh->GetInstanceSource()->GetAuthoredGeometryRevision() : 0) &&
                    entry.item.terrainCell == terrainCell &&
                    entry.item.terrainSurface == (terrainCell || terrainBatch) &&
                    (!terrainBatch || entry.item.completeTerrainFootprint == (terrainBatch->completeFootprint && !terrainBatch->distantVisual)) &&
                    entry.matrixLayerEnabled == layer.enabled &&
                    (!layer.enabled || std::memcmp(&entry.matrixLayer, &layer.localToLayer, sizeof(layer.localToLayer)) == 0))
                {
                    SceneRenderer::FrameRenderItem item = entry.item;
                    PrepareItemMaterial(renderer, item, true,
                        (materialWork[lookupIndex(lookup)] & NeedsTextures) != 0,
                        (materialWork[lookupIndex(lookup)] & NeedsShaders) != 0);
                    item.geometryFade = mesh->GetRuntimeGeometryFade();
                    item.shadowCasterFade = { std::max(0.f, mesh->runtimeShadowDistance), std::max(0.f, mesh->runtimeShadowFadeWidth) };
                    if (mesh->IsInstanced())
                    {
                        item.gpuMaximumLod = 0;
                        item.gpuLodDistances = { 0, 0 };
                        if ((mesh->gpuLodSelection || mesh->screenLodSelection) && candidate != renderer.GetViewOverlays().selectedObject &&
                            mesh != renderer.GetViewOverlays().selectedMesh && mesh != renderer.GetViewOverlays().weightPaintMesh &&
                            mesh->IsRuntimeLodResident(0) && mesh->IsRuntimeLodResident(1) &&
                            (!item.material || Engine::Rendering::IsFastFoliageSurface(item.material) ||
                                item.material->shader.empty()))
                        {
                            item.gpuMaximumLod = mesh->IsRuntimeLodResident(2) ? 2 : 1;
                            item.gpuLodDistances = { std::max(0.f, mesh->gpuLod1Distance), std::max(mesh->gpuLod1Distance, mesh->gpuLod2Distance) };
                            if (item.gpuMaximumLod == 1) item.gpuLodDistances.y = 1.e20f;
                            item.geometryFade = { mesh->lodTransitionWidth, item.gpuLodDistances.x, item.gpuLodDistances.y, 2.f + mesh->GetRuntimeVisibility() };
                        }
                        ++renderer.m_lastPrepareTimings.instancedRenderItemsReused;
                    }
                    else ++renderer.m_lastPrepareTimings.staticRenderItemsReused;
                    renderer.m_frameRenderItems.push_back(item);
                    continue;
                }
            }
        }
        renderer.m_staticRenderItemCache.erase(candidate);
        Engine::Components::Sprite::RenderData spriteData;
        const auto meshStateStarted = scene.IsUpdateProfilingEnabled() ? PrepareClock::now() : PrepareClock::time_point{};
        const bool spriteReady = sprite && sprite->IsEditorVisible() && sprite->PrepareRenderData(renderer.m_graphicsProvider, spriteData);
        const bool renderable = spriteReady || (mesh && mesh->IsReady() && mesh->IsEditorVisible() && mesh->IsAnimationVisible());
        if (scene.IsUpdateProfilingEnabled()) renderer.m_lastPrepareTimings.itemMeshStateMs +=
            std::chrono::duration<double, std::milli>(PrepareClock::now() - meshStateStarted).count();
        if (!candidate->IsEnabledInHierarchy() || !renderable)
            continue;

        SceneRenderer::FrameRenderItem item{};
        item.object = candidate;
        item.mesh = mesh;
        if (mesh) item.shadowCasterFade = { std::max(0.f, mesh->runtimeShadowDistance), std::max(0.f, mesh->runtimeShadowFadeWidth) };
        item.sprite = sprite;
        item.material = lookup.material;
        item.spatialManipulator = lookup.spatialManipulator;
        item.skinnedMesh = lookup.skinnedMesh;
        item.bakedLighting = lookup.bakedLighting;
        item.castsShadows = !lookup.material || lookup.material->castsShadows;
        item.terrainCell = lookup.terrain != nullptr;
        item.terrainSurface = item.terrainCell;
        if (const auto* batch = lookup.terrainBatch)
        {
            item.terrainSurface = true;
            item.completeTerrainFootprint = batch->completeFootprint && !batch->distantVisual;
        }
        item.belongsToPreview = renderer.GetViewOverlays().previewObject && Engine::Rendering::IsObjectOrDescendant(candidate, renderer.GetViewOverlays().previewObject);
        glm::mat4 authoredWorld = preparedTransforms ? renderer.SceneTransforms().ReadPreparedWorldMatrix(candidate->transform) : candidate->transform.GetWorldMatrix();
        if (candidate->transform.matrixLayer.enabled)
        {
            authoredWorld = candidate->transform.matrixLayer.localToLayer * authoredWorld;
        }
        item.authoredWorld = authoredWorld;
        item.world = hasActiveWarpVolume ? scene.MapSpatialMatrix(authoredWorld, { Scene::SpatialQueryDomain::Rendering, candidate }) : authoredWorld;
        const auto splitInstances = traversalRenderInstances.find(candidate);
        const bool hasSplitRenderInstances = !sprite && splitInstances != traversalRenderInstances.end() && !splitInstances->second.empty();
        if (hasSplitRenderInstances)
        {
            item.world = splitInstances->second.front().world;
            item.traversalClipPlane = splitInstances->second.front().clipPlane;
            item.traversalChartPortal = splitInstances->second.front().chartPortal;
        }

        const bool hasSkinnedMesh = lookup.skinnedMesh != nullptr;
        if (hasActiveWarpVolume && mesh && !sprite &&
            !hasSplitRenderInstances && !hasSkinnedMesh &&
            !mesh->GetVertices().empty())
        {
            SceneRenderer::WarpedRenderMesh& cached = renderer.m_warpedRenderMeshes[candidate];
            const uint64_t meshRevision = mesh->GetConfigurationRevision();
            const bool cacheValid = cached.evaluated &&
                cached.meshRevision == meshRevision &&
                cached.warpRevision == warpRevision &&
                std::memcmp(&cached.authoredWorld, &authoredWorld,
                    sizeof(authoredWorld)) == 0;
            if (cacheValid)
            {
                if (cached.affectedByWarp && cached.vertexBuffer)
                {
                    item.warpedVertexBuffer = cached.vertexBuffer.get();
                    item.world = glm::translate(glm::mat4(1.f),
                        cached.mappedOrigin);
                }
            }
            else
            {
            if (!warpSnapshot)
                warpSnapshot.emplace(scene.CaptureSpatialWarpSnapshot());
            const glm::mat3 authoredLinear(authoredWorld);
            const float authoredDeterminant = glm::determinant(authoredLinear);
            const glm::mat3 authoredNormal =
                std::isfinite(authoredDeterminant) &&
                std::abs(authoredDeterminant) > 1e-7f
                    ? glm::transpose(glm::inverse(authoredLinear))
                    : glm::mat3(1.f);
            const Scene::SpatialQuerySample originSample = warpSnapshot->Sample(
                glm::vec3(authoredWorld[3]), candidate);
            const auto& sourceVertices = mesh->GetVertices();
            std::vector<Engine::Model::AnimationVertex> warpedVertices(
                sourceVertices.size());
            std::vector<uint8_t> affectedVertices(sourceVertices.size());
            Engine::Rendering::RenderJobs::Get().For(sourceVertices.size(), 256,
                [&](size_t index)
            {
                const auto& sourceVertex = sourceVertices[index];
                Engine::Model::AnimationVertex warpedVertex = sourceVertex;
                const glm::vec3 localPosition(sourceVertex.pos[0],
                    sourceVertex.pos[1], sourceVertex.pos[2]);
                const glm::vec3 worldPosition = glm::vec3(authoredWorld *
                    glm::vec4(localPosition, 1.f));
                const Scene::SpatialQuerySample sample = warpSnapshot->Sample(
                    worldPosition, candidate);
                affectedVertices[index] = sample.affectedByWarpVolume ? 1 : 0;

                const glm::vec3 localNormal(sourceVertex.normal[0],
                    sourceVertex.normal[1], sourceVertex.normal[2]);
                const glm::vec3 rawWorldNormal = authoredNormal * localNormal;
                const float jacobianDeterminant = glm::determinant(sample.jacobian);
                const glm::mat3 warpedNormalMatrix =
                    std::isfinite(jacobianDeterminant) &&
                    std::abs(jacobianDeterminant) > 1e-7f
                        ? glm::transpose(glm::inverse(sample.jacobian))
                        : glm::mat3(1.f);
                glm::vec3 warpedNormal = warpedNormalMatrix * rawWorldNormal;
                const float normalLength = glm::length(warpedNormal);
                warpedNormal = normalLength > 1e-6f
                    ? warpedNormal / normalLength : glm::vec3(0.f, 1.f, 0.f);

                const glm::vec3 localTangent(sourceVertex.tangent[0],
                    sourceVertex.tangent[1], sourceVertex.tangent[2]);
                glm::vec3 warpedTangent = sample.jacobian *
                    (authoredLinear * localTangent);
                warpedTangent -= warpedNormal * glm::dot(warpedTangent,
                    warpedNormal);
                const float tangentLength = glm::length(warpedTangent);
                warpedTangent = tangentLength > 1e-6f
                    ? warpedTangent / tangentLength : glm::vec3(1.f, 0.f, 0.f);

                const glm::vec3 relativePosition = sample.point - originSample.point;
                warpedVertex.pos[0] = relativePosition.x;
                warpedVertex.pos[1] = relativePosition.y;
                warpedVertex.pos[2] = relativePosition.z;
                warpedVertex.normal[0] = warpedNormal.x;
                warpedVertex.normal[1] = warpedNormal.y;
                warpedVertex.normal[2] = warpedNormal.z;
                warpedVertex.tangent[0] = warpedTangent.x;
                warpedVertex.tangent[1] = warpedTangent.y;
                warpedVertex.tangent[2] = warpedTangent.z;
                warpedVertices[index] = warpedVertex;
            });
            const bool affectedByWarp = originSample.affectedByWarpVolume ||
                std::any_of(affectedVertices.begin(), affectedVertices.end(),
                    [](uint8_t affected) { return affected != 0; });
            if (affectedByWarp)
            {
                const size_t byteSize = warpedVertices.size() *
                    sizeof(Engine::Model::AnimationVertex);
                const bool topologyChanged = cached.vertices.size() !=
                    warpedVertices.size();
                const bool contentsChanged = topologyChanged || cached.vertices.empty() ||
                    std::memcmp(cached.vertices.data(), warpedVertices.data(),
                        byteSize) != 0;
                if (contentsChanged)
                {
                    if (topologyChanged || !cached.vertexBuffer ||
                        renderer.DefersGraphicsRetirement())
                    {
                        scene.RetireGraphicsBuffer(std::move(cached.vertexBuffer));
                        cached.vertexBuffer = renderer.m_graphicsProvider->GetBufferFactory()
                            ->CreateBuffer(Engine::Graphics::IGraphicsBuffer::Usage::VertexBuffer,
                                Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
                                byteSize, warpedVertices.data());
                    }
                    else if (void* mapped = cached.vertexBuffer->Map())
                    {
                        std::memcpy(mapped, warpedVertices.data(), byteSize);
                        cached.vertexBuffer->Unmap();
                    }
                    cached.vertices = std::move(warpedVertices);
                }
                if (cached.vertexBuffer)
                {
                    item.warpedVertexBuffer = cached.vertexBuffer.get();
                    item.world = glm::translate(glm::mat4(1.f),
                        originSample.point);
                }
            }
            else
            {
                cached.vertexBuffer.reset();
                cached.vertices.clear();
            }
            cached.authoredWorld = authoredWorld;
            cached.mappedOrigin = originSample.point;
            cached.meshRevision = meshRevision;
            cached.warpRevision = warpRevision;
            cached.affectedByWarp = affectedByWarp;
            cached.evaluated = true;
            }
        }

        if (sprite)
        {
            item.spriteVertexBuffer = spriteData.vertexBuffer;
            item.spriteTexture = spriteData.texture;
            item.spriteWorldSize = spriteData.worldSize;
            item.spriteUvRect = spriteData.uvRect;
            item.sortingLayer = sprite->sortingLayer;
            item.blended = true;
        }
        else if (item.material)
        {
            PrepareItemMaterial(renderer, item, true,
                        (materialWork[lookupIndex(lookup)] & NeedsTextures) != 0,
                        (materialWork[lookupIndex(lookup)] & NeedsShaders) != 0);
        }
        item.blended = item.belongsToPreview || item.blended;

        // Pack complete palettes by their actual size, including large rigs.
        {
            if (mesh && !mesh->IsPortalPoseSnapshot())
            if (Engine::Components::SkinnedMesh* skinned = lookup.skinnedMesh)
            {
                const auto* skeleton = skinned->ResolveSkeleton();
                const std::vector<glm::mat4>* sharedJoints = nullptr;
                if (skeleton)
                {
                    auto [found, inserted] = jointSkinMatrices.try_emplace(skeleton);
                    if (inserted)
                    {
                        const auto& joints = skeleton->ResolveJoints();
                        auto& matrices = found->second;
                        matrices.assign(skeleton->jointNodes.size(), glm::mat4(1.f));
                        for (size_t i = 0; i < std::min(joints.size(),
                                matrices.size()); ++i)
                            if (auto* joint = joints[i])
                                matrices[i] = (preparedTransforms ? renderer.SceneTransforms().ReadPreparedWorldMatrix(joint->transform):joint->transform.GetWorldMatrix()) *
                                    (i < skeleton->inverseBindMatrices.size()
                                        ? skeleton->inverseBindMatrices[i]
                                        : glm::mat4(1.f));
                    }
                    sharedJoints = &found->second;
                }
                const std::vector<glm::mat4>& palette =
                    sharedJoints ? skinned->BuildPalette(*sharedJoints) :
                        skinned->BuildPalette();
                if (!palette.empty())
                {
                    const size_t count = palette.size();
                    if (count > renderer.m_boneMatrixCapacity - boneMatrixOffset)
                        throw std::runtime_error("Bone palette capacity changed during frame preparation");
                    item.skinPaletteOffset = boneMatrixOffset;
                    item.skinJointCount = static_cast<uint32_t>(count);
                    std::memcpy(
                        static_cast<glm::mat4*>(renderer.m_boneDataMapped) +
                            item.skinPaletteOffset,
                        palette.data(), count * sizeof(glm::mat4));
                    ++skinPaletteSlot;
                    boneMatrixOffset += static_cast<uint32_t>(count);
                    boneDataChanged = true;
                }
            }
        }

        const auto appendGeometry = [&](SceneRenderer::FrameRenderItem draw) {
            PrepareItemTimer lodTimer(renderer.m_lastPrepareTimings.itemLodMs, scene.IsUpdateProfilingEnabled());
            Engine::Components::Mesh* source = draw.mesh;
            const auto publish = [&](SceneRenderer::FrameRenderItem& current) {
                PrepareItemTimer boundsTimer(renderer.m_lastPrepareTimings.itemBoundsMs, scene.IsUpdateProfilingEnabled());
                current.hasBounds = current.mesh && current.mesh->HasBounds();
                current.hasMorphTargets = current.mesh && current.mesh->HasMorphTargets();
                current.hasSurfaceOffsets = current.mesh && current.mesh->HasSurfaceOffsets();
                if (current.hasBounds)
                {
                    current.boundsMinimum = current.mesh->GetBoundsMin();
                    current.boundsMaximum = current.mesh->GetBoundsMax();
                    // Every resident LOD of one ordinary object shares the
                    // root's conservative bounds so the BVH and shadow
                    // cascades cannot reject a representation that extends
                    // beyond the currently selected mesh.
                    if (source && !source->IsInstanced() && current.mesh != source)
                        source->GetRuntimeLodAggregateBounds(
                            current.boundsMinimum, current.boundsMaximum);
                }
                renderer.m_frameRenderItems.push_back(current);
            };
            if (source && !draw.warpedVertexBuffer)
                draw.geometryFade = source->GetRuntimeGeometryFade();
            if(source && source->IsInstanced() && (source->gpuLodSelection || source->screenLodSelection) &&
                draw.object!=renderer.GetViewOverlays().selectedObject && source!=renderer.GetViewOverlays().selectedMesh && source!=renderer.GetViewOverlays().weightPaintMesh &&
                !draw.warpedVertexBuffer && !draw.traversalChartPortal && source->IsRuntimeLodResident(0) && source->IsRuntimeLodResident(1) &&
                (!draw.material || IsFastFoliageSurface(draw.material) ||
                    draw.material->shader.empty()))
            {
                draw.gpuMaximumLod=source->IsRuntimeLodResident(2) ? 2:1;
                draw.gpuLodDistances={std::max(0.f,source->gpuLod1Distance),std::max(source->gpuLod1Distance,source->gpuLod2Distance)};
                if(draw.gpuMaximumLod==1) draw.gpuLodDistances.y=1.e20f;
                draw.geometryFade={source->lodTransitionWidth,draw.gpuLodDistances.x,draw.gpuLodDistances.y,2.f+source->GetRuntimeVisibility()};
                publish(draw);
                return;
            }
            if (source && (source->gpuLodSelection || source->screenLodSelection) && draw.object!=renderer.GetViewOverlays().selectedObject &&
                source!=renderer.GetViewOverlays().selectedMesh && source!=renderer.GetViewOverlays().weightPaintMesh && !source->IsInstanced() &&
                !source->HasMorphTargets() && !source->HasSurfaceOffsets() && !draw.skinJointCount &&
                !draw.warpedVertexBuffer && !draw.traversalChartPortal && source->IsRuntimeLodResident(0) &&
                source->IsRuntimeLodResident(1))
            {
                const int maximumLod = source->IsRuntimeLodResident(2) ? 2 : 1;
                // One object, one Material: each level draws with that
                // material's lodMaterials entry (or the material itself).
                Engine::Components::Material* const baseMaterial = draw.material;
                const bool baseCastsShadows = draw.castsShadows;
                const bool baseBlended = draw.blended;
                for (int lod=0;lod<=maximumLod;++lod)
                {
                    draw.material = baseMaterial ? baseMaterial->GetLodMaterial(lod) : nullptr;
                    if (draw.material != baseMaterial) PrepareItemMaterial(renderer, draw);
                    else { draw.castsShadows = baseCastsShadows; draw.blended = baseBlended; }
                    draw.mesh=source->GetRuntimeLodMesh(lod);
                    draw.mesh->useLightProbes=source->useLightProbes;
                    draw.mesh->usesGrassShadowBudget=source->usesGrassShadowBudget;
                    draw.mesh->useBoundsForShadowCulling=source->useBoundsForShadowCulling;
                    draw.mesh->deformationBoundsPadding=source->deformationBoundsPadding;
                    draw.gpuLod=lod; draw.gpuMaximumLod=maximumLod;
                    draw.gpuLodDistances={std::max(0.f,source->gpuLod1Distance),
                        std::max(source->gpuLod1Distance,source->gpuLod2Distance)};
                    if(source->IsInstanced() && maximumLod==1) draw.gpuLodDistances.y=1.e20f;
                    if(source->IsInstanced()) draw.geometryFade={source->lodTransitionWidth,draw.gpuLodDistances.x,draw.gpuLodDistances.y,2.f+source->GetRuntimeVisibility()};
                    publish(draw);
                }
                return;
            }
            publish(draw);
            if (source && !draw.warpedVertexBuffer)
                if (auto* previous = source->GetPreviousRuntimeLodMesh())
                {
                    draw.mesh = previous;
                    draw.previousLod=true;
                    previous->usesGrassShadowBudget=source->usesGrassShadowBudget;
                    previous->deformationBoundsPadding=source->deformationBoundsPadding;
                    draw.geometryFade = source->GetRuntimeGeometryFade(true);
                    publish(draw);
                }
        };
        const size_t firstDraw = renderer.m_frameRenderItems.size();
        appendGeometry(item);
        if (hasSplitRenderInstances)
        {
            for (size_t index = 1u; index < traversalRenderInstances[candidate].size(); ++index)
            {
                SceneRenderer::FrameRenderItem remoteItem = item;
                remoteItem.world = traversalRenderInstances[candidate][index].world;
                remoteItem.traversalClipPlane = traversalRenderInstances[candidate][index].clipPlane;
                remoteItem.traversalChartPortal = traversalRenderInstances[candidate][index].chartPortal;
                appendGeometry(remoteItem);
            }
        }
        if (staticCandidate && renderer.m_frameRenderItems.size() == firstDraw + 1 && !item.warpedVertexBuffer && !hasSplitRenderInstances && !mesh->HasMorphTargets() && !mesh->HasSurfaceOffsets())
        {
            const auto& layer = candidate->transform.matrixLayer;
            renderer.m_staticRenderItemCache[candidate] = {
                renderer.m_frameRenderItems.back(), worldRevision(),
                mesh->GetConfigurationRevision(), mesh->GetAuthoredGeometryRevision(),
                layer.enabled, layer.localToLayer,
                mesh->GetInstanceData().get(), mesh->GetInstanceData() ? mesh->GetInstanceData()->revision : 0,
                mesh->GetInstanceSource(), mesh->GetInstanceSource() ? mesh->GetInstanceSource()->GetAuthoredGeometryRevision() : 0 };
            if (mesh->IsInstanced()) ++renderer.m_lastPrepareTimings.instancedRenderItemsUpdated;
            else ++renderer.m_lastPrepareTimings.staticRenderItemsUpdated;
        }
    }

    if (boneDataChanged)
    {
        renderer.m_boneDataBuffer->FlushMappedWrites();
        renderer.RecordFrameUploadBytes(uint64_t(boneMatrixOffset) * sizeof(glm::mat4));
    }
    const auto afterBoneUpload = scene.IsUpdateProfilingEnabled()
        ? PrepareClock::now() : PrepareClock::time_point{};
    renderer.m_lastSkinnedObjectCount = skinPaletteSlot;
    if (renderer.m_frameRenderItems.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Prepared draw count exceeds renderer index range");
    if(renderer.m_renderObjectSlotsRevision!=scene.GetStructureRevision())
    {
        std::unordered_set<uintptr_t> owners,charts;
        for(const auto& object:renderer.GetScene().GetObjects()) owners.insert(uintptr_t(object.get()));
        for(auto* chart:renderer.m_frameSpatialManipulatorScratch) charts.insert(uintptr_t(chart));
        for(auto it=renderer.m_renderObjectSlots.begin();it!=renderer.m_renderObjectSlots.end();)
            if(!owners.count(std::get<0>(it->first)) || (std::get<2>(it->first) && !charts.count(std::get<2>(it->first))))
            { renderer.m_freeRenderObjectSlots.push_back(it->second); it=renderer.m_renderObjectSlots.erase(it); }
            else ++it;
        renderer.m_renderObjectSlotsRevision=scene.GetStructureRevision();
    }
    renderer.m_frameItemsByRenderSlotOffsets.assign(
        size_t(renderer.m_renderObjectSlotCount) + 1u, 0u);
    for(auto& item:renderer.m_frameRenderItems)
    {
        SceneRenderer::RenderObjectKey key{uintptr_t(item.object),item.gpuLod,uintptr_t(item.traversalChartPortal),item.previousLod};
        auto found=renderer.m_renderObjectSlots.find(key);
        if(found==renderer.m_renderObjectSlots.end())
        {
            uint32_t index=renderer.m_renderObjectSlotCount;
            if(renderer.m_freeRenderObjectSlots.empty()) ++renderer.m_renderObjectSlotCount;
            else { index=renderer.m_freeRenderObjectSlots.back(); renderer.m_freeRenderObjectSlots.pop_back(); }
            found=renderer.m_renderObjectSlots.emplace(key,index).first;
        }
        item.objectSlot=found->second;
        // New render keys can grow the slot count while this frame is being
        // packed. Keep the offsets array large enough for every slot before
        // counting items into its [slot + 1] bucket.
        if (renderer.m_frameItemsByRenderSlotOffsets.size() <=
            size_t(item.objectSlot) + 1u)
            renderer.m_frameItemsByRenderSlotOffsets.resize(
                size_t(item.objectSlot) + 2u, 0u);
        ++renderer.m_frameItemsByRenderSlotOffsets[size_t(item.objectSlot) + 1u];
    }
    std::partial_sum(renderer.m_frameItemsByRenderSlotOffsets.begin(),
        renderer.m_frameItemsByRenderSlotOffsets.end(),
        renderer.m_frameItemsByRenderSlotOffsets.begin());
    renderer.m_frameItemsByRenderSlot.resize(renderer.m_frameRenderItems.size());
    auto nextFrameItem = renderer.m_frameItemsByRenderSlotOffsets;
    for (uint32_t index = 0; index < renderer.m_frameRenderItems.size(); ++index)
    {
        const uint32_t slot = renderer.m_frameRenderItems[index].objectSlot;
        renderer.m_frameItemsByRenderSlot[nextFrameItem[slot]++] = index;
    }
    renderer.EnsureObjectRenderCapacity(renderer.m_renderObjectSlotCount);
    if (scene.IsUpdateProfilingEnabled())
    {
        const auto ms = [](auto first, auto last)
        {
            return std::chrono::duration<double, std::milli>(last - first).count();
        };
        renderer.m_lastPrepareTimings.itemsMs = ms(afterLookup, PrepareClock::now());
    }
}

void SceneRendererFramePreparation::PrepareSpatialBounds(SceneRenderer& renderer)
{
    const auto spatialStart = std::chrono::steady_clock::now();
    std::vector<Engine::Rendering::RenderBounds> spatialBounds;
    std::vector<Engine::Rendering::RenderBoundsCache::Input> boundsInput;
    boundsInput.resize(renderer.m_frameRenderItems.size());
    Engine::Rendering::RenderJobs::Get().For(renderer.m_frameRenderItems.size(), 128, [&](size_t index)
        {
            const auto& item = renderer.m_frameRenderItems[index];
            Engine::Rendering::RenderBoundsCache::Input input;
            input.slot = item.objectSlot; input.world = item.world;
            if (item.hasBounds && !item.skinJointCount && !item.hasMorphTargets && !item.hasSurfaceOffsets && !item.warpedVertexBuffer && !item.traversalChartPortal)
            {
                input.bounded = true; input.minimum = item.boundsMinimum; input.maximum = item.boundsMaximum;
            }
            boundsInput[index] = input;
        });
    const auto bounds = renderer.m_worldBoundsCache.Prepare(boundsInput, spatialBounds);
    renderer.m_lastPrepareTimings.boundsReused = bounds.reused; renderer.m_lastPrepareTimings.boundsUpdated = bounds.updated;
    renderer.m_spatialSlotBounds.resize(renderer.m_renderObjectSlotCount);
    renderer.m_spatialSlotSeen.assign(renderer.m_renderObjectSlotCount, 0);
    for (size_t index = 0; index < renderer.m_frameRenderItems.size(); ++index)
    {
        const auto slot = renderer.m_frameRenderItems[index].objectSlot;
        const auto& current = spatialBounds[index];
        if (!renderer.m_spatialSlotSeen[slot])
        {
            renderer.m_spatialSlotBounds[slot] = current; renderer.m_spatialSlotSeen[slot] = 1;
        }
        else
        {
            auto& aggregate = renderer.m_spatialSlotBounds[slot];
            if (!aggregate.bounded || !current.bounded) aggregate.bounded = false;
            else
            {
                aggregate.minimum = glm::min(aggregate.minimum, current.minimum);
                aggregate.maximum = glm::max(aggregate.maximum, current.maximum);
            }
        }
    }
    renderer.m_lastPrepareTimings.spatialActiveSlots = renderer.m_frameRenderItems.size();
    renderer.m_lastPrepareTimings.spatialRetainedSlots = renderer.m_spatialSlotBounds.size();
    const auto spatial = renderer.m_renderSpatialIndex.Update(renderer.m_spatialSlotBounds);
    renderer.m_lastPrepareTimings.spatialIndexMs = spatial.totalMs; renderer.m_lastPrepareTimings.spatialCompareMs = spatial.compareMs;
    renderer.m_lastPrepareTimings.spatialRefitMs = spatial.refitMs; renderer.m_lastPrepareTimings.spatialBuildMs = spatial.buildMs;
    renderer.m_lastPrepareTimings.spatialChangedBounds = spatial.changedBounds; renderer.m_lastPrepareTimings.spatialReusedBounds = spatial.reusedBounds;
    renderer.m_lastPrepareTimings.spatialDirtyLeaves = spatial.dirtyLeaves; renderer.m_lastPrepareTimings.spatialRefitNodes = spatial.refitNodes;
    renderer.m_lastPrepareTimings.spatialRebuilds = spatial.rebuilds; renderer.m_lastPrepareTimings.spatialQualityRebuilds = spatial.qualityRebuilds;
    renderer.m_lastPrepareTimings.spatialMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - spatialStart).count();
}

}
