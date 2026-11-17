#include "pch.h"
#include "Engine/Editor/Runtime/EditorPreviews.h"
#include "Core/Scene/Scene.h"
#include "Core/Scene/Object.h"
#include "Core/Components/Animation/AnimationManager.h"
#include "Core/Components/Particles/ParticleEmitter.h"
#include "Core/Threading/JobScheduler.h"
#include <algorithm>
#include <vector>

namespace Engine::Editor
{
bool AdvanceEditorPreviews(Engine::Scene::Scene& scene, float deltaTime)
{
    scene.AssertOwnerThread();
    const float seconds = std::max(deltaTime, 0.f);
    bool advanced = false;
    auto& jobs = Engine::Threading::JobScheduler::Get();
    std::vector<Engine::Threading::JobScheduler::Handle> phases;
    phases.reserve(4);
    try
    {
        phases.push_back(jobs.ScheduleOnOwner([&]
        {
            for (auto* emitter : scene.GetComponents<Engine::Components::ParticleEmitter>())
                if (emitter->Owner && emitter->Owner->IsEnabledInHierarchy() &&
                    emitter->IsEditorPreviewRunning())
                {
                    emitter->Advance(seconds, emitter->Owner->transform.GetWorldMatrix());
                    advanced = true;
                }
        }));
        phases.push_back(jobs.ScheduleOnOwner([&]
        {
            for (auto* animation : scene.GetComponents<Engine::Components::AnimationManager>())
                if (animation->Owner && animation->Owner->IsEnabledInHierarchy() &&
                    animation->IsEditorPreviewRunning())
                {
                    animation->Tick(seconds);
                    advanced = true;
                }
        }, {phases.back()}));
        phases.push_back(jobs.ScheduleOnOwner([&]
        {
            advanced = scene.UpdateSpatialScripts(true) || advanced;
        }, {phases.back()}));
        phases.push_back(jobs.ScheduleOnOwner([&]
        {
            scene.CommitPendingObjectChanges();
        }, {phases.back()}));
        jobs.Wait(phases.back());
    }
    catch (...)
    {
        // Every queued closure captures the scene and local state. Fence all
        // phases before either can be destroyed during exception unwinding.
        for (const auto& phase : phases)
            try { jobs.Wait(phase); } catch (...) {}
        throw;
    }
    return advanced;
}
}
