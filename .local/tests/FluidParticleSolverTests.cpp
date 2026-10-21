#include "Core/Compoonents/Particles/FluidParticleSolver.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Scene/Scene.h"
#include "Core/Physics/Physics.h"
#include "Core/Object.h"
#include <glm/glm.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using Engine::Components::FluidParticleSolver;
using Engine::Components::ParticleEmitter;
using Engine::Components::ParticleState;

void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

bool Near(const glm::vec3& a, const glm::vec3& b, float tolerance = 1e-5f)
{
    return glm::length(a - b) <= tolerance;
}
}

int main()
{
    try
    {
        FluidParticleSolver::Settings settings;
        settings.boundsMin = {-0.5f, -0.5f, -0.5f};
        settings.boundsMax = {0.5f, 0.5f, 0.5f};
        std::vector<ParticleState> particles(27);
        for (size_t i = 0; i < particles.size(); ++i)
        {
            particles[i].id = i + 1;
            particles[i].position = {
                (static_cast<int>(i % 3) - 1) * .12f,
                (static_cast<int>((i / 3) % 3) - 1) * .12f + .15f,
                (static_cast<int>(i / 9) - 1) * .12f};
        }
        const auto initial = particles;
        FluidParticleSolver solver(settings);
        const Engine::Components::ParticleStepForces forces{{0.f, -9.81f, 0.f}, 0.1f};
        const auto simulate = [&]()
        {
            for (int tick = 0; tick < 240; ++tick)
            {
                solver.Step(particles.data(), particles.size(), 1.f / 60.f,
                    forces);
                for (const auto& particle : particles)
                {
                    Require(particle.active, "fluid particle lost");
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        Require(std::isfinite(particle.position[axis]) &&
                            std::isfinite(particle.velocity[axis]),
                            "fluid state became nonfinite");
                        Require(particle.position[axis] >=
                            settings.boundsMin[axis] - 1e-6f &&
                            particle.position[axis] <=
                            settings.boundsMax[axis] + 1e-6f,
                            "fluid particle escaped static bounds");
                    }
                    Require(glm::length(particle.velocity) <=
                        settings.maxSpeed + 1e-4f, "fluid speed limit failed");
                }
            }
        };
        simulate();
        Require(particles.size() == initial.size(), "fluid mass/count changed");
        Require(particles[13].position.y < initial[13].position.y,
            "fluid did not respond to gravity");
        const auto expected = particles;
        solver.Reset();
        particles = initial;
        simulate();
        for (size_t i = 0; i < particles.size(); ++i)
            Require(Near(particles[i].position, expected[i].position) &&
                Near(particles[i].velocity, expected[i].velocity) &&
                std::abs(particles[i].age - expected[i].age) < 1e-5f,
                "fluid reset replay changed state");

        settings.boundsMin = glm::vec3(-5.f);
        settings.boundsMax = glm::vec3(5.f);
        settings.pressureStiffness = 0.f;
        settings.viscosity = 0.f;
        std::vector<ParticleState> pair(2);
        pair[0].position.x = -.05f;
        pair[1].position.x = .05f;
        pair[0].velocity.x = 1.f;
        pair[1].velocity.x = -1.f;
        auto withoutViscosity = pair;
        FluidParticleSolver plain(settings);
        plain.Step(withoutViscosity.data(), 2, 1.f / 60.f, {});
        settings.viscosity = 0.1f;
        FluidParticleSolver viscous(settings);
        viscous.Step(pair.data(), 2, 1.f / 60.f, {});
        Require(std::abs(pair[0].velocity.x - pair[1].velocity.x) <
            std::abs(withoutViscosity[0].velocity.x -
                withoutViscosity[1].velocity.x),
            "viscosity did not reduce relative velocity");

        pair = std::vector<ParticleState>(2);
        pair[0].position.x = -.05f;
        pair[1].position.x = .05f;
        settings.viscosity = 0.f;
        settings.restDensity = 0.1f;
        settings.pressureStiffness = 8.f;
        FluidParticleSolver pressured(settings);
        pressured.Step(pair.data(), 2, 1.f / 60.f, {});
        Require(pair[0].velocity.x < 0.f && pair[1].velocity.x > 0.f,
            "SPH pressure did not separate neighbors");

        ParticleEmitter emitter;
        emitter.fluidEnabled = true;
        emitter.fluidVisualRadiusScale = 1.7f;
        emitter.fluidCouplingMode = static_cast<int>(
            ParticleEmitter::FluidCouplingMode::VisualOnly);
        emitter.fluid = settings;
        emitter.fluid.boundsMin = glm::vec3(-0.5f);
        emitter.fluid.boundsMax = glm::vec3(0.5f);
        emitter.burstOnStart = 12;
        emitter.maxParticles = 12;
        emitter.rate = 0.f;
        emitter.lifetime = 10.f;
        emitter.speed = 0.f;
        emitter.spreadDegrees = 0.f;
        emitter.shape = static_cast<int>(ParticleEmitter::Shape::Box);
        emitter.boxExtents = glm::vec3(0.2f);
        emitter.Restart();
        for (int tick = 0; tick < 120; ++tick)
            emitter.Advance(1.f / 60.f, glm::mat4(1.f));
        Require(emitter.GetParticles().size() == 12,
            "fluid emitter lost or duplicated particles");
        const auto emitterExpected = emitter.GetParticles();
        emitter.Restart();
        for (int tick = 0; tick < 120; ++tick)
            emitter.Advance(1.f / 60.f, glm::mat4(1.f));
        for (size_t i = 0; i < emitterExpected.size(); ++i)
            Require(emitter.GetParticles()[i].id == emitterExpected[i].id &&
                Near(emitter.GetParticles()[i].position,
                    emitterExpected[i].position),
                "fluid emitter restart is not reproducible");

        ParticleEmitter loaded;
        loaded.Deserialize(emitter.Serialize());
        Require(loaded.fluidEnabled &&
            std::abs(loaded.fluid.smoothingRadius -
                emitter.fluid.smoothingRadius) < 1e-6f &&
            std::abs(loaded.fluid.particleMass -
                emitter.fluid.particleMass) < 1e-6f &&
            std::abs(loaded.fluid.restDensity -
                emitter.fluid.restDensity) < 1e-6f &&
            std::abs(loaded.fluid.viscosity -
                emitter.fluid.viscosity) < 1e-6f &&
            loaded.fluid.substeps == emitter.fluid.substeps &&
            loaded.fluidVisual &&
            std::abs(loaded.fluidVisualRadiusScale -
                emitter.fluidVisualRadiusScale) < 1e-6f &&
            loaded.fluidCouplingMode == emitter.fluidCouplingMode &&
            Near(loaded.fluid.boundsMin, emitter.fluid.boundsMin) &&
            Near(loaded.fluid.boundsMax, emitter.fluid.boundsMax) &&
            loaded.GetParticles().empty(),
            "fluid settings serialization failed");

        const auto collideWithBody = [](int couplingMode)
        {
            Engine::Scene::Scene scene;
            auto* wall = scene.AddObject("Moving fluid boundary");
            wall->transform.position = {0.f, -0.5f, 0.f};
            auto* shape = wall->AddComponent<
                Engine::Components::PrimitiveObjectCollider>();
            shape->size = {2.f, 1.f, 2.f};
            auto* body = wall->AddComponent<Engine::Components::RigidBody>();
            body->bodyType = "Dynamic";
            body->mass = 1.f;
            body->useGravity = false;
            auto* source = scene.AddObject("Fluid source");
            source->transform.position = {0.f, 0.3f, 0.f};
            auto* fluidEmitter = source->AddComponent<ParticleEmitter>();
            fluidEmitter->fluidEnabled = true;
            fluidEmitter->fluidCouplingMode = couplingMode;
            fluidEmitter->fluid.boundsMin = glm::vec3(-5.f);
            fluidEmitter->fluid.boundsMax = glm::vec3(5.f);
            fluidEmitter->fluid.pressureStiffness = 0.f;
            fluidEmitter->fluid.viscosity = 0.f;
            fluidEmitter->fluid.substeps = 1;
            fluidEmitter->fluid.maxSpeed = 8.f;
            fluidEmitter->rate = 0.f;
            fluidEmitter->burstOnStart = 1;
            fluidEmitter->speed = 8.f;
            fluidEmitter->spreadDegrees = 0.f;
            fluidEmitter->direction = {0.f, -1.f, 0.f};
            fluidEmitter->gravity = {0.f, 0.f, 0.f};
            fluidEmitter->lifetime = 2.f;
            scene.Start();
            scene.GetPhysics().Step(1.f / 60.f);
            bool hit = false;
            for (int tick = 0; tick < 5; ++tick)
            {
                fluidEmitter->Advance(1.f / 60.f,
                    source->transform.GetWorldMatrix());
                hit = hit || !fluidEmitter->GetCollisionHits().empty();
            }
            Require(hit, "fluid did not collide with rigid boundary");
            Require(fluidEmitter->GetParticles().size() == 1 &&
                fluidEmitter->GetParticles()[0].velocity.y >= 0.f,
                "fluid did not bounce from rigid boundary");
            return body->GetLinearVelocity().y;
        };
        const float visualOnlyVelocity = collideWithBody(
            static_cast<int>(ParticleEmitter::FluidCouplingMode::VisualOnly));
        const float gameplayVelocity = collideWithBody(
            static_cast<int>(ParticleEmitter::FluidCouplingMode::GameplayImpulse));
        Require(std::abs(visualOnlyVelocity) < 1e-5f &&
            gameplayVelocity < -0.01f,
            "fluid coupling mode did not control rigid-body impulse");

        {
            Engine::Scene::Scene scene;
            auto* wall = scene.AddObject("Advancing boundary");
            wall->transform.position = {0.f, -0.5f, 0.f};
            auto* shape = wall->AddComponent<
                Engine::Components::PrimitiveObjectCollider>();
            shape->size = {2.f, 1.f, 2.f};
            auto* body = wall->AddComponent<Engine::Components::RigidBody>();
            body->bodyType = "Kinematic";
            auto* source = scene.AddObject("Stationary fluid");
            source->transform.position = {0.f, 0.2f, 0.f};
            auto* fluidEmitter = source->AddComponent<ParticleEmitter>();
            fluidEmitter->fluidEnabled = true;
            fluidEmitter->fluidCouplingMode = static_cast<int>(
                ParticleEmitter::FluidCouplingMode::VisualOnly);
            fluidEmitter->fluid.boundsMin = glm::vec3(-5.f);
            fluidEmitter->fluid.boundsMax = glm::vec3(5.f);
            fluidEmitter->fluid.pressureStiffness = 0.f;
            fluidEmitter->rate = 0.f;
            fluidEmitter->burstOnStart = 1;
            fluidEmitter->speed = 0.f;
            fluidEmitter->gravity = {0.f, 0.f, 0.f};
            scene.Start();
            scene.GetPhysics().Step(1.f / 60.f);
            body->SetLinearVelocity({0.f, 8.f, 0.f});
            fluidEmitter->Advance(1.f / 60.f,
                source->transform.GetWorldMatrix());
            fluidEmitter->Advance(1.f / 60.f,
                source->transform.GetWorldMatrix());
            Require(!fluidEmitter->GetCollisionHits().empty() &&
                fluidEmitter->GetParticles()[0].velocity.y > 0.f,
                "moving boundary did not push stationary fluid");
        }

        std::cout << "Fluid SPH, bounds, coupling, and reset pass\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
