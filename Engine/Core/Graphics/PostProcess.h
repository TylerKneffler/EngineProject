#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace Engine::Graphics
{
struct PostProcessSettings
{
    float exposure = 1.0f;
    uint32_t toneMapping = 1;
    // Fraction of the blurred bloom chain mixed into the HDR image. Zero
    // skips the bloom passes entirely.
    float bloomIntensity = 0.f;
    bool colorGrading = false;
    float shadowTint[3] = { 1.f, 1.f, 1.f };
    float highlightTint[3] = { 1.f, 1.f, 1.f };
    float gradingBalance = 0.f;
    float gradingContrast = 1.f;
    float gradingSaturation = 1.f;
    // Auto exposure (eye adaptation): the scene's log-luminance histogram,
    // trimmed to the [lowPercent, highPercent] range, gives its average
    // brightness; `exposure` above becomes compensation on top of mapping
    // that average to mid grey. EVs are log2 luminance, clamped to
    // [minEv, maxEv]. Speeds are in 1/s toward a brighter (up) or darker
    // (down) scene.
    bool autoExposure = false;
    float autoExposureMinEv = -8.f;
    float autoExposureMaxEv = 4.f;
    float autoExposureSpeedUp = 3.f;
    float autoExposureSpeedDown = 1.f;
    float autoExposureLowPercent = 0.5f;
    float autoExposureHighPercent = 0.95f;
    // FXAA in the compose pass (Model::AntiAliasingSettings). Off by default
    // so offline paths (video export) keep their own settings.
    bool fxaa = false;
    float fxaaSubpixel = 0.75f;
    float fxaaEdgeThreshold = 0.125f;
    // Temporal anti-aliasing, filled per view by the scene renderer: the
    // camera was jittered, and taaReprojection (row-major) maps this frame's
    // clip position (NDC x, y, device depth, 1) to the previous frame's clip
    // space (unjittered). History is unusable after a cut or resize.
    bool taa = false;
    bool taaHistoryValid = false;
    float taaFeedback = 0.9f;
    float taaSharpen = 0.f; // compose unsharp mask after TAA
    float taaReprojection[16] = { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f };
};

void SetPostProcessSettings(const PostProcessSettings& settings);
PostProcessSettings GetPostProcessSettings();

// Every post-process pass shares this 64-byte constant block (a DX cbuffer,
// DX12 root constants, or a Vulkan push-constant range). Layout matches
// Shaders/PostProcess/PostProcess.hlsl.
struct PostProcessConstants
{
    // Compose: exposure, operator (uint bits), bloom intensity, sample weight
    // (operator 4) or FXAA (other operators; see MakeComposeConstants).
    // Bloom passes: source texel size xy.
    float params0[4] = {};
    float shadowTint[4] = {}; // rgb, w = grading enabled
    float highlightTint[4] = {}; // rgb, w = shadow/highlight balance
    float grade[4] = {}; // contrast, saturation, 1 / bloom levels, auto exposure (1/0)
    // Temporal resolve: history weight, history valid (1/0). The temporal
    // pass reads its reprojection matrix rows from the four vectors above.
    // Compose: z = sharpening amount after TAA (0 off).
    float temporal[4] = {};
};
static_assert(sizeof(PostProcessConstants) == 80);

PostProcessConstants MakeComposeConstants(const PostProcessSettings& settings,
    uint32_t bloomLevels, float sampleWeight = 1.f);
PostProcessConstants MakeBloomPassConstants(uint32_t sourceWidth, uint32_t sourceHeight);
// TemporalResolvePS: reprojection rows in params0/shadowTint/highlightTint/
// grade, temporal = (history weight, history valid, 0, 0). historyValid is
// false when the renderer's own history is new (first frame, resize).
PostProcessConstants MakeTemporalConstants(const PostProcessSettings& settings, bool historyValid);

// Auto exposure runs three small passes before compose, on every backend:
//   luminance  scene -> LuminanceMap (log2 luminance, metering weight)
//   histogram  LuminanceMap -> ExposureBins x 1 (weighted counts)
//   adapt      histogram (t0) + previous exposure (t1) -> 1 x 1 exposure
//              (adapted EV, history valid, target EV)
// Two 1x1 exposure targets alternate frames; compose reads the current one
// as t2 (Vulkan binding 3). `history` false (first frame, resize, re-enable)
// snaps to the target without reading the previous target.
constexpr uint32_t ExposureMapWidth = 64, ExposureMapHeight = 32;
constexpr uint32_t ExposureBins = 64;
constexpr float ExposureHistogramMinEv = -14.f, ExposureHistogramMaxEv = 10.f;
PostProcessConstants MakeExposureConstants(const PostProcessSettings& settings,
    float deltaSeconds, bool history);

// Per-renderer auto exposure bookkeeping.
struct ExposureFrame
{
    uint32_t target = 0; // exposure target written this frame
    bool history = false; // the other target holds last frame's result
    float deltaSeconds = 0.f;
};
class ExposureState
{
public:
    // Once per composed frame with auto exposure on.
    ExposureFrame Next()
    {
        const auto now = std::chrono::steady_clock::now();
        ExposureFrame frame;
        frame.target = 1u - m_target;
        frame.history = m_history;
        frame.deltaSeconds = m_history
            ? std::chrono::duration<float>(now - m_last).count() : 0.f;
        m_target = frame.target;
        m_history = true;
        m_last = now;
        return frame;
    }
    // Targets recreated, or a frame composed without auto exposure: the
    // next frame snaps instead of adapting from stale history.
    void Reset() { m_history = false; }
    uint32_t Current() const { return m_target; }

private:
    uint32_t m_target = 1;
    bool m_history = false;
    std::chrono::steady_clock::time_point m_last{};
};

// Bloom renders into a half-resolution mip chain. Levels stop before the
// smallest mip gets narrower than about eight pixels.
constexpr uint32_t MaxBloomLevels = 6;
inline uint32_t BloomLevelCount(uint32_t width, uint32_t height)
{
    uint32_t levels = 0;
    uint32_t side = std::min(width, height) / 2u;
    while (levels < MaxBloomLevels && side >= 8u)
    {
        ++levels;
        side /= 2u;
    }
    return std::max(levels, 1u);
}
inline uint32_t BloomLevelExtent(uint32_t fullExtent, uint32_t level)
{
    return std::max(1u, (fullExtent / 2u) >> level);
}

// HLSL source of Shaders/PostProcess/PostProcess.hlsl for backends that
// compile at runtime. Throws when the file cannot be found.
std::string LoadPostProcessShaderSource();
}
