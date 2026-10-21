#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Particles/ParticleSolver.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <memory>
#include <iostream>

using Engine::Components::ParticleEmitter;

namespace
{
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

bool Near(const glm::vec3& a, const glm::vec3& b)
{
    return glm::length(a - b) < 1e-5f;
}

bool SameSettings(const ParticleEmitter& a, const ParticleEmitter& b)
{
    return a.rate == b.rate && a.burstOnStart == b.burstOnStart &&
        a.maxParticles == b.maxParticles && a.lifetime == b.lifetime &&
        a.speed == b.speed && a.spreadDegrees == b.spreadDegrees &&
        Near(a.direction, b.direction) && Near(a.gravity, b.gravity) &&
        a.drag == b.drag && Near(a.color, b.color) && a.size == b.size &&
        a.shape == b.shape && Near(a.boxExtents, b.boxExtents) &&
        a.sphereRadius == b.sphereRadius && a.worldSpace == b.worldSpace &&
        a.playOnStart == b.playOnStart && a.duration == b.duration &&
        a.loop == b.loop && a.seed == b.seed &&
        a.texturePath == b.texturePath && a.opacity == b.opacity &&
        a.blendMode == b.blendMode &&
        a.collisionMode == b.collisionMode &&
        a.collisionBounce == b.collisionBounce &&
        a.collisionFriction == b.collisionFriction &&
        a.collisionMask == b.collisionMask;
}

class KillParticleSolver final : public Engine::Components::IParticleSolver
{
public:
    explicit KillParticleSolver(int& resetCount) : m_resetCount(resetCount) {}
    void Reset() override { ++m_resetCount; }
    void Step(Engine::Components::ParticleState* particles, size_t count,
        float, const Engine::Components::ParticleStepForces&) override
    {
        for (size_t index = 0; index < count; ++index)
            particles[index].active = false;
    }
private:
    int& m_resetCount;
};

class TrackingParticleSolver final : public Engine::Components::IParticleSolver
{
public:
    explicit TrackingParticleSolver(std::vector<uint64_t>& removed)
        : m_removed(removed) {}
    void OnParticleRemoved(uint64_t id) override { m_removed.push_back(id); }
    void Step(Engine::Components::ParticleState* particles, size_t count,
        float seconds, const Engine::Components::ParticleStepForces& forces) override
    {
        m_ballistic.Step(particles, count, seconds, forces);
    }
private:
    std::vector<uint64_t>& m_removed;
    Engine::Components::BallisticParticleSolver m_ballistic;
};
}

int RunTests()
{
    ParticleEmitter emitter;
    emitter.rate = 0.f;
    emitter.burstOnStart = 3;
    emitter.maxParticles = 4;
    emitter.lifetime = 0.05f;
    emitter.speed = 0.f;
    emitter.gravity = {0.f, 0.f, 0.f};
    emitter.shape = static_cast<int>(ParticleEmitter::Shape::Box);
    emitter.boxExtents = {1.f, 2.f, 3.f};
    emitter.seed = 42;
    emitter.texturePath = "Textures/particle-test.png";
    emitter.opacity = 0.35f;
    emitter.blendMode = static_cast<int>(ParticleEmitter::BlendMode::Alpha);
    emitter.duration = 0.5f;
    emitter.loop = true;
    const glm::mat4 translated = glm::translate(glm::mat4(1.f), {10.f, 0.f, 0.f});

    emitter.Start();
    emitter.Advance(1.f / 60.f, translated);
    Require(emitter.GetParticles().size() == 3, "start burst count");
    auto first = emitter.GetParticles();
    for (const auto& particle : first)
    {
        Require(particle.position.x >= 9.f && particle.position.x <= 11.f,
            "world-space box emission");
        Require(particle.id >= 1 && particle.id <= 3, "stable IDs");
    }
    emitter.Burst(100);
    emitter.Advance(1.f / 60.f, translated);
    Require(emitter.GetParticles().size() == 4, "capacity bound");
    Require(emitter.GetParticles().back().id == 4, "next stable ID");
    emitter.Advance(3.f / 60.f, translated);
    Require(emitter.GetParticles().empty(), "expiry");

    emitter.Start();
    emitter.Advance(1.f / 60.f, translated);
    const auto& replay = emitter.GetParticles();
    Require(replay.size() == first.size(), "replay count");
    for (size_t i = 0; i < replay.size(); ++i)
        Require(replay[i].id == first[i].id &&
            Near(replay[i].position, first[i].position), "deterministic reset");

    const auto saved = emitter.Serialize();
    ParticleEmitter restored;
    restored.Deserialize(saved);
    Require(restored.rate == emitter.rate &&
        restored.burstOnStart == emitter.burstOnStart &&
        restored.shape == emitter.shape &&
        Near(restored.boxExtents, emitter.boxExtents) &&
        restored.seed == emitter.seed &&
        restored.texturePath == emitter.texturePath &&
        restored.opacity == emitter.opacity &&
        restored.blendMode == emitter.blendMode &&
        restored.duration == emitter.duration &&
        restored.loop == emitter.loop, "serialized settings");
    Require(restored.GetParticles().empty(), "runtime state not serialized");

    emitter.worldSpace = false;
    emitter.Start();
    emitter.Advance(1.f / 60.f, translated);
    for (const auto& particle : emitter.GetParticles())
        Require(particle.position.x >= -1.f && particle.position.x <= 1.f,
            "local-space box emission");
    const auto bounds = emitter.GetWorldBounds(translated);
    Require(bounds.valid, "particle bounds available");
    for (const auto& particle : emitter.GetParticles())
    {
        const glm::vec3 worldPosition = particle.position + glm::vec3(10.f, 0.f, 0.f);
        Require(glm::all(glm::lessThanEqual(bounds.minimum, worldPosition)) &&
            glm::all(glm::greaterThanEqual(bounds.maximum, worldPosition)),
            "world bounds contain local-space particles");
    }

    std::vector<Engine::Components::ParticleState> solverParticles(1);
    solverParticles[0].force = {0.f, 6.f, 0.f};
    Engine::Components::BallisticParticleSolver ballistic;
    ballistic.Step(solverParticles.data(), solverParticles.size(), 1.f / 60.f,
        { {0.f, -3.f, 0.f}, 0.f });
    Require(std::abs(solverParticles[0].velocity.y - 0.05f) < 1e-5f &&
        Near(solverParticles[0].force, {0.f, 0.f, 0.f}) &&
        solverParticles[0].age > 0.f, "solver force and age update");

    int solverResets = 0;
    emitter.SetSolver(std::make_unique<KillParticleSolver>(solverResets));
    emitter.Start();
    emitter.Advance(1.f / 60.f, translated);
    Require(emitter.GetParticles().size() == 3, "custom solver spawn");
    emitter.Advance(1.f / 60.f, translated);
    Require(emitter.GetParticles().empty() && solverResets >= 2,
        "custom solver expiry and reset");

    ParticleEmitter finite;
    finite.rate = 0.f;
    finite.burstOnStart = 1;
    finite.lifetime = 10.f;
    finite.speed = 0.f;
    finite.gravity = {0.f, 0.f, 0.f};
    finite.worldSpace = false;
    finite.duration = 2.f / 60.f;
    finite.Start();
    finite.Advance(2.f / 60.f, translated);
    Require(finite.GetParticles().size() == 1 && finite.IsPlaying(),
        "finite emission window");
    finite.Advance(1.f / 60.f, translated);
    Require(finite.GetParticles().size() == 1 && !finite.IsPlaying(),
        "finite emission stops while particles remain alive");
    finite.Play();
    finite.Advance(1.f / 60.f, translated);
    Require(finite.GetParticles().size() == 2 &&
        finite.GetParticles().back().id == 2, "play starts another emission cycle");

    finite.loop = true;
    finite.Start();
    finite.Advance(3.f / 60.f, translated);
    Require(finite.GetParticles().size() == 2, "loop repeats start burst");
    finite.Pause();
    finite.Advance(2.f / 60.f, translated);
    Require(finite.GetParticles().size() == 2, "pause holds emission clock");
    finite.Play();
    finite.Advance(2.f / 60.f, translated);
    Require(finite.GetParticles().size() == 3, "loop resumes from paused clock");
    finite.Reset();
    Require(!finite.GetWorldBounds(translated).valid, "empty emitter has no bounds");

    finite.playOnStart = false;
    finite.Restart();
    finite.Advance(1.f / 60.f, translated);
    Require(finite.IsPlaying() && finite.GetParticles().size() == 1,
        "restart previews an emitter with play-on-start disabled");

    ParticleEmitter manualStart;
    manualStart.rate = 0.f;
    manualStart.burstOnStart = 2;
    manualStart.speed = 0.f;
    manualStart.gravity = {0.f, 0.f, 0.f};
    manualStart.playOnStart = false;
    manualStart.Start();
    manualStart.Advance(1.f / 60.f, translated);
    Require(manualStart.GetParticles().empty(),
        "play-on-start disabled holds the initial burst");
    manualStart.Play();
    manualStart.Pause();
    manualStart.Play();
    manualStart.Advance(1.f / 60.f, translated);
    Require(manualStart.GetParticles().size() == 2,
        "manual play queues the start burst exactly once");

    Engine::Scene::Scene scene;
    auto* editorCamera = scene.editorCamera.AddComponent<Engine::Components::Camera>();
    auto* particleObject = scene.AddObject("Focus particles");
    auto* focusEmitter = particleObject->AddComponent<ParticleEmitter>();
    focusEmitter->rate = 0.f;
    focusEmitter->burstOnStart = 1;
    focusEmitter->speed = 60.f;
    focusEmitter->spreadDegrees = 0.f;
    focusEmitter->direction = {1.f, 0.f, 0.f};
    focusEmitter->gravity = {0.f, 0.f, 0.f};
    focusEmitter->Start();
    focusEmitter->Advance(2.f / 60.f, glm::mat4(1.f));
    scene.FocusEditorCamera(particleObject);
    Require(std::abs(editorCamera->target.x - 1.f) < 1e-4f,
        "editor camera focuses live particle bounds");

    ParticleEmitter replayEmitter;
    replayEmitter.rate = 37.f;
    replayEmitter.burstOnStart = 3;
    replayEmitter.maxParticles = 64;
    replayEmitter.lifetime = 4.f;
    replayEmitter.speed = 2.f;
    replayEmitter.gravity = {0.f, -1.f, 0.f};
    replayEmitter.shape = static_cast<int>(ParticleEmitter::Shape::Sphere);
    replayEmitter.seed = 91;
    replayEmitter.duration = .4f;
    replayEmitter.loop = true;
    replayEmitter.Start();
    for (int step = 0; step < 120; ++step)
        replayEmitter.Advance(1.f / 60.f, translated);
    const auto expected = replayEmitter.GetParticles();
    Require(replayEmitter.GetSimulationTicks() == 120 && !expected.empty(),
        "normal playback advances fixed ticks");
    Require(replayEmitter.Seek(.5, translated) &&
        replayEmitter.GetSimulationTicks() == 30,
        "backward seek resets and replays fixed ticks");
    Require(replayEmitter.Seek(2.0, translated) &&
        replayEmitter.GetSimulationTicks() == 120,
        "forward seek replays fixed ticks");
    const auto matchesExpected = [&]()
    {
        const auto& actual = replayEmitter.GetParticles();
        if (actual.size() != expected.size()) return false;
        for (size_t index = 0; index < actual.size(); ++index)
            if (actual[index].id != expected[index].id ||
                !Near(actual[index].position, expected[index].position) ||
                !Near(actual[index].velocity, expected[index].velocity) ||
                std::abs(actual[index].age - expected[index].age) > 1e-5f)
                return false;
        return true;
    };
    Require(matchesExpected(), "seek reproduces seeded particle state and IDs");
    Require(!replayEmitter.Seek(601.0, translated) &&
        replayEmitter.GetSimulationTicks() == 120 && matchesExpected(),
        "out-of-range seek preserves state");
    replayEmitter.Pause();
    replayEmitter.Play();
    Require(replayEmitter.GetSimulationTicks() == 120 && matchesExpected(),
        "play resumes without resetting live state");

    const auto sceneSaved = replayEmitter.Serialize();
    replayEmitter.Deserialize(sceneSaved);
    Require(replayEmitter.GetSimulationTicks() == 0 &&
        replayEmitter.GetParticles().empty() && !replayEmitter.IsPlaying(),
        "scene reload clears runtime particle state");
    replayEmitter.Start();
    for (int step = 0; step < 120; ++step)
        replayEmitter.Advance(1.f / 60.f, translated);
    Require(matchesExpected(), "scene reload replays the same IDs and state");

    ParticleEmitter moving;
    moving.rate = 0.f;
    moving.burstOnStart = 1;
    moving.lifetime = 10.f;
    moving.speed = 0.f;
    moving.gravity = {0.f, 0.f, 0.f};
    moving.Start();
    for (int tick = 1; tick <= 3; ++tick)
        moving.Advance(1.f / 60.f, glm::translate(glm::mat4(1.f),
            glm::vec3(static_cast<float>(tick), 0.f, 0.f)));
    Require(moving.GetParticles().size() == 1 &&
        std::abs(moving.GetParticles()[0].position.x - 1.f) < 1e-5f,
        "moving emitter spawns at its first tick transform");
    const auto movingExpected = moving.GetParticles()[0];
    int sampledTicks = 0;
    Require(moving.Seek(3.0 / 60.0, [&](double time)
    {
        ++sampledTicks;
        return glm::translate(glm::mat4(1.f),
            glm::vec3(static_cast<float>(time * 60.0), 0.f, 0.f));
    }) && sampledTicks == 3, "seek samples the moving emitter per tick");
    Require(moving.GetParticles().size() == 1 &&
        moving.GetParticles()[0].id == movingExpected.id &&
        Near(moving.GetParticles()[0].position, movingExpected.position) &&
        std::abs(moving.GetParticles()[0].age - movingExpected.age) < 1e-5f,
        "moving emitter seek reproduces spawned state");

    Engine::Scene::Scene prefabScene;
    auto* source = prefabScene.AddObject("Emitter prefab source");
    auto* configured = source->AddComponent<ParticleEmitter>();
    configured->rate = 19.5f;
    configured->burstOnStart = 5;
    configured->maxParticles = 23;
    configured->lifetime = 3.25f;
    configured->speed = 7.5f;
    configured->spreadDegrees = 38.f;
    configured->direction = {0.f, 0.f, 1.f};
    configured->gravity = {1.f, -2.f, 3.f};
    configured->drag = 0.4f;
    configured->color = {0.2f, 0.4f, 0.8f};
    configured->size = 0.3f;
    configured->shape = static_cast<int>(ParticleEmitter::Shape::Sphere);
    configured->boxExtents = {1.f, 2.f, 3.f};
    configured->sphereRadius = 2.5f;
    configured->worldSpace = false;
    configured->playOnStart = false;
    configured->duration = 1.5f;
    configured->loop = true;
    configured->seed = 341;
    configured->texturePath = "Textures/particle-preset.png";
    configured->opacity = 0.7f;
    configured->blendMode = static_cast<int>(ParticleEmitter::BlendMode::Alpha);
    configured->collisionMode = static_cast<int>(ParticleEmitter::CollisionMode::Bounce);
    configured->collisionBounce = 0.8f;
    configured->collisionFriction = 0.3f;
    configured->collisionMask = 7;
    configured->Restart();
    configured->Advance(1.f / 60.f, translated);
    Require(!configured->GetParticles().empty(), "prefab source has runtime state");

    ParticleEmitter presetCopy;
    presetCopy.Deserialize(configured->Serialize());
    Require(SameSettings(*configured, presetCopy) &&
        presetCopy.GetParticles().empty(), "all preset settings round-trip without runtime state");

    const auto prefabPath = std::filesystem::temp_directory_path() /
        ("engine-particle-emitter-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".prefab");
    struct RemovePrefab
    {
        std::filesystem::path path;
        ~RemovePrefab() { std::error_code error; std::filesystem::remove(path, error); }
    } removePrefab{prefabPath};
    Require(Engine::Serialization::SceneSerializer::SavePrefab(*source,
        prefabPath.string()), "save emitter prefab");
    Engine::Scene::Scene instantiatedScene;
    auto* instance = Engine::Serialization::SceneSerializer::InstantiatePrefab(
        instantiatedScene, prefabPath.string());
    auto* prefabEmitter = instance ? instance->GetComponent<ParticleEmitter>() : nullptr;
    Require(prefabEmitter && SameSettings(*configured, *prefabEmitter) &&
        prefabEmitter->GetParticles().empty(),
        "prefab preserves emitter settings without runtime particles");

    const std::string savedScene = Engine::Serialization::SceneSerializer::SaveToString(
        instantiatedScene);
    Engine::Scene::Scene reloadedScene;
    Require(Engine::Serialization::SceneSerializer::LoadFromString(
        reloadedScene, savedScene), "reload scene with emitter prefab");
    auto* sceneObject = reloadedScene.FindObjectByName("Emitter prefab source");
    auto* sceneEmitter = sceneObject ? sceneObject->GetComponent<ParticleEmitter>() : nullptr;
    Require(sceneEmitter && SameSettings(*configured, *sceneEmitter) &&
        sceneEmitter->GetParticles().empty(),
        "scene reload preserves emitter prefab settings");

    Engine::Scene::Scene budgetScene;
    budgetScene.SetParticleBudget(3);
    auto* firstObject = budgetScene.AddObject("First budget emitter");
    auto* firstEmitter = firstObject->AddComponent<ParticleEmitter>();
    firstEmitter->rate = 0.f;
    firstEmitter->burstOnStart = 2;
    firstEmitter->lifetime = 0.05f;
    firstEmitter->speed = 0.f;
    firstEmitter->gravity = {0.f, 0.f, 0.f};
    std::vector<uint64_t> firstRemoved;
    firstEmitter->SetSolver(std::make_unique<TrackingParticleSolver>(firstRemoved));
    auto* secondObject = budgetScene.AddObject("Second budget emitter");
    auto* secondEmitter = secondObject->AddComponent<ParticleEmitter>();
    secondEmitter->rate = 0.f;
    secondEmitter->burstOnStart = 2;
    secondEmitter->lifetime = 10.f;
    secondEmitter->speed = 0.f;
    secondEmitter->gravity = {0.f, 0.f, 0.f};
    std::vector<uint64_t> secondRemoved;
    secondEmitter->SetSolver(std::make_unique<TrackingParticleSolver>(secondRemoved));
    budgetScene.Start();
    budgetScene.Update(1.f / 60.f);
    Require(budgetScene.GetParticleLiveCount() == 3 &&
        firstEmitter->GetParticles().size() == 2 &&
        secondEmitter->GetParticles().size() == 1,
        "scene budget drops excess spawns in object order");
    Require(!firstEmitter->AddParticleForce(999, {1.f, 0.f, 0.f}) &&
        firstEmitter->AddParticleForce(1, {30.f, 0.f, 0.f}) &&
        firstEmitter->AddParticleForce(1, {30.f, 0.f, 0.f}),
        "force accumulation targets a stable live ID");
    budgetScene.Update(1.f / 60.f);
    Require(std::abs(firstEmitter->GetParticles()[0].velocity.x - 1.f) < 1e-5f &&
        Near(firstEmitter->GetParticles()[0].force, {0.f, 0.f, 0.f}) &&
        firstEmitter->GetWorldBounds(glm::mat4(1.f)).maximum.x > 0.f,
        "solver consumes force and live bounds reflect integration");
    budgetScene.SetParticleBudget(2);
    Require(budgetScene.GetParticleLiveCount() == 2 &&
        secondEmitter->GetParticles().empty() &&
        secondRemoved == std::vector<uint64_t>{1},
        "lowering scene budget trims later emitters and retires solver state");
    budgetScene.Update(2.f / 60.f);
    Require(firstEmitter->GetParticles().empty() &&
        firstRemoved == (std::vector<uint64_t>{1, 2}) &&
        !firstEmitter->GetWorldBounds(glm::mat4(1.f)).valid,
        "expiry recycles dense slots and invalidates empty bounds");
    firstEmitter->Burst(2);
    budgetScene.Update(1.f / 60.f);
    Require(firstEmitter->GetParticles().size() == 2 &&
        firstEmitter->GetParticles()[0].id == 3 &&
        firstEmitter->GetParticles()[1].id == 4,
        "recycled slots receive new stable IDs");
    budgetScene.SetParticleBudget(4);
    budgetScene.Update(1.f / 60.f);
    Require(secondEmitter->GetParticles().empty(),
        "dropped burst particles do not queue for a later budget increase");
    secondEmitter->Burst(2);
    budgetScene.Update(1.f / 60.f);
    Require(budgetScene.GetParticleLiveCount() == 4 &&
        secondEmitter->GetParticles().size() == 2 &&
        secondEmitter->GetParticles()[0].id == 2,
        "released scene budget admits later bursts without backlog");

    Engine::Scene::Scene collisionScene;
    auto* floor = collisionScene.AddObject("Floor");
    floor->transform.position = {0.f, -0.5f, 0.f};
    auto* floorShape = floor->AddComponent<Engine::Components::PrimitiveObjectCollider>();
    floorShape->size = {10.f, 1.f, 10.f};
    auto* floorBody = floor->AddComponent<Engine::Components::RigidBody>();
    floorBody->bodyType = "Static";
    auto* drop = collisionScene.AddObject("Drop");
    drop->transform.position = {0.f, 1.f, 0.f};
    auto* collisionEmitter = drop->AddComponent<ParticleEmitter>();
    collisionEmitter->rate = 0.f;
    collisionEmitter->burstOnStart = 1;
    collisionEmitter->speed = 120.f;
    collisionEmitter->spreadDegrees = 0.f;
    collisionEmitter->direction = {0.f, -1.f, 0.f};
    collisionEmitter->gravity = {0.f, 0.f, 0.f};
    collisionEmitter->collisionMode = static_cast<int>(ParticleEmitter::CollisionMode::Kill);
    collisionScene.Start();
    collisionScene.GetPhysics().Step(1.f / 60.f);
    collisionEmitter->Advance(1.f / 60.f, drop->transform.GetWorldMatrix());
    collisionEmitter->Advance(1.f / 60.f, drop->transform.GetWorldMatrix());
    Require(collisionEmitter->GetParticles().empty() &&
        collisionEmitter->GetCollisionHits().size() == 1 &&
        collisionEmitter->GetCollisionHits()[0].particleId == 1 &&
        collisionEmitter->GetCollisionHits()[0].object == floor &&
        collisionEmitter->GetCollisionHits()[0].killed,
        "kill on contact reports a stable particle ID and collider");
    collisionEmitter->collisionMode = static_cast<int>(ParticleEmitter::CollisionMode::Bounce);
    collisionEmitter->collisionBounce = 0.5f;
    collisionEmitter->collisionFriction = 0.f;
    collisionEmitter->Restart();
    collisionEmitter->Advance(2.f / 60.f, drop->transform.GetWorldMatrix());
    Require(collisionEmitter->GetParticles().size() == 1 &&
        collisionEmitter->GetCollisionHits().size() == 1 &&
        !collisionEmitter->GetCollisionHits()[0].killed &&
        collisionEmitter->GetParticles()[0].velocity.y > 50.f,
        "bounce reflects velocity and retains the particle");
    collisionEmitter->Advance(1.f / 60.f, drop->transform.GetWorldMatrix());
    Require(collisionEmitter->GetCollisionHits().empty(),
        "hit batch is replaced at the next advance");
    collisionEmitter->worldSpace = false;
    collisionEmitter->collisionMask = 0;
    collisionEmitter->Restart();
    collisionEmitter->Advance(2.f / 60.f, drop->transform.GetWorldMatrix());
    Require(collisionEmitter->GetCollisionHits().empty() &&
        collisionEmitter->GetParticles()[0].position.y < -1.f,
        "collision mask can exclude scene colliders");
    collisionEmitter->collisionMask = -1;
    collisionEmitter->Restart();
    collisionEmitter->Advance(2.f / 60.f, drop->transform.GetWorldMatrix());
    Require(collisionEmitter->GetCollisionHits().size() == 1 &&
        collisionEmitter->GetParticles()[0].velocity.y > 50.f &&
        collisionEmitter->GetParticles()[0].position.y < 0.f,
        "local-space response converts the world hit back to emitter space");

    Engine::Scene::Scene enclosure;
    const auto addWall = [&enclosure](const char* name,
        const glm::vec3& position, const glm::vec3& size)
    {
        auto* wall = enclosure.AddObject(name);
        wall->transform.position = position;
        auto* shape = wall->AddComponent<Engine::Components::PrimitiveObjectCollider>();
        shape->size = size;
        auto* body = wall->AddComponent<Engine::Components::RigidBody>();
        body->bodyType = "Static";
        return wall;
    };
    Engine::Core::Object* walls[] = {
        addWall("Floor", {0.f, -0.25f, 0.f}, {6.5f, 0.5f, 6.5f}),
        addWall("Ceiling", {0.f, 5.25f, 0.f}, {6.5f, 0.5f, 6.5f}),
        addWall("Left", {-3.25f, 2.5f, 0.f}, {0.5f, 5.f, 6.5f}),
        addWall("Right", {3.25f, 2.5f, 0.f}, {0.5f, 5.f, 6.5f}),
        addWall("Front", {0.f, 2.5f, -3.25f}, {6.5f, 5.f, 0.5f}),
        addWall("Back", {0.f, 2.5f, 3.25f}, {6.5f, 5.f, 0.5f})
    };
    auto* enclosureSource = enclosure.AddObject("Contained Particles");
    enclosureSource->transform.position = {0.f, 2.5f, 0.f};
    auto* contained = enclosureSource->AddComponent<ParticleEmitter>();
    contained->rate = 0.f;
    contained->burstOnStart = 256;
    contained->maxParticles = 256;
    contained->lifetime = 5.f;
    contained->speed = 10.f;
    contained->spreadDegrees = 180.f;
    contained->gravity = {0.f, 0.f, 0.f};
    contained->collisionMode = static_cast<int>(ParticleEmitter::CollisionMode::Bounce);
    contained->collisionBounce = 0.95f;
    contained->collisionFriction = 0.02f;
    enclosure.Start();
    enclosure.GetPhysics().Step(1.f / 60.f);
    size_t wallHits[6]{};
    for (int tick = 0; tick < 240; ++tick)
    {
        contained->Advance(1.f / 60.f, enclosureSource->transform.GetWorldMatrix());
        for (const auto& hit : contained->GetCollisionHits())
        {
            for (size_t wall = 0; wall < 6; ++wall)
                if (hit.object == walls[wall]) ++wallHits[wall];
        }
        for (const auto& particle : contained->GetParticles())
        {
            const glm::vec3 p = particle.position;
            const bool inside = std::abs(p.x) <= 3.01f &&
                std::abs(p.z) <= 3.01f && p.y >= -0.01f && p.y <= 5.01f;
            if (!inside)
                std::cerr << "escape tick=" << tick << " id=" << particle.id
                    << " position=" << p.x << ',' << p.y << ',' << p.z
                    << " velocity=" << particle.velocity.x << ','
                    << particle.velocity.y << ',' << particle.velocity.z << '\n';
            Require(inside,
                "bouncing particle escaped the enclosed room");
        }
    }
    Require(contained->GetParticles().size() == 256,
        "enclosure retains live particles before their lifetime expires");
    for (size_t hits : wallHits)
        Require(hits > 0, "particles must collide with every enclosure face");
    return 0;
}

int main()
{
    try { return RunTests(); }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
