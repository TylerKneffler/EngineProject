#pragma once

#include "Core/Model/ProjectLayers.h"
#include "Core/Rendering/DetailCostModel.h"
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Engine::Rendering
{
// Process-wide project layers and the adaptive detail budget.
//
// The project's layer list is set once from ProjectSettings (and again when
// the editor changes it). The renderer reports the GPU time of geometry
// passes each frame; the budget turns that into one global detail scale,
// eased over DetailBudgetSettings::responseSeconds. Each layer's LOD scale is
//     lodDistanceScale * globalScale ^ budgetWeight
// so exempt layers (weight 0) keep their authored detail.
//
// LayerLodScale and GlobalScale are lock-free and may be read from any thread.
class DetailBudget
{
public:
    static DetailBudget& Get();

    void Configure(const std::vector<Engine::Model::ProjectLayer>& layers,
        const Engine::Model::DetailBudgetSettings& settings);

    std::vector<Engine::Model::ProjectLayer> GetLayers() const;
    Engine::Model::DetailBudgetSettings GetSettings() const;
    // Index of the named layer, or -1.
    int FindLayer(std::string_view name) const;
    // The named layer, or 0 (Default) when it does not exist.
    int FindLayerOrDefault(std::string_view name) const;
    std::string GetLayerName(int layer) const;

    float LayerLodScale(int layer) const
    {
        const size_t index = layer >= 0 &&
            static_cast<size_t>(layer) < m_layerScales.size()
            ? static_cast<size_t>(layer) : 0;
        return m_layerScales[index].load(std::memory_order_relaxed);
    }
    // The eased controller output.
    float GlobalScale() const { return m_globalScale.load(std::memory_order_relaxed); }
    // The scale LODs actually use: it glides toward a stepped copy of the
    // controller output (see PublishScales).
    float AppliedScale() const { std::lock_guard lock(m_mutex); return m_publishedGlobal; }
    // Incremented whenever the layer scales change. Systems with expensive
    // detail changes (terrain cell LODs, grass) compare it to react.
    uint64_t Revision() const { return m_revision.load(std::memory_order_relaxed); }
    // The last geometry GPU time reported, in milliseconds.
    double LastGeometryMs() const { return m_lastGeometryMs.load(std::memory_order_relaxed); }
    // CPU render-recording time of the last reported frame, in milliseconds.
    double LastCpuRenderMs() const { return m_lastCpuMs.load(std::memory_order_relaxed); }
    bool Enabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // CPU time spent recording render passes; accumulated until the next
    // ReportGeometryCost, which treats it as one frame's CPU render cost.
    void AddCpuRenderTime(double milliseconds);
    // One measured frame of geometry GPU time. Call once per resolved GPU
    // frame; repeated sample ids are ignored.
    // fixedMs is the part LOD distances do not change (terrain, grass); the
    // cost model calibrates against the rest.
    void ReportGeometryCost(uint64_t sampleId, double geometryMs, double fixedMs = 0.0);
    // Test and tool hook: fixes the global scale (and disables adaptation
    // until the next Configure).
    void SetFixedGlobalScale(float scale);

    // Predicted cost of the main view's LOD'd instances at every scale (see
    // DetailCostModel.h). While a recent curve exists, the scale is the
    // largest the cost budget affords; otherwise the reactive controller runs.
    void SubmitCostCurve(const DetailCostCurve& curve);
    struct ModelStatus
    {
        bool active = false;
        double costBudget = 0.0, predictedCost = 0.0, occludedFraction = 0.0;
        uint32_t placements = 0;
        float targetScale = 1.f;
    };
    ModelStatus GetModelStatus() const;

    // Texture budget (DetailBudgetSettings::textureBudget*). The texture
    // streamer calls this once per frame with the bytes textures occupy now
    // (streamed and fully resident); it returns the pool size in bytes, 0 when
    // the budget is off. Video memory is sampled about once a second.
    uint64_t UpdateTextureBudget(uint64_t textureBytes);
    struct TextureBudgetStatus
    {
        uint64_t budgetBytes = 0, baseBytes = 0, extendedBytes = 0, textureBytes = 0;
        uint64_t vramBudgetBytes = 0, vramUsageBytes = 0;
    };
    TextureBudgetStatus GetTextureBudgetStatus() const;

private:
    DetailBudget();
    // seconds > 0 glides the applied scale toward its step; 0 jumps to it.
    void PublishScales(double seconds = 0.0);

    mutable std::mutex m_mutex;
    std::vector<Engine::Model::ProjectLayer> m_layers;
    Engine::Model::DetailBudgetSettings m_settings;
    std::array<std::atomic<float>, Engine::Model::MaxProjectLayers> m_layerScales{};
    std::atomic<float> m_globalScale{ 1.f };
    std::atomic<uint64_t> m_revision{ 1 };
    std::atomic<double> m_lastGeometryMs{ 0.0 };
    std::atomic<double> m_lastCpuMs{ 0.0 };
    double m_cpuAccumulatedMs = 0.0;
    double m_smoothedGpuMs = 0.0, m_smoothedCpuMs = 0.0, m_smoothedFixedMs = 0.0;
    std::atomic<bool> m_enabled{ true };
    bool m_fixed = false;
    uint64_t m_lastSampleId = 0;
    std::chrono::steady_clock::time_point m_lastReport{};
    float m_steppedGlobal = 1.f;
    float m_publishedGlobal = 1.f;
    DetailCostCurve m_curve;
    std::chrono::steady_clock::time_point m_curveTime{};
    double m_costBudget = 0.0;
    float m_modelTarget = 1.f;
    TextureBudgetStatus m_texture;
    std::chrono::steady_clock::time_point m_lastTextureSample{};
};
}
