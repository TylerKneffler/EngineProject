#include "Core/Rendering/Portal/PortalRenderPolicy.h"
#include <cassert>
#include <array>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <string>

// CTest must receive a process failure, not a modal CRT assertion dialog.
#undef assert
#define assert(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #condition, \
        __FILE__, __LINE__); return 1; } } while (false)

namespace
{
// A tiny API-neutral aperture execution model. Backend adapters must produce
// this exact stencil/depth result; the real draw path supplies the same
// StencilForDepth values to DX11, DX12, and Vulkan graphics contexts.
struct ApertureSurface
{
    std::array<uint8_t, 16> stencil{};
    std::array<float, 16> depth{};

    ApertureSurface() { depth.fill(1.f); }

    void WriteAperture(const Engine::Rendering::Portal::StencilStep& step,
        const std::array<bool, 16>& aperture)
    {
        for (size_t pixel = 0; pixel < aperture.size(); ++pixel)
        {
            if (!aperture[pixel])
                continue;
            if (step.writeOperation ==
                Engine::Rendering::Portal::ApertureWriteOperation::Replace)
            {
                stencil[pixel] = static_cast<uint8_t>(step.writeReference);
            }
            else if (stencil[pixel] == step.writeReference)
            {
                stencil[pixel] = static_cast<uint8_t>(step.sceneReadReference);
            }
        }
    }

    bool DrawRemote(size_t pixel, uint32_t expectedStencil,
        float remoteDepth, float targetPlaneDistance) const
    {
        // Remote fragments must be inside this aperture, on the target-facing
        // side of its plane, and not behind already-written remote depth.
        return stencil[pixel] == expectedStencil &&
            targetPlaneDistance >= 0.f && remoteDepth <= depth[pixel];
    }
};

std::string ReadFile(const char* path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
}

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
    assert(ClampConnectionRepeatLimit(0) ==
        kMinimumConnectionRepeatLimit);
    assert(ClampConnectionRepeatLimit(100) ==
        kMaximumConnectionRepeatLimit);
    // A repeat limit counts the direct connection as its first visit. Two
    // visits therefore allow A -> B -> A once; the third is rejected.
    assert(CanRepeatConnection(0u, 2));
    assert(CanRepeatConnection(1u, 2));
    assert(!CanRepeatConnection(2u, 2));
    int entranceEndpoint = 0;
    int exitEndpoint = 0;
    assert(!IsImmediateExitAperture(&entranceEndpoint, nullptr));
    assert(!IsImmediateExitAperture(&entranceEndpoint, &exitEndpoint));
    assert(IsImmediateExitAperture(&exitEndpoint, &exitEndpoint));
    assert(ClampViewBudget(0) == kMinimumViewBudget);
    assert(ClampViewBudget(1000) == kMaximumViewBudget);

    const std::array<bool, 16> firstAperture {
        false, false, false, false,
        false, true,  true,  false,
        false, true,  true,  false,
        false, false, false, false };
    const std::array<bool, 16> secondAperture {
        false, false, false, false,
        false, false, false, false,
        false, false, false, true,
        false, false, false, true };
    const std::array<bool, 16> nestedAperture {
        false, false, false, false,
        false, false, true,  false,
        false, false, false, false,
        false, false, false, false };

    for (const Backend backend : { Backend::DirectX11, Backend::DirectX12,
        Backend::Vulkan })
    {
        ApertureSurface surface;
        const StencilStep root = StencilForDepth(backend, 0u, 11u);
        const StencilStep otherRoot = StencilForDepth(backend, 0u, 31u);
        const StencilStep nested = StencilForDepth(backend, 1u, 11u);
        surface.WriteAperture(root, firstAperture);
        surface.WriteAperture(otherRoot, secondAperture);
        surface.WriteAperture(nested, nestedAperture);

        // Root portals do not leak into each other; recursive views inherit
        // their parent aperture; target clipping and depth both reject leaks.
        assert(surface.DrawRemote(5u, root.sceneReadReference, 0.5f, 0.1f));
        assert(!surface.DrawRemote(5u, otherRoot.sceneReadReference, 0.5f, 0.1f));
        assert(surface.DrawRemote(6u, nested.sceneReadReference, 0.5f, 0.1f));
        assert(!surface.DrawRemote(5u, nested.sceneReadReference, 0.5f, 0.1f));
        assert(surface.DrawRemote(11u, otherRoot.sceneReadReference, 0.5f, 0.1f));
        assert(!surface.DrawRemote(11u, root.sceneReadReference, 0.5f, 0.1f));
        assert(!surface.DrawRemote(6u, nested.sceneReadReference, 0.5f, -0.001f));
        assert(!surface.DrawRemote(6u, nested.sceneReadReference, 1.1f, 0.1f));
    }

    // All normal backends compile the same object shader. Keep the clipping
    // uniform/fragment rejection in the parity test so a shader edit cannot
    // silently regress one backend while its stencil constants still match.
    const std::string objectShader =
        ReadFile("Engine/Core/Shaders/Object/Object.hlsl");
    assert(objectShader.find("portalClipPlane") != std::string::npos);
    assert(objectShader.find("clip(-1.0)") != std::string::npos);
    return 0;
}
