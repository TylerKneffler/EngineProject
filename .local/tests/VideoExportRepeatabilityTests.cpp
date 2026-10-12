#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Physics/Collider.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Rendering/ExportFrameSequence.h"
#include "Core/Renderers/DX11/DX11GameRenderer.h"
#include "Core/Scene/Scene.h"
#include "Core/SceneManager.h"
#include "Core/Window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <glm/geometric.hpp>

namespace
{
constexpr uint32_t kWidth = 320u;
constexpr uint32_t kHeight = 180u;
constexpr uint32_t kFps = 30u;
constexpr uint64_t kFrameCount = 12u;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "video export repeatability failed: %s (%s:%d)\n", \
        #condition, __FILE__, __LINE__); return false; } } while (false)

uint64_t HashFrame(const std::vector<uint8_t>& pixels)
{
    uint64_t hash = 1469598103934665603ull;
    for (uint8_t value : pixels)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

struct ExportResult
{
    std::vector<uint64_t> frameHashes;
    std::vector<double> sampleTimes;
    std::vector<uint64_t> simulationFrames;
};

bool CaptureExport(Engine::Renderers::DX11GameRenderer& renderer,
    const std::string& scenePath, ExportResult& result)
{
    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    Engine::Core::SceneManager::SetActiveScene(&scene);
    Engine::Core::SceneManager::SetDefaultScenePath(scenePath);
    CHECK(scene.Load(scenePath));
    scene.SetFixedTimeStep(1.0 / static_cast<double>(kFps));
    scene.GetPhysics().ConfigureFixedStep(1.0 / 120.0, 8u, 10u);
    scene.Start();

    Engine::Rendering::ExportFrameSequence sequence(scene);
    std::vector<uint8_t> pixels;
    const auto consumeReadback = [&](bool wait)
    {
        if (!renderer.ReadExportFrameRGBA(pixels, wait))
            return !wait;
        result.frameHashes.push_back(HashFrame(pixels));
        return true;
    };

    for (uint64_t frame = 0; frame < kFrameCount; ++frame)
    {
        sequence.PrepareNextFrame();
        result.sampleTimes.push_back(scene.GetElapsedTime());
        result.simulationFrames.push_back(scene.GetFrameIndex());

        renderer.BeginFrame();
        std::unique_ptr<Engine::Graphics::IGraphicsContext> context =
            renderer.CreateFrameGraphicsContext();
        Engine::Components::Camera* camera = scene.FindGameCamera();
        CHECK(context && camera);
        const float aspect = static_cast<float>(kWidth) /
            static_cast<float>(kHeight);
        scene.Render(context.get(), aspect, camera, false,
            kWidth, kHeight, true);
        renderer.Clear(0.1f, 0.1f, 0.1f, 1.f);
        scene.Render(context.get(), aspect, camera, false);
        renderer.EndFrame();

        CHECK(consumeReadback(false));
        if (renderer.GetPendingExportFrameCount() >=
            renderer.GetExportReadbackCapacity())
            CHECK(consumeReadback(true));
    }
    while (renderer.GetPendingExportFrameCount() > 0u)
        CHECK(consumeReadback(true));
    renderer.WaitIdle();
    Engine::Core::SceneManager::SetActiveScene(nullptr);

    CHECK(sequence.GetPreparedFrameCount() == kFrameCount);
    CHECK(result.frameHashes.size() == kFrameCount);
    CHECK(result.sampleTimes.size() == kFrameCount);
    CHECK(result.simulationFrames.size() == kFrameCount);
    return true;
}

struct PhysicsOutcome
{
    glm::vec3 position{};
    glm::vec3 linearVelocity{};
    glm::vec3 angularVelocity{};
    uint64_t physicsSteps = 0u;
    bool grounded = false;
    bool colliding = false;
};

bool CapturePhysicsOutcome(Engine::Renderers::DX11GameRenderer& renderer,
    uint32_t outputFps, PhysicsOutcome& outcome)
{
    using Engine::Components::PrimitiveObjectCollider;
    using Engine::Components::RigidBody;

    Engine::Scene::Scene scene;
    scene.Init(renderer.GetGraphicsProvider());
    Engine::Core::SceneManager::SetActiveScene(&scene);

    Engine::Core::Object* floor = scene.AddObject("Floor");
    floor->transform.position = glm::vec3(0.f, -0.5f, 0.f);
    RigidBody* floorBody = floor->AddComponent<RigidBody>();
    floorBody->bodyType = "Static";
    floorBody->useGravity = false;
    PrimitiveObjectCollider* floorCollider =
        floor->AddComponent<PrimitiveObjectCollider>();
    floorCollider->shape = "Box";
    floorCollider->size = glm::vec3(20.f, 1.f, 20.f);

    Engine::Core::Object* ball = scene.AddObject("Ball");
    ball->transform.position = glm::vec3(0.f, 4.f, 0.f);
    RigidBody* ballBody = ball->AddComponent<RigidBody>();
    ballBody->bodyType = "Dynamic";
    ballBody->mass = 1.f;
    ballBody->linearDamping = 0.f;
    ballBody->angularDamping = 0.f;
    ballBody->friction = 0.45f;
    ballBody->restitution = 0.35f;
    ballBody->continuousCollision = true;
    ballBody->initialLinearVelocity = glm::vec3(1.25f, 0.5f, -0.3f);
    ballBody->initialAngularVelocity = glm::vec3(0.2f, 1.1f, -0.4f);
    PrimitiveObjectCollider* ballCollider =
        ball->AddComponent<PrimitiveObjectCollider>();
    ballCollider->shape = "Sphere";
    ballCollider->radius = 0.5f;

    scene.SetFixedTimeStep(1.0 / static_cast<double>(outputFps));
    scene.GetPhysics().ConfigureFixedStep(1.0 / 120.0, 8u, 18u);
    CHECK(scene.GetPhysics().GetMaximumSubsteps() == 8u);
    CHECK(scene.GetPhysics().GetSolverIterations() == 18u);
    scene.Start();

    Engine::Rendering::ExportFrameSequence sequence(scene);
    const uint64_t lastOutputFrame = static_cast<uint64_t>(outputFps) * 2u;
    for (uint64_t frame = 0; frame <= lastOutputFrame; ++frame)
    {
        sequence.PrepareNextFrame();
        outcome.physicsSteps += scene.GetPhysics().GetLastSubstepCount();
    }
    CHECK(std::abs(scene.GetElapsedTime() - 2.0) < 1e-12);
    CHECK(outcome.physicsSteps == 240u);
    outcome.position = ball->transform.GetWorldPosition();
    outcome.linearVelocity = ballBody->GetLinearVelocity();
    outcome.angularVelocity = ballBody->GetAngularVelocity();
    outcome.grounded = ballBody->IsGrounded();
    outcome.colliding = ballBody->IsColliding();
    Engine::Core::SceneManager::SetActiveScene(nullptr);
    return true;
}

bool NearlyEqual(const glm::vec3& first, const glm::vec3& second,
    float tolerance = 1e-3f)
{
    return glm::length(first - second) <= tolerance;
}

bool Run(const std::string& scenePath)
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    Engine::Core::Window window(instance,
        L"Video Export Repeatability Test", kWidth, kHeight);
    Engine::Renderers::DX11GameRenderer renderer;
    CHECK(renderer.Init(window.GetHWND(), kWidth, kHeight));
    CHECK(renderer.EnableOffscreenExport(kWidth, kHeight, 1u, 3u));

    ExportResult first;
    ExportResult second;
    ExportResult third;
    CHECK(CaptureExport(renderer, scenePath, first));
    CHECK(CaptureExport(renderer, scenePath, second));
    CHECK(CaptureExport(renderer, scenePath, third));

    CHECK(first.sampleTimes.front() == 0.0);
    CHECK(first.simulationFrames.front() == 0u);
    for (uint64_t frame = 0; frame < kFrameCount; ++frame)
    {
        const double expected = static_cast<double>(frame) /
            static_cast<double>(kFps);
        CHECK(std::abs(first.sampleTimes[frame] - expected) < 1e-12);
        CHECK(first.simulationFrames[frame] == frame);
    }

    CHECK(first.sampleTimes == second.sampleTimes);
    CHECK(first.sampleTimes == third.sampleTimes);
    CHECK(first.simulationFrames == second.simulationFrames);
    CHECK(first.simulationFrames == third.simulationFrames);
    CHECK(first.frameHashes == second.frameHashes);
    CHECK(first.frameHashes == third.frameHashes);
    CHECK(std::set<uint64_t>(first.frameHashes.begin(),
        first.frameHashes.end()).size() > 1u);

    PhysicsOutcome at24Fps;
    PhysicsOutcome at30Fps;
    PhysicsOutcome at50Fps;
    PhysicsOutcome at60Fps;
    CHECK(CapturePhysicsOutcome(renderer, 24u, at24Fps));
    CHECK(CapturePhysicsOutcome(renderer, 30u, at30Fps));
    CHECK(CapturePhysicsOutcome(renderer, 50u, at50Fps));
    CHECK(CapturePhysicsOutcome(renderer, 60u, at60Fps));
    CHECK(NearlyEqual(at24Fps.position, at30Fps.position));
    CHECK(NearlyEqual(at24Fps.position, at60Fps.position));
    CHECK(NearlyEqual(at24Fps.position, at50Fps.position));
    constexpr float velocityTolerance = 0.02f;
    CHECK(NearlyEqual(at24Fps.linearVelocity, at30Fps.linearVelocity,
        velocityTolerance));
    CHECK(NearlyEqual(at24Fps.linearVelocity, at60Fps.linearVelocity,
        velocityTolerance));
    CHECK(NearlyEqual(at24Fps.linearVelocity, at50Fps.linearVelocity,
        velocityTolerance));
    CHECK(NearlyEqual(at24Fps.angularVelocity, at30Fps.angularVelocity,
        velocityTolerance));
    CHECK(NearlyEqual(at24Fps.angularVelocity, at60Fps.angularVelocity,
        velocityTolerance));
    CHECK(NearlyEqual(at24Fps.angularVelocity, at50Fps.angularVelocity,
        velocityTolerance));
    CHECK(at24Fps.grounded == at30Fps.grounded);
    CHECK(at24Fps.grounded == at50Fps.grounded);
    CHECK(at24Fps.grounded == at60Fps.grounded);
    CHECK(at24Fps.colliding == at30Fps.colliding);
    CHECK(at24Fps.colliding == at50Fps.colliding);
    CHECK(at24Fps.colliding == at60Fps.colliding);
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: VideoExportRepeatabilityTests <scene>\n");
        return 2;
    }
    if (!Run(argv[1]))
        return 1;
    std::printf("video export repeatability regression passed\n");
    return 0;
}
