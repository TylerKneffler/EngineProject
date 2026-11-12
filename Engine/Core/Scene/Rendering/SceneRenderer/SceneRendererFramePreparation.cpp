#include "Core/Scene/Rendering/SceneRenderer/SceneRendererFramePreparation.h"

#include "Core/Rendering/RenderJobs.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Terrain/Terrain.h"
#include "Core/Compoonents/Terrain/TerrainBatchAppearance.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Sprite/Sprite.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/SpatialUpdate.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Model/MeshData.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Rendering/RenderBoundsCache.h"
#include "Core/Rendering/RenderSpatialIndex.h"
#include "Core/Rendering/GrassInteraction.h"
#include "Core/Rendering/TextureFootprint.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <numeric>
#include <optional>
#include <unordered_map>

namespace Engine::Scene::Renderer
{
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

void SceneRendererFramePreparation::PrepareItemMaterial(Scene& scene, Scene::FrameRenderItem& item)
{
    if (!item.material) return;
    { PrepareItemTimer timer(scene.m_lastPrepareTimings.itemValidationMs, scene.m_updateProfilingEnabled);
      item.material->Validate(); }
    { PrepareItemTimer timer(scene.m_lastPrepareTimings.itemTexturesMs, scene.m_updateProfilingEnabled);
      item.material->PrepareTextures(scene.m_graphicsProvider); }
    { PrepareItemTimer timer(scene.m_lastPrepareTimings.itemShadersMs, scene.m_updateProfilingEnabled);
      scene.PrewarmMaterialShaders(*item.material, item.terrainSurface); }
    item.castsShadows = item.material->castsShadows;
    item.blended = item.material->GetAlphaMode() == Engine::Components::MaterialAlphaMode::Blend;
}

void SceneRendererFramePreparation::PrepareFrameItems(
    Scene& scene,
    uint64_t warpRevision,
    bool hasActiveWarpVolume, bool preparedTransforms,
    std::optional<Scene::SpatialWarpSnapshot>& warpSnapshot)
{
    const auto& frameComponentLookupScratch = scene.m_frameComponentLookupScratch;
    std::unordered_map<Engine::Core::Object*,
        std::vector<Engine::Components::SpatialManipulator::TraversalRenderInstance>>
        traversalRenderInstances;
    for (const Scene::FrameComponentLookup& lookup : frameComponentLookupScratch)
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
    const auto afterLookup = scene.m_updateProfilingEnabled
        ? PrepareClock::now() : PrepareClock::time_point{};
    std::unordered_map<const Engine::Components::Skeleton*,
        std::vector<glm::mat4>> jointSkinMatrices;
    bool boneDataChanged = false;
    for (const Scene::FrameComponentLookup& lookup : frameComponentLookupScratch)
    {
        Engine::Core::Object* candidate = lookup.object;
        if (!candidate)
            continue;
        Engine::Components::Mesh* mesh = lookup.mesh;
        Engine::Components::Sprite* sprite = lookup.sprite;
        const auto worldRevision = [&]
        {
            return preparedTransforms ? scene.m_transformHierarchy.ReadPreparedWorldRevision(candidate->transform) : candidate->transform.GetWorldRevision();
        };
        const bool staticCandidate = mesh && !sprite && !lookup.skinnedMesh &&
            !hasActiveWarpVolume && !scene.m_previewObject &&
            (!(mesh->gpuLodSelection || mesh->screenLodSelection) || mesh->IsInstanced()) &&
            !mesh->HasMorphTargets() && !mesh->HasSurfaceOffsets() &&
            !mesh->GetPreviousRuntimeLodMesh() &&
            traversalRenderInstances.find(candidate) == traversalRenderInstances.end();
        if (staticCandidate && candidate->IsEnabledInHierarchy() &&
            mesh->IsReady() && mesh->IsEditorVisible() &&
            mesh->IsAnimationVisible())
        {
            auto cached = scene.m_staticRenderItemCache.find(candidate);
            if (cached != scene.m_staticRenderItemCache.end())
            {
                auto& entry = cached->second;
                const auto& layer = candidate->transform.matrixLayer;
                const bool terrainCell = candidate->GetComponent<Engine::Components::Terrain>() != nullptr;
                const auto* terrainBatch = candidate->GetComponent<Engine::Components::TerrainBatchAppearance>();
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
                    Scene::FrameRenderItem item = entry.item;
                    PrepareItemMaterial(scene, item);
                    item.geometryFade = mesh->GetRuntimeGeometryFade();
                    item.shadowCasterFade = { std::max(0.f, mesh->runtimeShadowDistance), std::max(0.f, mesh->runtimeShadowFadeWidth) };
                    if (mesh->IsInstanced())
                    {
                        item.gpuMaximumLod = 0;
                        item.gpuLodDistances = { 0, 0 };
                        if ((mesh->gpuLodSelection || mesh->screenLodSelection) && candidate != scene.m_selectedObject &&
                            mesh != scene.m_editorSelectedMesh && mesh != scene.m_editorWeightPaintMesh &&
                            mesh->IsRuntimeLodResident(0) && mesh->IsRuntimeLodResident(1) &&
                            (!item.material || Engine::Scene::Renderer::IsFastFoliageSurface(item.material) ||
                                item.material->shader.empty()))
                        {
                            item.gpuMaximumLod = mesh->IsRuntimeLodResident(2) ? 2 : 1;
                            item.gpuLodDistances = { std::max(0.f, mesh->gpuLod1Distance), std::max(mesh->gpuLod1Distance, mesh->gpuLod2Distance) };
                            if (item.gpuMaximumLod == 1) item.gpuLodDistances.y = 1.e20f;
                            item.geometryFade = { mesh->lodTransitionWidth, item.gpuLodDistances.x, item.gpuLodDistances.y, 2.f + mesh->GetRuntimeVisibility() };
                        }
                        ++scene.m_lastPrepareTimings.instancedRenderItemsReused;
                    }
                    else ++scene.m_lastPrepareTimings.staticRenderItemsReused;
                    scene.m_frameRenderItems.push_back(item);
                    continue;
                }
            }
        }
        scene.m_staticRenderItemCache.erase(candidate);
        Engine::Components::Sprite::RenderData spriteData;
        const auto meshStateStarted = scene.m_updateProfilingEnabled ? PrepareClock::now() : PrepareClock::time_point{};
        const bool spriteReady = sprite && sprite->IsEditorVisible() && sprite->PrepareRenderData(scene.m_graphicsProvider, spriteData);
        const bool renderable = spriteReady || (mesh && mesh->IsReady() && mesh->IsEditorVisible() && mesh->IsAnimationVisible());
        if (scene.m_updateProfilingEnabled) scene.m_lastPrepareTimings.itemMeshStateMs +=
            std::chrono::duration<double, std::milli>(PrepareClock::now() - meshStateStarted).count();
        if (!candidate->IsEnabledInHierarchy() || !renderable)
            continue;

        Scene::FrameRenderItem item{};
        item.object = candidate;
        item.mesh = mesh;
        if (mesh) item.shadowCasterFade = { std::max(0.f, mesh->runtimeShadowDistance), std::max(0.f, mesh->runtimeShadowFadeWidth) };
        item.sprite = sprite;
        item.material = lookup.material;
        item.spatialManipulator = lookup.spatialManipulator;
        item.skinnedMesh = lookup.skinnedMesh;
        item.bakedLighting = lookup.bakedLighting;
        item.castsShadows = !lookup.material || lookup.material->castsShadows;
        item.terrainCell = candidate->GetComponent<Engine::Components::Terrain>() != nullptr;
        item.terrainSurface = item.terrainCell;
        if (const auto* batch = candidate->GetComponent<Engine::Components::TerrainBatchAppearance>())
        {
            item.terrainSurface = true;
            item.completeTerrainFootprint = batch->completeFootprint && !batch->distantVisual;
        }
        item.belongsToPreview = scene.m_previewObject && Engine::Scene::Renderer::IsObjectOrDescendant(candidate, scene.m_previewObject);
        glm::mat4 authoredWorld = preparedTransforms ? scene.m_transformHierarchy.ReadPreparedWorldMatrix(candidate->transform) : candidate->transform.GetWorldMatrix();
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
            Scene::WarpedRenderMesh& cached = scene.m_warpedRenderMeshes[candidate];
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
                        scene.m_deferGraphicsRetirement)
                    {
                        scene.RetireGraphicsBuffer(std::move(cached.vertexBuffer));
                        cached.vertexBuffer = scene.m_graphicsProvider->GetBufferFactory()
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
            PrepareItemMaterial(scene, item);
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
                                matrices[i] = (preparedTransforms ? scene.m_transformHierarchy.ReadPreparedWorldMatrix(joint->transform):joint->transform.GetWorldMatrix()) *
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
                    if (count > scene.m_boneMatrixCapacity - boneMatrixOffset)
                        throw std::runtime_error("Bone palette capacity changed during frame preparation");
                    item.skinPaletteOffset = boneMatrixOffset;
                    item.skinJointCount = static_cast<uint32_t>(count);
                    std::memcpy(
                        static_cast<glm::mat4*>(scene.m_boneDataMapped) +
                            item.skinPaletteOffset,
                        palette.data(), count * sizeof(glm::mat4));
                    ++skinPaletteSlot;
                    boneMatrixOffset += static_cast<uint32_t>(count);
                    boneDataChanged = true;
                }
            }
        }

        const auto appendGeometry = [&](Scene::FrameRenderItem draw) {
            PrepareItemTimer lodTimer(scene.m_lastPrepareTimings.itemLodMs, scene.m_updateProfilingEnabled);
            Engine::Components::Mesh* source = draw.mesh;
            const auto publish = [&](Scene::FrameRenderItem& current) {
                PrepareItemTimer boundsTimer(scene.m_lastPrepareTimings.itemBoundsMs, scene.m_updateProfilingEnabled);
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
                scene.m_frameRenderItems.push_back(current);
            };
            if (source && !draw.warpedVertexBuffer)
                draw.geometryFade = source->GetRuntimeGeometryFade();
            if(source && source->IsInstanced() && (source->gpuLodSelection || source->screenLodSelection) &&
                draw.object!=scene.m_selectedObject && source!=scene.m_editorSelectedMesh && source!=scene.m_editorWeightPaintMesh &&
                !draw.warpedVertexBuffer && !draw.traversalChartPortal && source->IsRuntimeLodResident(0) && source->IsRuntimeLodResident(1) &&
                (!draw.material || Renderer::IsFastFoliageSurface(draw.material) ||
                    draw.material->shader.empty()))
            {
                draw.gpuMaximumLod=source->IsRuntimeLodResident(2) ? 2:1;
                draw.gpuLodDistances={std::max(0.f,source->gpuLod1Distance),std::max(source->gpuLod1Distance,source->gpuLod2Distance)};
                if(draw.gpuMaximumLod==1) draw.gpuLodDistances.y=1.e20f;
                draw.geometryFade={source->lodTransitionWidth,draw.gpuLodDistances.x,draw.gpuLodDistances.y,2.f+source->GetRuntimeVisibility()};
                publish(draw);
                return;
            }
            if (source && (source->gpuLodSelection || source->screenLodSelection) && draw.object!=scene.m_selectedObject &&
                source!=scene.m_editorSelectedMesh && source!=scene.m_editorWeightPaintMesh && !source->IsInstanced() &&
                !source->HasMorphTargets() && !source->HasSurfaceOffsets() && !draw.skinJointCount &&
                !draw.warpedVertexBuffer && !draw.traversalChartPortal && source->IsRuntimeLodResident(0) &&
                source->IsRuntimeLodResident(1))
            {
                const int maximumLod = source->IsRuntimeLodResident(2) ? 2 : 1;
                for (int lod=0;lod<=maximumLod;++lod)
                {
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
        const size_t firstDraw = scene.m_frameRenderItems.size();
        appendGeometry(item);
        if (hasSplitRenderInstances)
        {
            for (size_t index = 1u; index < traversalRenderInstances[candidate].size(); ++index)
            {
                Scene::FrameRenderItem remoteItem = item;
                remoteItem.world = traversalRenderInstances[candidate][index].world;
                remoteItem.traversalClipPlane = traversalRenderInstances[candidate][index].clipPlane;
                remoteItem.traversalChartPortal = traversalRenderInstances[candidate][index].chartPortal;
                appendGeometry(remoteItem);
            }
        }
        if (staticCandidate && scene.m_frameRenderItems.size() == firstDraw + 1 && !item.warpedVertexBuffer && !hasSplitRenderInstances && !mesh->HasMorphTargets() && !mesh->HasSurfaceOffsets())
        {
            const auto& layer = candidate->transform.matrixLayer;
            scene.m_staticRenderItemCache[candidate] = {
                scene.m_frameRenderItems.back(), worldRevision(),
                mesh->GetConfigurationRevision(), mesh->GetAuthoredGeometryRevision(),
                layer.enabled, layer.localToLayer,
                mesh->GetInstanceData().get(), mesh->GetInstanceData() ? mesh->GetInstanceData()->revision : 0,
                mesh->GetInstanceSource(), mesh->GetInstanceSource() ? mesh->GetInstanceSource()->GetAuthoredGeometryRevision() : 0 };
            if (mesh->IsInstanced()) ++scene.m_lastPrepareTimings.instancedRenderItemsUpdated;
            else ++scene.m_lastPrepareTimings.staticRenderItemsUpdated;
        }
    }

    if (boneDataChanged)
    {
        scene.m_boneDataBuffer->FlushMappedWrites();
        scene.RecordFrameUploadBytes(uint64_t(boneMatrixOffset) * sizeof(glm::mat4));
    }
    const auto afterBoneUpload = scene.m_updateProfilingEnabled
        ? PrepareClock::now() : PrepareClock::time_point{};
    scene.m_lastSkinnedObjectCount = skinPaletteSlot;
    if (scene.m_frameRenderItems.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Prepared draw count exceeds renderer index range");
    if(scene.m_renderObjectSlotsRevision!=scene.GetStructureRevision())
    {
        std::unordered_set<uintptr_t> owners,charts;
        for(const auto& object:scene.m_objects) owners.insert(uintptr_t(object.get()));
        for(auto* chart:scene.m_frameSpatialManipulatorScratch) charts.insert(uintptr_t(chart));
        for(auto it=scene.m_renderObjectSlots.begin();it!=scene.m_renderObjectSlots.end();)
            if(!owners.count(std::get<0>(it->first)) || (std::get<2>(it->first) && !charts.count(std::get<2>(it->first))))
            { scene.m_freeRenderObjectSlots.push_back(it->second); it=scene.m_renderObjectSlots.erase(it); }
            else ++it;
        scene.m_renderObjectSlotsRevision=scene.GetStructureRevision();
    }
    scene.m_frameItemsByRenderSlotOffsets.assign(
        size_t(scene.m_renderObjectSlotCount) + 1u, 0u);
    for(auto& item:scene.m_frameRenderItems)
    {
        Scene::RenderObjectKey key{uintptr_t(item.object),item.gpuLod,uintptr_t(item.traversalChartPortal),item.previousLod};
        auto found=scene.m_renderObjectSlots.find(key);
        if(found==scene.m_renderObjectSlots.end())
        {
            uint32_t index=scene.m_renderObjectSlotCount;
            if(scene.m_freeRenderObjectSlots.empty()) ++scene.m_renderObjectSlotCount;
            else { index=scene.m_freeRenderObjectSlots.back(); scene.m_freeRenderObjectSlots.pop_back(); }
            found=scene.m_renderObjectSlots.emplace(key,index).first;
        }
        item.objectSlot=found->second;
        // New render keys can grow the slot count while this frame is being
        // packed. Keep the offsets array large enough for every slot before
        // counting items into its [slot + 1] bucket.
        if (scene.m_frameItemsByRenderSlotOffsets.size() <=
            size_t(item.objectSlot) + 1u)
            scene.m_frameItemsByRenderSlotOffsets.resize(
                size_t(item.objectSlot) + 2u, 0u);
        ++scene.m_frameItemsByRenderSlotOffsets[size_t(item.objectSlot) + 1u];
    }
    std::partial_sum(scene.m_frameItemsByRenderSlotOffsets.begin(),
        scene.m_frameItemsByRenderSlotOffsets.end(),
        scene.m_frameItemsByRenderSlotOffsets.begin());
    scene.m_frameItemsByRenderSlot.resize(scene.m_frameRenderItems.size());
    auto nextFrameItem = scene.m_frameItemsByRenderSlotOffsets;
    for (uint32_t index = 0; index < scene.m_frameRenderItems.size(); ++index)
    {
        const uint32_t slot = scene.m_frameRenderItems[index].objectSlot;
        scene.m_frameItemsByRenderSlot[nextFrameItem[slot]++] = index;
    }
    scene.EnsureObjectRenderCapacity(scene.m_renderObjectSlotCount);
    if (scene.m_updateProfilingEnabled)
    {
        const auto ms = [](auto first, auto last)
        {
            return std::chrono::duration<double, std::milli>(last - first).count();
        };
        scene.m_lastPrepareTimings.itemsMs = ms(afterLookup, PrepareClock::now());
    }
}

void SceneRendererFramePreparation::PrepareSpatialBounds(Scene& scene)
{
    const auto spatialStart = std::chrono::steady_clock::now();
    std::vector<Engine::Rendering::RenderBounds> spatialBounds;
    std::vector<Engine::Rendering::RenderBoundsCache::Input> boundsInput;
    boundsInput.resize(scene.m_frameRenderItems.size());
    Engine::Rendering::RenderJobs::Get().For(scene.m_frameRenderItems.size(), 128, [&](size_t index)
        {
            const auto& item = scene.m_frameRenderItems[index];
            Engine::Rendering::RenderBoundsCache::Input input;
            input.slot = item.objectSlot; input.world = item.world;
            if (item.hasBounds && !item.skinJointCount && !item.hasMorphTargets && !item.hasSurfaceOffsets && !item.warpedVertexBuffer && !item.traversalChartPortal)
            {
                input.bounded = true; input.minimum = item.boundsMinimum; input.maximum = item.boundsMaximum;
            }
            boundsInput[index] = input;
        });
    const auto bounds = scene.m_worldBoundsCache.Prepare(boundsInput, spatialBounds);
    scene.m_lastPrepareTimings.boundsReused = bounds.reused; scene.m_lastPrepareTimings.boundsUpdated = bounds.updated;
    scene.m_spatialSlotBounds.resize(scene.m_renderObjectSlotCount);
    scene.m_spatialSlotSeen.assign(scene.m_renderObjectSlotCount, 0);
    for (size_t index = 0; index < scene.m_frameRenderItems.size(); ++index)
    {
        const auto slot = scene.m_frameRenderItems[index].objectSlot;
        const auto& current = spatialBounds[index];
        if (!scene.m_spatialSlotSeen[slot])
        {
            scene.m_spatialSlotBounds[slot] = current; scene.m_spatialSlotSeen[slot] = 1;
        }
        else
        {
            auto& aggregate = scene.m_spatialSlotBounds[slot];
            if (!aggregate.bounded || !current.bounded) aggregate.bounded = false;
            else
            {
                aggregate.minimum = glm::min(aggregate.minimum, current.minimum);
                aggregate.maximum = glm::max(aggregate.maximum, current.maximum);
            }
        }
    }
    scene.m_lastPrepareTimings.spatialActiveSlots = scene.m_frameRenderItems.size();
    scene.m_lastPrepareTimings.spatialRetainedSlots = scene.m_spatialSlotBounds.size();
    const auto spatial = scene.m_renderSpatialIndex.Update(scene.m_spatialSlotBounds);
    scene.m_lastPrepareTimings.spatialIndexMs = spatial.totalMs; scene.m_lastPrepareTimings.spatialCompareMs = spatial.compareMs;
    scene.m_lastPrepareTimings.spatialRefitMs = spatial.refitMs; scene.m_lastPrepareTimings.spatialBuildMs = spatial.buildMs;
    scene.m_lastPrepareTimings.spatialChangedBounds = spatial.changedBounds; scene.m_lastPrepareTimings.spatialReusedBounds = spatial.reusedBounds;
    scene.m_lastPrepareTimings.spatialDirtyLeaves = spatial.dirtyLeaves; scene.m_lastPrepareTimings.spatialRefitNodes = spatial.refitNodes;
    scene.m_lastPrepareTimings.spatialRebuilds = spatial.rebuilds; scene.m_lastPrepareTimings.spatialQualityRebuilds = spatial.qualityRebuilds;
    scene.m_lastPrepareTimings.spatialMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - spatialStart).count();
}

}
