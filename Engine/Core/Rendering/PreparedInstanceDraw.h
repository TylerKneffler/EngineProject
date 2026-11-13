#pragma once
#include "Core/Graphics/IGraphicsContext.h"

namespace Engine::Rendering
{
// View-local draw data published after visibility/LOD selection. The persistent
// instance pool and frame slot own these resources through native submission.
// Additional passes replay this record without rebuilding tiles or reculling.
struct PreparedInstanceDraw
{
    const Graphics::IGraphicsBuffer* constants = nullptr;
    const Graphics::IGraphicsBuffer* visible = nullptr;
    const Graphics::IGraphicsBuffer* placements = nullptr;
    const Graphics::IGraphicsBuffer* vertices = nullptr;
    const Graphics::IGraphicsBuffer* indices = nullptr;
    uint32_t stride = 0, indexCount = 0, elementCount = 0, firstElement = 0, instanceCount = 0;
    bool indirect = false;

    template<class Context>
    void Submit(Context& context, const Graphics::IPipelineState* pipeline,
        const Graphics::IGraphicsBuffer* emptyMorphDelta,
        const Graphics::IGraphicsBuffer* emptyMorphWeights) const
    {
        context.SetPipeline(pipeline);
        context.SetConstantBuffer(0, constants);
        context.SetStructuredBuffer(45, visible);
        context.SetStructuredBuffer(46, placements);
        context.SetStructuredBuffer(10, emptyMorphDelta);
        context.SetStructuredBuffer(11, emptyMorphWeights);
        context.SetVertexBuffer(0, vertices, stride, 0);
        if (indices) context.SetIndexBuffer(indices, indexCount, 0);
        if (indirect) context.DrawInstancesIndirect(visible, 0, indices != nullptr);
        else if (indices) context.DrawIndexedInstanced(elementCount, instanceCount, firstElement, 0, 0);
        else context.DrawInstanced(elementCount, instanceCount, firstElement, 0);
    }
};
}
