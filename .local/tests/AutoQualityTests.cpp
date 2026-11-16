#include "Core/Rendering/AutoQuality.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using Engine::Rendering::AutoQualityController;

namespace
{
void Require(bool value, const char* what)
{
    if (value) return;
    std::fprintf(stderr, "AutoQualityTests failed: %s\n", what);
    std::abort();
}

// A synthetic scene: frame time is a base plus each channel's real cost at
// full quality times its value. Real costs differ from the priors, so the
// controller has to learn them.
struct Scene
{
    double base = 10.0;
    double cost[AutoQualityController::ChannelCount]{};
    double Frame(const AutoQualityController& controller) const
    {
        double total = base;
        for (int channel = 0; channel < AutoQualityController::ChannelCount; ++channel)
            total += cost[channel] * controller.Value(static_cast<AutoQualityController::Channel>(channel));
        return total;
    }
};

// Runs the controller at 60 Hz for the given seconds; returns changes made.
int Run(AutoQualityController& controller, const Scene& scene, double targetMs, double seconds)
{
    int changes = 0;
    for (int frame = 0; frame < static_cast<int>(seconds * 60.0); ++frame)
        changes += controller.Update(scene.Frame(controller), targetMs, 1.0 / 60.0) ? 1 : 0;
    return changes;
}
}

int main()
{
    using C = AutoQualityController;
    constexpr double Target = 16.67;

    // Within the target: nothing changes.
    {
        AutoQualityController controller;
        Scene scene;
        scene.base = 12.0;
        Require(Run(controller, scene, Target, 10.0) == 0, "no change while under the target");
        for (int channel = 0; channel < C::ChannelCount; ++channel)
            Require(controller.Value(static_cast<C::Channel>(channel)) == 1.f, "full quality kept");
    }

    // Over the target: lowers until the frame fits, and learns that
    // vegetation (prior 3 ms) is really cheap here while shadows are not.
    {
        AutoQualityController controller;
        Scene scene;
        scene.base = 12.0;
        scene.cost[C::Shadows] = 6.0;
        scene.cost[C::Vegetation] = .3;
        scene.cost[C::ViewDistance] = 1.0;
        scene.cost[C::Texture] = .2;
        scene.cost[C::Material] = .5;
        // 12 + 8 = 20 ms against 16.67.
        Run(controller, scene, Target, 30.0);
        const double frame = scene.Frame(controller);
        Require(frame <= Target * C::OverFraction, "frame brought under the target");
        Require(controller.Value(C::Shadows) < 1.f, "shadows, the real cost, lowered");
        Require(controller.Value(C::Vegetation) > .4f + 1.e-3f, "cheap vegetation not driven to its floor");
        Require(controller.Value(C::Texture) == 1.f, "costly-to-lose, cheap texture kept");
        Require(controller.LearnedMsPerUnit(C::Vegetation) < 2.0, "vegetation learned as cheaper than its prior");
        Require(controller.LearnedMsPerUnit(C::Shadows) > 3.0, "shadows learned as costly");
        Require(controller.Value(C::Geometry) == 1.f && controller.Value(C::RenderScale) == 1.f,
            "geometry and render scale never adjusted");
        // Settled: holds without hunting.
        Require(Run(controller, scene, Target, 20.0) <= 1, "holds once settled");
    }

    // Load drops: quality comes back to full.
    {
        AutoQualityController controller;
        Scene scene;
        scene.base = 14.0;
        scene.cost[C::Shadows] = 4.0;
        scene.cost[C::ViewDistance] = 2.0;
        Run(controller, scene, Target, 30.0);
        Require(controller.Value(C::Shadows) < 1.f || controller.Value(C::ViewDistance) < 1.f,
            "lowered under load");
        scene.base = 6.0;
        Run(controller, scene, Target, 60.0);
        for (int channel = 0; channel < C::ChannelCount; ++channel)
            Require(controller.Value(static_cast<C::Channel>(channel)) == 1.f, "full quality restored");
    }

    // Impossible target: every adjustable channel ends at its floor and the
    // controller stops there.
    {
        AutoQualityController controller;
        Scene scene;
        scene.base = 40.0;
        scene.cost[C::Shadows] = 2.0;
        Run(controller, scene, Target, 60.0);
        const auto policies = C::DefaultPolicies();
        for (int channel = 0; channel < C::ChannelCount; ++channel)
            if (policies[channel].adjustable)
                Require(std::abs(controller.Value(static_cast<C::Channel>(channel)) - policies[channel].minimum) < 1.e-4f,
                    "floor reached");
        Require(Run(controller, scene, Target, 10.0) == 0, "no changes at the floor");
    }

    // Material is one step: full or simplified, nothing between.
    {
        AutoQualityController controller;
        Scene scene;
        scene.base = 40.0;
        Run(controller, scene, Target, 60.0);
        Require(controller.Value(C::Material) == .5f, "material steps straight to simplified");
    }

    // A single spike does not move quality.
    {
        AutoQualityController controller;
        Scene scene;
        scene.base = 12.0;
        Run(controller, scene, Target, 2.0);
        Require(!controller.Update(80.0, Target, 1.0 / 60.0), "spike ignored on its frame");
        Require(Run(controller, scene, Target, 5.0) == 0, "spike does not trigger a step");
    }

    std::printf("AutoQualityTests passed\n");
    return 0;
}
