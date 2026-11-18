#include "pch.h"
#include "Core/Rendering/RenderSystem.h"
#include "Core/Rendering/SceneRenderer.h"
#include "Core/Runtime/Game.h"
#include "Core/Runtime/AsyncSceneReader.h"
#include "Core/Scene/Scene.h"
#include "Core/Scene/Object.h"
#include "Core/Audio/Audio.h"
#include "Core/Physics/Physics.h"
#include "Core/Components/Animation/RigConstraints.h"
#include "Core/Components/Animation/RigMorphControls.h"
#include "Core/Components/Animation/SoftTissueSurface.h"
#include "Core/Components/Physics/RigPhysicsBinding.h"
#include "Core/Components/Physics/RigidBody.h"
#include "Core/Components/Physics/SpatialManipulator.h"
#include "Core/Renderers/UIRenderer.h"
#include "Core/Input/NativeInputSnapshot.h"
#include "Core/Threading/ComponentUpdateScheduler.h"
#include "Core/Threading/JobScheduler.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <typeinfo>
#include <unordered_set>
#include <vector>

namespace Engine::Runtime
{
Game::Game(Scene& scene)
    : m_scene(scene)
    , m_audio(std::make_unique<Engine::Audio::Audio>(scene))
    , m_sceneReader(std::make_unique<AsyncSceneReader>())
{
    m_scene.AssertOwnerThread();
    m_scene.BindGame(this);
    m_scene.AddObserver(this);
}

Game::~Game()
{
    // Joins the reader worker before the scene can be released.
    m_sceneReader.reset();
    // Sources stop with the simulation that played them.
    m_audio->Reset();
    m_scene.RemoveObserver(this);
    if (m_scene.GetGame() == this)
        m_scene.BindGame(nullptr);
}

void Game::OnObjectsClearing()
{
    m_audio->Reset();
}

bool Game::LoadScene(const std::string& path)
{
    m_scene.AssertOwnerThread();
    m_sceneReader->Cancel();
    if (!m_scene.Load(path))
        return false;
    if (m_onSceneLoaded)
        m_onSceneLoaded(&m_scene);
    return true;
}

void Game::RequestSceneLoad(const std::string& path)
{
    if (path.empty()) return;
    m_scene.AssertOwnerThread();
    LoadSceneAsync(path, {});
}

bool Game::RequestDefaultSceneLoad()
{
    if (m_defaultScenePath.empty())
        return false;
    RequestSceneLoad(m_defaultScenePath);
    return true;
}

void Game::LoadSceneAsync(const std::string& path,
    std::function<void(Scene*)> onComplete)
{
    if (path.empty())
    {
        if (onComplete) onComplete(nullptr);
        return;
    }
    m_scene.AssertOwnerThread();
    m_sceneReader->Submit(path, std::move(onComplete), m_loadGeneration,
        m_scene.CaptureSceneGeneration());
}

void Game::CancelPendingSceneLoad()
{
    m_scene.AssertOwnerThread();
    ++m_loadGeneration;
    m_sceneReader->Cancel();
}

bool Game::ProcessPendingSceneLoad()
{
    m_scene.AssertOwnerThread();
    auto result = m_sceneReader->TakeCompleted();
    if (!result || result->generation != m_loadGeneration ||
        result->sceneGeneration != m_scene.CaptureSceneGeneration())
        return false;
    Scene* loaded = nullptr;
    if (result->readSucceeded && m_scene.LoadFromValue(result->document))
    {
        Start();
        loaded = &m_scene;
        if (m_onSceneLoaded)
            m_onSceneLoaded(loaded);
    }
    if (result->callback)
        result->callback(loaded);
    return loaded != nullptr;
}

bool Game::HasStarted() const
{
    return m_scene.HasStarted();
}

void Game::SetFixedTimeStep(double seconds)
{
    m_scene.AssertOwnerThread();
    m_clock.SetFixedStep(seconds);
}

void Game::UseVariableTimeStep()
{
    m_scene.AssertOwnerThread();
    m_clock.UseVariableStep();
}

void Game::AdvanceManualFrame(float deltaTime)
{
    m_scene.AssertOwnerThread();
    m_deltaTime = m_clock.Advance(deltaTime);
}

void Game::UpdateFixedFrame()
{
    m_scene.AssertOwnerThread();
    if (!m_clock.IsFixedStep())
        throw std::logic_error("UpdateFixedFrame requires a configured fixed time step");
    Update(0.f);
}

void Game::Start()
{
    m_scene.AssertOwnerThread();
    m_scene.FenceBackgroundWork();
    m_clock.Reset();
    m_deltaTime = 0.f;
    const auto& objects = m_scene.GetObjects();
    for (const auto& object : objects)
        if (object)
            object->transform.ClearEditorOverride();
    // Component Start() can remove preview objects (terrain does this when
    // switching to its runtime bake), invalidating a scene object iterator.
    std::vector<Engine::Core::Object*> startingObjects;
    startingObjects.reserve(objects.size());
    for (const auto& object : objects)
        if (object)
            startingObjects.push_back(object.get());
    for (Engine::Core::Object* object : startingObjects)
        if (std::any_of(m_scene.GetObjects().begin(), m_scene.GetObjects().end(),
            [object](const auto& current) { return current.get() == object; }))
            object->Start();
    m_scene.MarkStarted();
    m_scene.CommitPendingObjectChanges();
}

void Game::Update(float deltaTime)
{
    const bool profiling = m_scene.IsUpdateProfilingEnabled();
    m_scene.AssertOwnerThread();
    using Clock = std::chrono::steady_clock;
    const auto updateBegin = profiling ? Clock::now() : Clock::time_point{};
    m_deltaTime = m_clock.Advance(deltaTime);
    m_scene.EnforceParticleBudget();
    m_scene.BeginUpdatePass();
    struct UpdatingGuard
    {
        Scene& scene;
        ~UpdatingGuard() { scene.EndUpdatePass(); }
    } updatingGuard{m_scene};
    const double physicsDelta = m_clock.IsFixedStep()
        ? m_clock.GetFixedStep() : static_cast<double>(m_deltaTime);
    Clock::time_point afterObjects{}, afterConstraints{}, afterPose{}, afterPhysics{};
    const auto componentUpdates = [&]
    {
    const auto updateObjects = m_scene.GetUpdateObjects(); // callbacks may change the registry
    std::vector<Engine::Threading::ComponentUpdateScheduler::Node> parallelUpdates;
    const auto finishParallelUpdates = [&]
    {
        Engine::Threading::ComponentUpdateScheduler::Run(parallelUpdates);
        parallelUpdates.clear();
    };
    for (auto* object : updateObjects)
    {
        // Object subclasses may override Update and must retain their exact
        // owner-thread dispatch. A legacy component makes its whole object
        // serial so callbacks can still edit the live component list.
        if (typeid(*object) != typeid(Engine::Core::Object))
        {
            finishParallelUpdates();
            object->Update();
            continue;
        }
        if (!object->IsEnabledInHierarchy()) continue;
        // Worker-safe objects contribute their component updates to the
        // current parallel segment; otherwise the object updates serially.
        if (!object->CollectWorkerUpdates(parallelUpdates))
        {
            finishParallelUpdates();
            object->Update();
        }
    }
    finishParallelUpdates();
    };
    const auto spatialScripts = [&]
    {
        m_scene.UpdateSpatialScripts();
        afterObjects = profiling ? Clock::now() : Clock::time_point{};
    };
    const auto transforms = [&]
    {
        m_scene.PrepareTransformHierarchy();
    };
    const auto animationConstraints = [&]
    {
        m_scene.ApplyPoseConstraints();
        afterConstraints = profiling ? Clock::now() : Clock::time_point{};
        m_audio->Update(m_deltaTime);
        afterPose = profiling ? Clock::now() : Clock::time_point{};
    };
    const auto physicsPreparation = [&] { m_scene.GetPhysics().PrepareStep(physicsDelta); };
    const auto physicsSimulation = [&] { m_scene.GetPhysics().SimulatePreparedStep(); };
    const auto postPhysics = [&]
    {
    m_scene.GetPhysics().FinishPreparedStep();
    afterPhysics = profiling ? Clock::now() : Clock::time_point{};
    for (auto* binding : m_scene.GetComponents<Engine::Components::RigPhysicsBinding>())
        if (binding->Owner->IsEnabledInHierarchy()) binding->PostPhysicsUpdate();
    for (auto* constraints : m_scene.GetComponents<Engine::Components::RigConstraints>())
        if (constraints->Owner->IsEnabledInHierarchy())
        { constraints->RefreshPoseOutputs(); constraints->PostPhysicsUpdate(); }
    for (auto* morphs : m_scene.GetComponents<Engine::Components::RigMorphControls>())
        if (morphs->Owner->IsEnabledInHierarchy() && !morphs->Owner->GetComponent<Engine::Components::RigConstraints>())
            morphs->RefreshPoseOutputs();
    // Bullet has now published current-frame poses and overlap pairs. Portal
    // traversal deliberately runs here rather than in Component::Update so a
    // crossing is evaluated against the motion that just occurred.
    struct OrderedPortal
    {
        Engine::Components::SpatialManipulator* manipulator = nullptr;
        Scene::ObjectPath path;
    };
    std::vector<OrderedPortal> portals;
    for (auto* manipulator : m_scene.GetComponents<Engine::Components::SpatialManipulator>())
        if (manipulator->Owner->IsEnabledInHierarchy())
        {
            Scene::ObjectPath path;
            m_scene.TryGetObjectPath(manipulator->Owner, path);
            portals.push_back({manipulator, std::move(path)});
        }
    std::sort(portals.begin(), portals.end(),
        [](const OrderedPortal& first, const OrderedPortal& second)
        {
            if (first.manipulator->portalTraversalPriority !=
                second.manipulator->portalTraversalPriority)
            {
                return first.manipulator->portalTraversalPriority >
                    second.manipulator->portalTraversalPriority;
            }
            return first.path < second.path;
        });
    std::unordered_set<const Engine::Components::RigidBody*> claimedBodies;
    for (const OrderedPortal& portal : portals)
        portal.manipulator->PostPhysicsUpdate(&claimedBodies);
    // Contacts and final bone/portal poses are available only after physics.
    for (auto* tissue : m_scene.GetComponents<Engine::Components::SoftTissueSurface>())
        if (tissue->Owner->IsEnabledInHierarchy())
        {
            for (const auto* body : claimedBodies)
                if (body && tissue->IsBoundUnder(body->Owner)) { tissue->Reset(); break; }
            tissue->PostPhysicsUpdate(static_cast<float>(physicsDelta));
        }
    if (const auto* input = Engine::Core::CurrentNativeInputSnapshot())
    {
        if (input->clientWidth && input->clientHeight)
            Engine::Renderers::UIRenderer::UpdateNativeInteraction(m_scene, *input);
    }
    };
    const auto publishRenderSnapshot = [&]
    {
    m_scene.EndUpdatePass();
    m_scene.CommitPendingObjectChanges();
    if (const auto* input = Engine::Core::CurrentNativeInputSnapshot())
    {
        if (input->clientWidth && input->clientHeight)
            m_scene.PrepareNativeUiLayout(static_cast<float>(input->clientWidth) /
                static_cast<float>(input->clientHeight));
        else
            m_scene.ClearPreparedNativeUiLayout();
    }
    m_scene.EnforceParticleBudget();
    if (profiling)
    {
        const auto ms = [](auto first, auto last)
        { return std::chrono::duration<double, std::milli>(last - first).count(); };
        m_lastUpdateTimings = { ms(updateBegin, afterObjects),
            ms(afterObjects, afterPose), ms(afterPose, afterPhysics),
            ms(afterPhysics, Clock::now()), ms(afterObjects,afterConstraints), ms(afterConstraints,afterPose) };
    }
    };
    auto& jobs = Engine::Threading::JobScheduler::Get();
    std::vector<Engine::Threading::JobScheduler::Handle> phases;
    phases.reserve(8);
    try
    {
        phases.push_back(jobs.ScheduleOnOwner(componentUpdates));
        phases.push_back(jobs.ScheduleOnOwner(spatialScripts, {phases.back()}));
        phases.push_back(jobs.ScheduleOnOwner(transforms, {phases.back()}));
        phases.push_back(jobs.ScheduleOnOwner(animationConstraints, {phases.back()}));
        phases.push_back(jobs.ScheduleOnOwner(physicsPreparation, {phases.back()}));
        phases.push_back(jobs.ScheduleOnOwner(physicsSimulation, {phases.back()}));
        phases.push_back(jobs.ScheduleOnOwner(postPhysics, {phases.back()}));
        phases.push_back(jobs.ScheduleOnOwner(publishRenderSnapshot, {phases.back()}));
        jobs.Wait(phases.back());
    }
    catch (...)
    {
        for (const auto& phase : phases)
            try { jobs.Wait(phase); } catch (...) {}
        m_scene.GetPhysics().AbortPreparedStep();
        throw;
    }
}
}
