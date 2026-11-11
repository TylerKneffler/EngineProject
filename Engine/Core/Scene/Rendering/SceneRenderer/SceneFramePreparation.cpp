#include "pch.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Sprite/Sprite.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Compoonents/Path/SplineGeometry.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Graphics/GraphicsCommandPacket.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Rendering/Lighting/BakedLightingData.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneNativeRenderPacket.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererFramePreparation.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererUtilities.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/PrefabLoadProfile.h"

namespace Engine::Scene
{
using Renderer::HashRevision;
std::shared_ptr<const Scene::NativeRenderPacket> Scene::TakeNativeRenderPacket()
{
    AssertOwnerThread();
    if (!m_renderFramePrepared || m_nativeRenderPacketTaken)
        throw std::logic_error("Native render packet requires a prepared scene frame");
    m_nativeRenderPacketTaken = true;
    auto packet = std::make_shared<NativeRenderPacket>();
    packet->grassCapsules = m_frameGrassCapsules;
    packet->items = std::move(m_frameRenderItems);
    packet->meshBuffers.reserve(packet->items.size() * 2);
    for (const FrameRenderItem& item : packet->items)
    {
        if (!item.mesh) continue;
        if (auto vertex = item.mesh->GetVertexBufferOwner())
            packet->meshBuffers.push_back(std::move(vertex));
        if (auto index = item.mesh->GetIndexBufferOwner())
            packet->meshBuffers.push_back(std::move(index));
    }
    return packet;
}

void Scene::RetainNativePacketMeshBuffers(const NativeRenderPacket& packet,
    Engine::Graphics::GraphicsCommandPacket& commands) const
{
    AssertOwnerThread();
    for (const auto& buffer : packet.meshBuffers)
        commands.RetainBuffer(buffer);
}

void Scene::SelectNativeFrameBuffers()
{
    if (!m_deferGraphicsRetirement || !m_graphicsProvider ||
        !m_graphicsProvider->GetContextFactory()) return;
    const uint32_t slotIndex = m_graphicsProvider->GetContextFactory()
        ->GetPreparedFrameSlot();
    if (m_nativeFrameBuffers.size() <= slotIndex)
        m_nativeFrameBuffers.resize(static_cast<size_t>(slotIndex) + 1u);
    if (!m_nativeFrameBuffers[slotIndex])
        m_nativeFrameBuffers[slotIndex] = std::make_unique<NativeFrameBuffers>();
    auto* selected = m_nativeFrameBuffers[slotIndex].get();
    if (selected == m_activeNativeFrameBuffers) return;
    if (!m_activeNativeFrameBuffers)
    {
        m_activeNativeFrameBuffers = selected;
        return;
    }
    const auto exchange = [&](NativeFrameBuffers& slot)
    {
        using std::swap;
        swap(m_gridConstantBuffer, slot.grid);
        swap(m_gridCBMapped, slot.gridMapped);
        swap(m_skyboxConstantBuffer, slot.skybox);
        swap(m_skyboxCBMapped, slot.skyboxMapped);
        swap(m_lightDataBuffer, slot.lights);
        swap(m_lightDataMapped, slot.lightsMapped);
        swap(m_boneDataBuffer, slot.bones);
        swap(m_boneDataMapped, slot.bonesMapped);
        swap(m_boneMatrixCapacity, slot.boneCapacity);
        swap(m_clusterHeaderBuffer, slot.clusterHeaders);
        swap(m_clusterHeaderMapped, slot.clusterHeadersMapped);
        swap(m_clusterHeaderCapacity, slot.clusterHeaderCapacity);
        swap(m_clusterLightIndexBuffer, slot.clusterIndices);
        swap(m_clusterLightIndexMapped, slot.clusterIndicesMapped);
        swap(m_clusterLightIndexCapacity, slot.clusterIndexCapacity);
        swap(m_portalApertureBuffer, slot.portalAperture);
        swap(m_portalApertureMapped, slot.portalApertureMapped);
    };
    auto* previous = m_activeNativeFrameBuffers;
    exchange(*previous);
    exchange(*selected);
    auto* factory = m_graphicsProvider->GetBufferFactory();
    const auto createLike = [&](const std::unique_ptr<IGraphicsBuffer>& source,
        std::unique_ptr<IGraphicsBuffer>& destination, void*& mapped)
    {
        if (!source || destination) return;
        destination = factory->CreateBuffer(source->GetUsage(),
            source->GetAccessMode(), source->GetSize(), nullptr,
            source->GetElementStride());
        mapped = destination ? destination->Map() : nullptr;
        if (!mapped)
            throw std::runtime_error("Failed to allocate native frame upload buffer");
    };
    createLike(previous->grid, m_gridConstantBuffer, m_gridCBMapped);
    createLike(previous->skybox, m_skyboxConstantBuffer, m_skyboxCBMapped);
    createLike(previous->lights, m_lightDataBuffer, m_lightDataMapped);
    createLike(previous->bones, m_boneDataBuffer, m_boneDataMapped);
    if (m_boneDataBuffer && !m_boneMatrixCapacity)
        m_boneMatrixCapacity = previous->boneCapacity;
    createLike(previous->portalAperture, m_portalApertureBuffer,
        m_portalApertureMapped);
    m_activeNativeFrameBuffers = selected;
}

void Scene::ProcessGraphicsPreparation(IGraphicsProvider* graphicsProvider)
{
    AssertOwnerThread();
    if (!m_graphicsPreparationPending || !graphicsProvider) return;
    Engine::Serialization::PrefabLoadProfile profile("scene graphics preparation");
    if (m_graphicsPreparationRestart && m_graphicsPreparationItems.empty())
    {
        m_graphicsPreparationItems.clear();
        m_graphicsPreparationCursor = 0;
        for (const auto& object : m_objects)
            for (auto* component : object->Components)
                if (component)
                    m_graphicsPreparationItems.emplace_back(GetObjectId(object.get()), component);
        m_graphicsPreparationRestart = false;
    }
    const auto started = std::chrono::steady_clock::now();
    while (m_graphicsPreparationCursor < m_graphicsPreparationItems.size())
    {
        const auto [id, component] = m_graphicsPreparationItems[m_graphicsPreparationCursor++];
        auto* object = FindObjectById(id);
        if (object && std::find(object->Components.begin(), object->Components.end(), component) != object->Components.end())
        {
            Engine::Serialization::PrefabLoadProfile::Stage stage(
                Engine::Serialization::PrefabLoadProfile::Enabled()
                    ? "graphics_" + component->GetTypeName() : "");
            component->PrepareGraphicsResources(graphicsProvider);
        }
        // Individual driver calls cannot be interrupted. Yield between
        // components, including when one upload exceeds the soft budget.
        if (m_graphicsPreparationBudgetMs > 0 &&
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count() >= m_graphicsPreparationBudgetMs)
            return;
    }
    m_graphicsPreparationItems.clear();
    m_graphicsPreparationCursor = 0;
    // Finish this queue before rescanning requests added during preparation;
    // otherwise continuous streaming can keep restarting at the first object.
    if (m_graphicsPreparationRestart) return;
    m_graphicsPreparationPending = false;
    m_graphicsPreparationBudgetMs = 0;
}

void Scene::PrepareRenderFrame(bool publishSnapshot)
{
    AssertOwnerThread();
    using PrepareClock = std::chrono::steady_clock;
    const auto prepareBegin=PrepareClock::now();
    m_lastPrepareTimings={};
    auto stageBegin=PrepareClock::now();
    Engine::Components::Texture::PublishQueued(m_graphicsProvider);
    auto stageEnd=PrepareClock::now();
    m_lastPrepareTimings.texturePublicationMs=std::chrono::duration<double,std::milli>(stageEnd-stageBegin).count();
    stageBegin=stageEnd;
    m_materialShaderSources.Poll();
    stageEnd=PrepareClock::now();
    m_lastPrepareTimings.shaderSourcePollMs=std::chrono::duration<double,std::milli>(stageEnd-stageBegin).count();
    stageBegin=stageEnd;
    PublishMaterialShaderPipelines();
    stageEnd=PrepareClock::now();
    m_lastPrepareTimings.shaderPipelinePublicationMs=std::chrono::duration<double,std::milli>(stageEnd-stageBegin).count();
    const auto afterAssetPublication=PrepareClock::now();
    const bool priorPublicationSuppressed = m_suppressSceneSnapshotPublication;
    struct RestorePublication
    {
        bool& flag;
        bool previous;
        ~RestorePublication() { flag = previous; }
    } restorePublication{m_suppressSceneSnapshotPublication, priorPublicationSuppressed};
    m_suppressSceneSnapshotPublication = priorPublicationSuppressed || !publishSnapshot;
    // Commit editor edits before preparing owned draw data. Replay-only editor
    // frames publish on their simulation commit instead of scanning twice.
    CommitPendingObjectChanges();
    const auto afterInitialCommit=PrepareClock::now();
    // Copy the registry view: deriving prefab batches adds scene components.
    const auto objectLayers=GetComponents<Engine::Components::TerrainObjectLayer>();
    const auto prefabBudgetStart = PrepareClock::now();
    size_t prefabVisited = 0;
    const size_t prefabCount = objectLayers.size();
    const size_t prefabStart = prefabCount ? m_prefabPreparationCursor % prefabCount : 0;
    for (; prefabVisited < prefabCount; ++prefabVisited)
    {
        auto* layer = objectLayers[(prefabStart + prefabVisited) % prefabCount];
        layer->PreparePrefabInstances(m_graphicsProvider);
        const auto& prefabTimings=layer->GetLastPrefabPrepareTimings();
        m_lastPrepareTimings.prefabSignatureMs+=prefabTimings.placementSignatureMs;
        m_lastPrepareTimings.prefabSourceProbeMs+=prefabTimings.sourceProbeMs;
        if (std::chrono::duration<double, std::milli>(
                PrepareClock::now() - prefabBudgetStart).count() >= 3.0)
        { ++prefabVisited; break; }
    }
    m_prefabPreparationCursor = prefabCount ? (prefabStart + prefabVisited) % prefabCount : 0;
    const auto afterPrefabInstances=PrepareClock::now();
    const auto splineGeometry=GetComponents<SplineGeometry>();
    for (auto* geometry:splineGeometry) geometry->PrepareGeometry(m_graphicsProvider);
    CommitPendingObjectChanges();
    const auto afterPrefabCommit=PrepareClock::now();
    ProcessGraphicsPreparation(m_graphicsProvider);
    const auto afterGraphicsPreparation=PrepareClock::now();
    SelectNativeFrameBuffers();
    const auto afterNativeBuffers=PrepareClock::now();
    const auto elapsed=[](auto begin,auto end) { return std::chrono::duration<double,std::milli>(end-begin).count(); };
    m_lastPrepareTimings.assetPublicationMs=elapsed(prepareBegin,afterAssetPublication);
    m_lastPrepareTimings.objectCommitMs=elapsed(afterAssetPublication,afterInitialCommit)+elapsed(afterPrefabInstances,afterPrefabCommit);
    m_lastPrepareTimings.prefabInstancesMs=elapsed(afterInitialCommit,afterPrefabInstances);
    m_lastPrepareTimings.graphicsPreparationMs=elapsed(afterPrefabCommit,afterGraphicsPreparation);
    m_lastPrepareTimings.nativeBuffersMs=elapsed(afterGraphicsPreparation,afterNativeBuffers);
    const auto preparationJobs=Engine::Rendering::RenderJobs::Get().ReadStatistics();
    m_renderFramePrepared = false;
    m_nativeRenderPacketTaken = false;
    m_frameRenderItems.clear();
    ++m_materialPreparationSerial;
    m_preparedMaterialShaderSources.clear();
    m_frameLightCount = 0;
    m_lastSkinnedObjectCount = 0;

    if (!m_graphicsProvider || !m_lightDataMapped || !m_boneDataMapped)
        return;

    const auto transformBegin=PrepareClock::now();
    const auto structureRevision=GetStructureRevision();
    const bool preparedTransforms=structureRevision==m_transformPreparationRevision;
    m_transformPreparationRevision=structureRevision;
    Engine::Components::TransformHierarchy::Statistics transforms;
    double transformGatherMs=0;
    if(preparedTransforms)
    {
        m_transformPreparationScratch.clear(); m_transformPreparationScratch.reserve(m_objects.size());
        for(const auto& object:m_objects) m_transformPreparationScratch.push_back(&object->transform);
        const auto transformCaptured=PrepareClock::now();
        transformGatherMs=std::chrono::duration<double,std::milli>(transformCaptured-transformBegin).count();
        transforms=m_transformHierarchy.Prepare(m_transformPreparationScratch);
        if(!transforms.valid) throw std::runtime_error("Cannot prepare a cyclic or duplicate transform hierarchy");
    }
    // Streaming/object commit frames retain lazy owner validation. This avoids
    // rebuilding a whole hierarchy index to evaluate a handful of new leaves.
    const auto afterTransforms=PrepareClock::now();

    if(m_frameLookupRevision!=GetStructureRevision() || m_frameLookupEditPose!=m_editorMeshEditPose || m_frameLookupFull)
    {
        const auto describe=[&](Engine::Core::Object* object) {
            FrameComponentLookup lookup{}; lookup.object=object;
            lookup.mesh=object->GetComponent<Engine::Components::Mesh>();
            lookup.sprite=object->GetComponent<Engine::Components::Sprite>();
            lookup.material=object->GetComponent<Engine::Components::Material>();
            lookup.spatialManipulator=object->GetComponent<Engine::Components::SpatialManipulator>();
            lookup.skinnedMesh=m_editorMeshEditPose ? nullptr:object->GetComponent<Engine::Components::SkinnedMesh>();
            lookup.bakedLighting=object->GetComponent<Engine::Rendering::BakedLightingData>(); return lookup;
        };
        if(m_frameLookupFull || m_frameLookupEditPose!=m_editorMeshEditPose)
        {
            m_staticRenderItemCache.clear();
            m_frameComponentLookupScratch.clear(); m_frameLookupIndices.clear(); m_frameLookupFree.clear();
            m_frameComponentLookupScratch.reserve(m_objects.size());
            for(const auto& object:m_objects)
            { m_frameLookupIndices[object.get()]=m_frameComponentLookupScratch.size(); m_frameComponentLookupScratch.push_back(describe(object.get())); }
        }
        else for(auto* owner:m_frameLookupDirty)
        {
            m_staticRenderItemCache.erase(owner);
            auto found=m_frameLookupIndices.find(owner);
            if(!m_componentRegistry.Contains(owner))
            {
                if(found!=m_frameLookupIndices.end())
                { m_frameComponentLookupScratch[found->second]={}; m_frameLookupFree.push_back(found->second); m_frameLookupIndices.erase(found); }
                continue;
            }
            if(found!=m_frameLookupIndices.end()) m_frameComponentLookupScratch[found->second]=describe(owner);
            else
            {
                size_t index=m_frameComponentLookupScratch.size();
                if(m_frameLookupFree.empty()) m_frameComponentLookupScratch.push_back(describe(owner));
                else { index=m_frameLookupFree.back(); m_frameLookupFree.pop_back(); m_frameComponentLookupScratch[index]=describe(owner); }
                m_frameLookupIndices[owner]=index;
            }
        }
        m_frameSpatialManipulatorScratch.clear();
        for(const auto& lookup:m_frameComponentLookupScratch) if(lookup.spatialManipulator) m_frameSpatialManipulatorScratch.push_back(lookup.spatialManipulator);
        m_frameLookupDirty.clear(); m_frameLookupFull=false;
        m_frameLookupRevision=GetStructureRevision(); m_frameLookupEditPose=m_editorMeshEditPose;
    }
    if(m_materialCacheStructureRevision!=GetStructureRevision())
    {
        std::unordered_set<const Engine::Components::Material*> live;
        for(const auto& lookup:m_frameComponentLookupScratch) if(lookup.material) live.insert(lookup.material);
        for(auto it=m_renderMaterialCache.begin();it!=m_renderMaterialCache.end();)
            if(!live.count(it->first)) it=m_renderMaterialCache.erase(it); else ++it;
        m_materialCacheStructureRevision=GetStructureRevision();
    }
    size_t requiredBoneMatrices = 0;
    for (const auto& lookup : m_frameComponentLookupScratch)
        if (lookup.skinnedMesh)
            if (const auto* skeleton = lookup.skinnedMesh->ResolveSkeleton())
                requiredBoneMatrices += skeleton->jointNodes.size();
    if (requiredBoneMatrices > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Bone matrix count exceeds renderer index range");
    EnsureSkinPaletteCapacity(static_cast<uint32_t>(requiredBoneMatrices));

    // Nonlinear mesh results depend on authored geometry and active warp
    // volumes, not on camera motion. A compact signature lets both editor and
    // runtime frames reuse static CPU deformation and its upload buffer.
    uint64_t warpRevision = 1469598103934665603ull;
    bool hasActiveWarpVolume = false;
    for (const FrameComponentLookup& lookup : m_frameComponentLookupScratch)
    {
        if (!lookup.object || !lookup.spatialManipulator)
            continue;
        const auto* manipulator = lookup.spatialManipulator;
        hasActiveWarpVolume = hasActiveWarpVolume ||
            (manipulator->enabled && manipulator->definesWarpVolume &&
                lookup.object->IsEnabledInHierarchy());
        HashRevision(warpRevision,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(manipulator)));
        HashRevision(warpRevision, manipulator->GetConfigurationRevision());
        HashRevision(warpRevision,
            lookup.object->transform.GetWorldRevision());
        HashRevision(warpRevision, manipulator->enabled ? 1u : 0u);
        HashRevision(warpRevision, manipulator->definesWarpVolume ? 1u : 0u);
        HashRevision(warpRevision,
            lookup.object->IsEnabledInHierarchy() ? 1u : 0u);
    }

    std::optional<SpatialWarpSnapshot> warpSnapshot;
    m_frameRenderItems.reserve(m_objects.size());
    if (!hasActiveWarpVolume)
    {
        if (m_deferGraphicsRetirement)
            for (auto& [object, cached] : m_warpedRenderMeshes)
                RetireGraphicsBuffer(std::move(cached.vertexBuffer));
        m_warpedRenderMeshes.clear();
    }
    // The authored Mesh buffer cannot be changed for rendering a nonlinear
    // volume: it may also back a collider, an editor asset, or another draw.
    // Keep a private upload buffer for each affected object instead.
    for (auto cache = m_warpedRenderMeshes.begin();
         cache != m_warpedRenderMeshes.end();)
    {
        const bool stillInScene = std::any_of(m_objects.begin(), m_objects.end(),
            [&](const std::unique_ptr<Engine::Core::Object>& object)
            {
                return object.get() == cache->first;
            });
        if (!stillInScene)
        {
            RetireGraphicsBuffer(std::move(cache->second.vertexBuffer));
            cache = m_warpedRenderMeshes.erase(cache);
        }
        else
            ++cache;
    }
    Engine::Scene::Renderer::SceneRendererFramePreparation::PrepareFrameItems(*this, warpRevision, hasActiveWarpVolume, preparedTransforms, warpSnapshot);
    m_lastPrepareTimings.transformsMs = transforms.totalMs + transformGatherMs;
    m_lastPrepareTimings.transformSnapshotMs = transforms.snapshotMs + transformGatherMs;
    m_lastPrepareTimings.transformComputeMs = transforms.computeMs;
    m_lastPrepareTimings.transformCommitMs = transforms.commitMs;
    m_lastPrepareTimings.transformLocalUpdated = transforms.localUpdated;
    m_lastPrepareTimings.transformWorldUpdated = transforms.worldUpdated;
    m_lastPrepareTimings.transformWorldReused = transforms.worldReused;
    m_lastPrepareTimings.transformLevels = transforms.levels;
    m_lastPrepareTimings.transformStructureFallback = preparedTransforms ? 0 : 1;
    Engine::Scene::Renderer::SceneRendererFramePreparation::PrepareSpatialBounds(*this);
    const auto completedJobs=Engine::Rendering::RenderJobs::Get().ReadStatistics();
    m_lastPrepareTimings.workerMs=double(completedJobs.workerNs-preparationJobs.workerNs)/1.e6;
    m_lastPrepareTimings.mainJobMs=double(completedJobs.callerNs-preparationJobs.callerNs)/1.e6;
    m_lastPrepareTimings.workerWaitMs=double(completedJobs.waitNs-preparationJobs.waitNs)/1.e6;
    m_lastPrepareTimings.workerQueueMs=double(completedJobs.queueNs-preparationJobs.queueNs)/1.e6;
    m_frameGrassCapsules = Engine::Rendering::CaptureGrassInteractionCapsules(*this);
    m_renderFramePrepared = true;
}

// ---------------------------------------------------------------------------
// Scene::Render
// ---------------------------------------------------------------------------


}
