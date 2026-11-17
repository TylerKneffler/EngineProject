// Closed-loop tests of the adaptive detail budget (Rendering/DetailBudget).
//
// A synthetic view supplies the cost curve (cost ~ scale^2, what the
// DetailCostModel predicts for a field of instanced LODs) and a synthetic GPU
// turns the *applied* scale into geometry milliseconds, reported with the
// renderer's few frames of timing latency. The controller runs in simulated
// time through the timestamped ReportGeometryCost/SubmitCostCurve overloads.
#include "Core/Rendering/DetailBudget.h"
#include "Core/Model/ProjectLayers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <vector>

using Engine::Rendering::DetailBudget;
using Engine::Rendering::DetailCostCurve;
using Clock = std::chrono::steady_clock;

namespace
{
int g_failures = 0;
void Check(bool condition, const char* what)
{
    if (!condition) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

DetailCostCurve Curve(double costAtScaleOne)
{
    DetailCostCurve curve;
    curve.valid = true;
    curve.placements = 5000;
    for (int bucket = 0; bucket < DetailCostCurve::Buckets; ++bucket)
    {
        const double scale = std::exp(DetailCostCurve::LogScale(bucket));
        curve.cost[bucket] = costAtScaleOne * scale * scale;
    }
    return curve;
}

struct View
{
    double costAtScaleOne = 1.e5; // curve the model predicts
    double msPerCost = 5.0 / 9.e5; // what the GPU actually charges
};

struct Result
{
    std::vector<double> time, geometryMs, applied;
};

// fixedMs: terrain + grass, independent of LOD distances.
Result Run(double seconds, const std::function<View(double)>& view, double fixedMs = 1.0,
    float startScale = 1.f)
{
    auto& budget = DetailBudget::Get();
    budget.Configure(Engine::Model::DefaultProjectLayers(), {});
    budget.SetFixedGlobalScale(startScale);
    budget.Configure(Engine::Model::DefaultProjectLayers(), {}); // adaptive again, from startScale
    constexpr double frame = 1.0 / 90.0;
    constexpr size_t latencyFrames = 3; // GPU timestamps resolve a few frames late
    std::deque<double> inFlight;
    Result result;
    auto now = Clock::time_point{} + std::chrono::hours(1);
    uint64_t sample = 0;
    for (double t = 0.0; t < seconds; t += frame)
    {
        now += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(frame));
        const View current = view(t);
        const auto curve = Curve(current.costAtScaleOne);
        budget.SubmitCostCurve(curve, now);
        const double drawn = fixedMs + current.msPerCost * curve.CostAt(budget.AppliedScale());
        inFlight.push_back(drawn);
        if (inFlight.size() > latencyFrames)
        {
            budget.ReportGeometryCost(++sample, inFlight.front(), fixedMs, now);
            inFlight.pop_front();
        }
        result.time.push_back(t);
        result.geometryMs.push_back(drawn);
        result.applied.push_back(budget.AppliedScale());
    }
    return result;
}

struct Stats { double minimum = 1e30, maximum = 0, mean = 0, overBudget = 0; };
Stats Measure(const std::vector<double>& values, const std::vector<double>& time, double from,
    double overThreshold = 1e30)
{
    Stats stats; size_t count = 0;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (time[i] < from) continue;
        stats.minimum = std::min(stats.minimum, values[i]);
        stats.maximum = std::max(stats.maximum, values[i]);
        stats.mean += values[i]; ++count;
        if (values[i] > overThreshold) stats.overBudget += 1;
    }
    if (count) { stats.mean /= count; stats.overBudget /= count; }
    return stats;
}
}

int main()
{
    const double budgetMs = Engine::Model::DetailBudgetSettings{}.geometryBudgetMs;

    // Static view. Equilibrium: 1 ms fixed + 5 ms at scale 3 = the 6 ms budget.
    {
        const auto run = Run(40.0, [](double) { return View{}; });
        const auto geometry = Measure(run.geometryMs, run.time, 0.0, budgetMs * 1.15);
        const auto settled = Measure(run.geometryMs, run.time, 20.0);
        const auto scale = Measure(run.applied, run.time, 20.0);
        std::printf("static: geometry peak %.2f ms, over 115%% %.1f%% of frames; settled %.2f..%.2f ms, "
            "scale %.2f..%.2f\n", geometry.maximum, geometry.overBudget * 100.0,
            settled.minimum, settled.maximum, scale.minimum, scale.maximum);
        Check(geometry.maximum <= budgetMs * 1.25, "static view overshoots the geometry budget by >25%");
        Check(settled.mean >= budgetMs * .7 && settled.mean <= budgetMs * 1.05,
            "static view does not settle near the budget");
        Check(scale.maximum / scale.minimum <= 1.15, "static view keeps oscillating after settling");
    }

    // Turning: the view alternates every 1.5 s between sparse and dense
    // content, and the GPU's real cost per predicted unit drifts +-30%.
    {
        const auto run = Run(40.0, [](double t) {
            const int phase = static_cast<int>(t / 1.5);
            View view;
            view.costAtScaleOne = phase % 2 ? 2.e5 : .6e5;
            view.msPerCost *= phase % 3 == 0 ? 1.3 : phase % 3 == 1 ? .8 : 1.0;
            return view;
        });
        const auto geometry = Measure(run.geometryMs, run.time, 5.0, budgetMs * 1.5);
        std::printf("turning: geometry peak %.2f ms, mean %.2f ms, over 150%% %.1f%% of frames\n",
            geometry.maximum, geometry.mean, geometry.overBudget * 100.0);
        Check(geometry.overBudget <= .05, "turning view spends >5% of frames over 150% of budget");
        Check(geometry.maximum <= budgetMs * 2.0, "turning view peaks above twice the budget");
        Check(geometry.mean >= budgetMs * .5, "turning view leaves most of the budget unused");
    }

    // Misleading model (seen in the landscape vista): views the curve calls
    // dense are mostly hidden, so the real GPU cost per predicted unit moves
    // opposite to the prediction. Detail must neither collapse nor run away.
    {
        const auto run = Run(40.0, [](double t) {
            const int phase = static_cast<int>(t / 0.8);
            View view;
            view.costAtScaleOne = phase % 2 ? 3.e5 : .7e5;
            view.msPerCost *= phase % 2 ? .25 : 1.2;
            return view;
        });
        const auto geometry = Measure(run.geometryMs, run.time, 10.0, budgetMs * 1.5);
        const auto scale = Measure(run.applied, run.time, 10.0);
        std::printf("misleading model: geometry peak %.2f ms, mean %.2f ms, over 150%% %.1f%%; "
            "scale %.2f..%.2f (mean %.2f)\n", geometry.maximum, geometry.mean,
            geometry.overBudget * 100.0, scale.minimum, scale.maximum, scale.mean);
        Check(geometry.overBudget <= .05, "misleading model spends >5% of frames over 150% of budget");
        Check(geometry.mean >= budgetMs * .45, "misleading model collapses detail far below the budget");
    }

    std::printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
