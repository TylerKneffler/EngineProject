#include "Core/Scene/Rendering/SceneRenderer/SceneRendererGeometrySubmission.h"

#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IPipelineState.h"
#include "Core/Rendering/PersistentInstanceBatches.h"
#include "Core/Scene/Rendering/SceneRenderer/SceneRendererMaterialShaders.h"

namespace Engine::Scene::Renderer
{
SceneRendererGeometrySubmission::SceneRendererGeometrySubmission(
    Scene& scene,
    Engine::Graphics::IGraphicsContext* context)
    : m_scene(scene), m_context(context)
{
}

Engine::Graphics::IPipelineState* SceneRendererGeometrySubmission::GeometryPipeline(
    const PreparedDraw& draw,
    Engine::Graphics::IPipelineState* base) const
{
    if (!draw.terrain)
        return base;
    const auto terrain = m_scene.m_terrainPipelineByBase.find(base);
    return terrain != m_scene.m_terrainPipelineByBase.end()
        ? terrain->second.get() : base;
}

void SceneRendererGeometrySubmission::SubmitGeometry(
    const PreparedDraw& draw,
    bool shadowGeometry)
{
    auto* geometryMesh = shadowGeometry && draw.shadowMesh
        ? draw.shadowMesh : draw.mesh;
    auto* geometryVertexBuffer = shadowGeometry && draw.shadowVertexBuffer
        ? draw.shadowVertexBuffer : draw.vertexBuffer;
    auto* geometryIndexBuffer = shadowGeometry && draw.shadowIndexBuffer
        ? draw.shadowIndexBuffer : draw.indexBuffer;
    const uint32_t vertexStride = shadowGeometry && draw.shadowVertexStride > 0
        ? draw.shadowVertexStride : draw.vertexStride;
    const uint32_t vertexCount = shadowGeometry && draw.shadowVertexCount > 0
        ? draw.shadowVertexCount : draw.vertexCount;
    const uint32_t indexCount = shadowGeometry && draw.shadowIndexCount > 0
        ? draw.shadowIndexCount : draw.indexCount;

    if (geometryMesh && geometryMesh->IsInstanced())
    {
        auto& instances = *geometryMesh->GetInstanceData();
        if (instances.slicesRevision != instances.revision)
        {
            for (auto& slice : instances.slices)
                m_scene.m_persistentInstances.Retire(std::move(slice.buffer));
            instances.slices.clear();
            instances.slicesRevision = instances.revision;
        }
        for (const auto& range : geometryMesh->GetInstanceRanges())
        {
            if (!range.instanceCount || geometryMesh->GetInstanceBuffer(range.firstInstance))
                continue;
            Engine::Components::Mesh::InstanceData::Slice slice;
            slice.firstInstance = range.firstInstance;
            slice.instanceCount = range.instanceCount;
            slice.buffer = m_scene.m_graphicsProvider->GetBufferFactory()->
                CreateStaticShaderResource(
                    size_t(range.instanceCount) *
                        sizeof(Engine::Components::Mesh::InstanceTransform),
                    instances.transforms.data() + range.firstInstance,
                    sizeof(Engine::Components::Mesh::InstanceTransform));
            ++m_scene.m_lastRenderCpuTimings.instances.allocations;
            m_scene.m_lastRenderCpuTimings.instances.fallbackBytes +=
                slice.buffer->GetSize();
            instances.slices.push_back(std::move(slice));
        }
    }

    m_context->SetStructuredBuffer(10, draw.morphDeltaBuffer);
    m_context->SetStructuredBuffer(11, draw.morphWeightBuffer);
    m_context->SetStructuredBuffer(45,
        geometryMesh && geometryMesh->IsInstanced()
            ? geometryMesh->GetInstanceBuffer()
            : m_scene.m_emptyMorphDeltaBuffer.get());
    m_context->SetStructuredBuffer(46, m_scene.m_emptyMorphDeltaBuffer.get());
    m_context->SetVertexBuffer(0, geometryVertexBuffer, vertexStride, 0);

    if (geometryMesh && geometryMesh->IsInstanced())
    {
        for (const auto& range : geometryMesh->GetInstanceRanges())
        {
            if (!range.instanceCount || !range.vertexCount)
                continue;
            // D3D starts SV_InstanceID at zero, so bind the compact species slice.
            m_context->SetStructuredBuffer(45,
                geometryMesh->GetInstanceBuffer(range.firstInstance));
            if (geometryIndexBuffer)
            {
                m_context->SetIndexBuffer(geometryIndexBuffer, indexCount, 0);
                m_context->DrawIndexedInstanced(range.vertexCount,
                    range.instanceCount, range.firstVertex, 0, 0);
            }
            else
                m_context->DrawInstanced(range.vertexCount,
                    range.instanceCount, range.firstVertex, 0);
        }
    }
    else if (geometryIndexBuffer && indexCount > 0u)
    {
        m_context->SetIndexBuffer(geometryIndexBuffer, indexCount, 0);
        m_context->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    }
    else
        m_context->DrawInstanced(vertexCount, 1, 0, 0);
}

void SceneRendererGeometrySubmission::SubmitMaterialGeometry(
    const PreparedDraw& draw,
    Engine::Graphics::IPipelineState* base,
    const std::function<void()>& restoreState)
{
    auto* pipeline = base;
    if (draw.material && !draw.material->shader.empty() &&
        !draw.surfaceShaderResolved)
    {
        std::string error;
        if (auto* custom = m_scene.ResolveMaterialShaderPipeline(
            m_scene.ResolveMaterialShaderSource(*draw.material,
                draw.material->shader),
            base, draw.terrain, false, "Auto", error))
            pipeline = custom;
        if (!error.empty())
            draw.material->shaderError = error;
    }
    m_context->SetPipeline(pipeline);
    if (restoreState)
        restoreState();
    SubmitGeometry(draw);
}
}
