#include "Core/Rendering/Portal/PortalRenderPolicy.h"
#include <cassert>

int main()
{
    using namespace Engine::Rendering::Portal;

    constexpr uint32_t rootBase = 17u;
    for (uint32_t depth = 0; depth <
        static_cast<uint32_t>(kMaximumRecursionDepth); ++depth)
    {
        const StencilStep dx11 = StencilForDepth(
            Backend::DirectX11, depth, rootBase);
        const StencilStep dx12 = StencilForDepth(
            Backend::DirectX12, depth, rootBase);
        const StencilStep vulkan = StencilForDepth(
            Backend::Vulkan, depth, rootBase);
        assert(dx11 == dx12);
        assert(dx11 == vulkan);
        assert(dx11.sceneReadReference == rootBase + depth);
        if (depth == 0u)
        {
            assert(dx11.writeOperation == ApertureWriteOperation::Replace);
            assert(dx11.writeReference == rootBase);
        }
        else
        {
            assert(dx11.writeOperation ==
                ApertureWriteOperation::IncrementAndClamp);
            assert(dx11.writeReference == rootBase + depth - 1u);
        }
    }

    assert(StencilForDepth(Backend::DirectX12, 0u, 1u).
        sceneReadReference != StencilForDepth(
            Backend::DirectX12, 0u, 9u).sceneReadReference);

    assert(TriangulatedApertureVertexCount(0) == 0u);
    assert(TriangulatedApertureVertexCount(2) == 0u);
    assert(TriangulatedApertureVertexCount(3) == 3u);
    assert(TriangulatedApertureVertexCount(4) == 6u);
    assert(TriangulatedApertureVertexCount(8) == 18u);

    assert(ClampRecursionDepth(-1) == kMinimumRecursionDepth);
    assert(ClampRecursionDepth(100) == kMaximumRecursionDepth);
    assert(ClampViewBudget(0) == kMinimumViewBudget);
    assert(ClampViewBudget(1000) == kMaximumViewBudget);
    return 0;
}
