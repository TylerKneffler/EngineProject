#include "Core/Rendering/MeshLodSelection.h"
#include <cstdlib>

static void require(bool condition) { if (!condition) std::abort(); }

// Coverage produced by Object.hlsl's distance-LOD intervals. In a fixed preview
// no other LOD is drawn, so this coverage must not fall to zero as the view moves.
static float coverage(glm::vec4 fade, float distance, int runtimeLod)
{
    if (fade.w < .5f) return 1.f;
    if (fade.w >= 2.f)
    {
        const float width = glm::clamp((fade.z - fade.y) *
            (fade.w >= 4.f ? .25f : fade.x), 0.f, .5f);
        const float first = glm::smoothstep(fade.y - width, fade.y + width, distance);
        const float second = glm::smoothstep(fade.z - width, fade.z + width, distance);
        const int lod = fade.w >= 4.f ? int(fade.x) : runtimeLod;
        return (lod == 0 ? 1.f - first : lod == 1 ? first - second : second)
            * glm::clamp(fade.w - 2.f, 0.f, 1.f);
    }
    return glm::max(0.f, fade.y - fade.x);
}

int main()
{
    using Engine::Rendering::ResolveLodPreviewFade;
    for (int lod = 0; lod < 3; ++lod)
    {
        // Authored prefab bands and runtime-generated LODs use different encodings.
        for (auto fade : {glm::vec4(float(lod), 20.f, 60.f, 4.f),
                          glm::vec4(.25f, 20.f, 60.f, 3.f)})
        {
            const auto preview = ResolveLodPreviewFade(fade, true, false, lod);
            for (float distance : {0.f, 10.f, 20.f, 40.f, 60.f, 100.f, 1000.f})
                require(coverage(preview, distance, lod) == 1.f);
            require(ResolveLodPreviewFade(fade, true, false, -1) == fade);
            require(ResolveLodPreviewFade(fade, false, false, lod) == fade);
            require(ResolveLodPreviewFade(fade, true, true, lod) == fade);
        }
    }
    // Reproduces the reported missing-near-LOD failure without the preview fix.
    require(coverage({0.f, 20.f, 60.f, 4.f}, 100.f, 0) == 0.f);
    const glm::vec4 transition(.25f, .75f, 0.f, 1.f);
    require(coverage(ResolveLodPreviewFade(transition, true, false, 0), 100.f, 0) == 1.f);
    require(ResolveLodPreviewFade(transition, true, false, -1) == transition);
}
