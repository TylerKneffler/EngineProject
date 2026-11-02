#include "Core/Scene/Scene.h"
#include "Core/SceneManager.h"
#include "Core/Compoonents/Animation/TwoBoneIK.h"
#include "Core/Compoonents/Animation/ChainIK.h"
#include "Core/Compoonents/Animation/RigController.h"
#include "Core/Compoonents/Animation/RigConstraints.h"
#include "Core/Compoonents/Animation/RigMorphControls.h"
#include "Core/Compoonents/Physics/RigPhysicsBinding.h"
#include "Core/Compoonents/Materials/MaterialPropertyMorph.h"
#include "Core/Compoonents/Constraints/TransformConstraint.h"
#include "Core/Audio/Audio.h"
#include "Core/Compoonents/Physics/SpatialManipulator.h"
#include "Core/Compoonents/Animation/SoftTissueSurface.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/Cloth.h"
#include "Core/Compoonents/Animation/IKBone.h"
#include "Core/Compoonents/Particles/ParticleEmitter.h"
#include "Core/Physics/Physics.h"
#include "Core/Physics/IPhysicsAdapter.h"
#include "Core/Renderers/UIRenderer.h"
#include "Core/UI/UILayout.h"
#include "Core/Compoonents/UI/Canvas.h"
#include "Core/Input/NativeInputSnapshot.h"
#include "Core/Threading/ComponentUpdateScheduler.h"
#include "Core/Threading/JobScheduler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <unordered_set>
#include <unordered_map>

namespace Engine::Scene
{
namespace
{
void DisconnectSpatialManipulators(Engine::Core::Object* object)
{
    if (!object)
        return;
    for (Engine::Core::Component* component : object->Components)
        if (auto* manipulator = dynamic_cast<
                Engine::Components::SpatialManipulator*>(component))
            manipulator->Disconnect();
}

bool ContainsObject(const Engine::Core::Object* root,
    const Engine::Core::Object* candidate)
{
    if (!root)
        return false;
    if (root == candidate)
        return true;
    for (const Engine::Core::Object* child : root->Children)
        if (ContainsObject(child, candidate))
            return true;
    return false;
}
}

Scene::Scene()
    : Scene([](Scene& owner)
        { return Engine::Physics::CreateBulletPhysicsAdapter(owner); })
{
}

Scene::Scene(PhysicsAdapterFactory physicsAdapterFactory)
{
    m_physics = std::make_unique<Engine::Physics::Physics>(
        *this, physicsAdapterFactory ? physicsAdapterFactory(*this) : nullptr);
    m_audio = std::make_unique<Engine::Audio::Audio>(*this);
    CommitPendingObjectChanges();
}

Scene::~Scene()
{
    try { FenceBackgroundWork(); }
    catch (...) { /* Component destructors still join their own workers. */ }
    m_audio->Reset();
    m_physics->Reset();
    for (const auto& object : m_objects)
        DisconnectSpatialManipulators(object.get());
    for (const auto& object : m_objects)
        if (object)
            object->OwnerScene = nullptr;
}

void Scene::FenceBackgroundWork()
{
    AssertOwnerThread();
    if (m_isUpdating)
        throw std::logic_error("Background work cannot be fenced during Update");
    if (Engine::Core::SceneManager::GetActiveScene() == this)
        Engine::Core::SceneManager::CancelPendingSceneLoad();
    FenceSceneGeneration();
    // A worker can execute code from a reloadable script module. Join it
    // before replacing scene objects or unloading that module.
    for (auto* component : GetComponents<Engine::Core::Component>())
        component->FenceBackgroundWork();
}

void Scene::Start()
{
    AssertOwnerThread();
    FenceBackgroundWork();
    m_clock.Reset();
    m_deltaTime = 0.f;
    for (const auto& object : m_objects)
        if (object)
            object->transform.ClearEditorOverride();
    // Component Start() can remove preview objects (terrain does this when
    // switching to its runtime bake), invalidating an m_objects iterator.
    std::vector<Engine::Core::Object*> startingObjects;
    startingObjects.reserve(m_objects.size());
    for (const auto& object : m_objects)
        if (object)
            startingObjects.push_back(object.get());
    for (Engine::Core::Object* object : startingObjects)
        if (std::any_of(m_objects.begin(), m_objects.end(),
            [object](const auto& current) { return current.get() == object; }))
            object->Start();
    m_hasStarted = true;
    CommitPendingObjectChanges();
}

void Scene::RefreshUpdateRegistry()
{
    if(m_updateRegistryRevision==GetStructureRevision() && !m_updateRegistryFull) return;
    const auto classify=[](Object* object) {
        bool updates=typeid(*object)!=typeid(Object),pose=false;
        for(auto* component:object->Components) if(component)
        {
            updates|=component->RequiresUpdate();
            pose|=dynamic_cast<Engine::Components::RigController*>(component) ||
                dynamic_cast<Engine::Components::MaterialPropertyMorph*>(component) ||
                dynamic_cast<Engine::Components::RigConstraints*>(component) ||
                dynamic_cast<Engine::Components::RigMorphControls*>(component) ||
                dynamic_cast<Engine::Components::RigPhysicsBinding*>(component) ||
                dynamic_cast<Engine::Components::TwoBoneIK*>(component) ||
                dynamic_cast<Engine::Components::ChainIK*>(component) ||
                dynamic_cast<Engine::Components::TransformConstraint*>(component);
        }
        return std::pair<bool,bool>{updates,pose};
    };
    if(m_updateRegistryFull)
    {
        m_updateObjects.clear(); m_poseObjects.clear();
        for(const auto& object:m_objects)
        { const auto state=classify(object.get()); if(state.first) m_updateObjects.push_back(object.get()); if(state.second) m_poseObjects.push_back(object.get()); }
    }
    else for(auto* owner:m_updateRegistryDirty)
    {
        const auto state=m_componentRegistry.Contains(owner) ? classify(owner):std::pair<bool,bool>{false,false};
        const auto update=[&](auto& list,bool active) {
            auto found=std::find(list.begin(),list.end(),owner);
            if(active && found==list.end()) list.push_back(owner);
            else if(!active && found!=list.end()) list.erase(found);
        };
        update(m_updateObjects,state.first); update(m_poseObjects,state.second);
    }
    m_updateRegistryDirty.clear(); m_updateRegistryFull=false; m_updateRegistryRevision=GetStructureRevision();
}

void Scene::Update(float deltaTime)
{
    AssertOwnerThread();
    ResetGrassPerformance();
    using Clock = std::chrono::steady_clock;
    const auto updateBegin = m_updateProfilingEnabled ? Clock::now() : Clock::time_point{};
    m_deltaTime = m_clock.Advance(deltaTime);
    EnforceParticleBudget();
    m_componentRegistry.ReleaseSnapshots();
    m_isUpdating = true;
    struct UpdatingGuard
    {
        bool& flag;
        ~UpdatingGuard() { flag = false; }
    } updatingGuard{m_isUpdating};
    const double physicsDelta = m_clock.IsFixedStep()
        ? m_clock.GetFixedStep() : static_cast<double>(m_deltaTime);
    Clock::time_point afterObjects{}, afterConstraints{}, afterPose{}, afterPhysics{};
    const auto componentUpdates = [&]
    {
    RefreshUpdateRegistry();
    const auto updateObjects = m_updateObjects; // callbacks may change the registry
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
        std::vector<Engine::Threading::ComponentUpdateScheduler::Node> objectUpdates;
        bool serial = false;
        for (auto* component : object->Components)
        {
            if (!component || !component->RequiresUpdate()) continue;
            const auto policy = component->GetCallbackPolicy();
            if (policy.updateThread != Engine::Core::Component::UpdateThreadAffinity::Worker)
            {
                serial = true;
                break;
            }
            if (policy.updateSafety != Engine::Core::Component::UpdateParallelSafety::DeclaredAccess)
                throw std::logic_error("Worker component update requires declared parallel safety");
            auto access = component->GetUpdateAccess();
            if (!access)
                throw std::logic_error("Worker component update requires an access declaration");
            objectUpdates.push_back({component, std::move(*access)});
        }
        if (serial)
        {
            finishParallelUpdates();
            object->Update();
        }
        else
            parallelUpdates.insert(parallelUpdates.end(),
                std::make_move_iterator(objectUpdates.begin()),
                std::make_move_iterator(objectUpdates.end()));
    }
    finishParallelUpdates();
    };
    const auto spatialScripts = [&]
    {
        UpdateSpatialScripts();
        afterObjects = m_updateProfilingEnabled ? Clock::now() : Clock::time_point{};
    };
    const auto transforms = [&]
    {
        std::vector<Engine::Components::Transform*> values;
        values.reserve(m_objects.size());
        for (const auto& object : m_objects)
            values.push_back(&object->transform);
        m_transformHierarchy.Prepare(values);
    };
    const auto animationConstraints = [&]
    {
        ApplyPoseConstraints();
        afterConstraints = m_updateProfilingEnabled ? Clock::now() : Clock::time_point{};
        m_audio->Update(m_deltaTime);
        afterPose = m_updateProfilingEnabled ? Clock::now() : Clock::time_point{};
    };
    const auto physicsPreparation = [&] { m_physics->PrepareStep(physicsDelta); };
    const auto physicsSimulation = [&] { m_physics->SimulatePreparedStep(); };
    const auto postPhysics = [&]
    {
    m_physics->FinishPreparedStep();
    afterPhysics = m_updateProfilingEnabled ? Clock::now() : Clock::time_point{};
    for (auto* binding : GetComponents<Engine::Components::RigPhysicsBinding>())
        if (binding->Owner->IsEnabledInHierarchy()) binding->PostPhysicsUpdate();
    for (auto* constraints : GetComponents<Engine::Components::RigConstraints>())
        if (constraints->Owner->IsEnabledInHierarchy())
        { constraints->RefreshPoseOutputs(); constraints->PostPhysicsUpdate(); }
    for (auto* morphs : GetComponents<Engine::Components::RigMorphControls>())
        if (morphs->Owner->IsEnabledInHierarchy() && !morphs->Owner->GetComponent<Engine::Components::RigConstraints>())
            morphs->RefreshPoseOutputs();
    // Bullet has now published current-frame poses and overlap pairs. Portal
    // traversal deliberately runs here rather than in Component::Update so a
    // crossing is evaluated against the motion that just occurred.
    struct OrderedPortal
    {
        Engine::Components::SpatialManipulator* manipulator = nullptr;
        ObjectPath path;
    };
    std::vector<OrderedPortal> portals;
    for (auto* manipulator : GetComponents<Engine::Components::SpatialManipulator>())
        if (manipulator->Owner->IsEnabledInHierarchy())
        {
            ObjectPath path;
            TryGetObjectPath(manipulator->Owner, path);
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
    for (auto* tissue : GetComponents<Engine::Components::SoftTissueSurface>())
        if (tissue->Owner->IsEnabledInHierarchy())
        {
            for (const auto* body : claimedBodies)
                if (body && tissue->IsBoundUnder(body->Owner)) { tissue->Reset(); break; }
            tissue->PostPhysicsUpdate(static_cast<float>(physicsDelta));
        }
    if (const auto* input = Engine::Core::CurrentNativeInputSnapshot())
    {
        if (input->clientWidth && input->clientHeight)
            Engine::Renderers::UIRenderer::UpdateNativeInteraction(*this, *input);
    }
    };
    const auto publishRenderSnapshot = [&]
    {
    m_isUpdating = false;
    CommitPendingObjectChanges();
    if (const auto* input = Engine::Core::CurrentNativeInputSnapshot())
    {
        if (input->clientWidth && input->clientHeight)
            PrepareNativeUiLayout(static_cast<float>(input->clientWidth) /
                static_cast<float>(input->clientHeight));
        else
            m_preparedNativeUiLayout.clear();
    }
    EnforceParticleBudget();
    if (m_updateProfilingEnabled)
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
        m_physics->AbortPreparedStep();
        throw;
    }
}

void Scene::SetParticleBudget(size_t capacity)
{
    m_particleBudget = std::min(capacity, size_t{100000});
    EnforceParticleBudget();
}

size_t Scene::GetParticleLiveCount() const
{
    size_t count=0;
    for(auto* emitter:GetComponents<Engine::Components::ParticleEmitter>()) count+=emitter->GetParticles().size();
    return count;
}
size_t Scene::GetParticleCapacityFor(const Engine::Components::ParticleEmitter* target) const
{
    size_t remaining=m_particleBudget;
    for(auto* emitter:GetComponents<Engine::Components::ParticleEmitter>())
    {
        if(emitter==target) continue;
        const auto reserved=std::max(emitter->GetParticles().size(),size_t(emitter->GetGpuReservedCapacity()));
        remaining=reserved>=remaining ? 0:remaining-reserved;
    }
    return remaining;
}
void Scene::EnforceParticleBudget()
{
    size_t remaining=m_particleBudget;
    for(auto* emitter:GetComponents<Engine::Components::ParticleEmitter>())
    {
        emitter->TrimToCapacity(remaining);
        const auto reserved=std::max(emitter->GetParticles().size(),size_t(emitter->GetGpuReservedCapacity()));
        remaining=reserved>=remaining ? 0:remaining-reserved;
    }
}

void Scene::ApplyPoseConstraints()
{
    RefreshUpdateRegistry();
    const auto poseObjects = m_poseObjects;
    for (auto* object : poseObjects)
        if (object && object->IsEnabledInHierarchy())
            if (auto* legacy = object->GetComponent<Engine::Components::RigController>())
                legacy->EnsureComponents();
    for (auto* object : poseObjects)
        if (object && object->IsEnabledInHierarchy())
            for (Engine::Core::Component* component : object->Components)
                if (auto* morph = dynamic_cast<Engine::Components::MaterialPropertyMorph*>(component))
                    morph->Apply();
    const auto applyRig = [](Engine::Core::Object* object)
    {
        auto* constraints = object->GetComponent<Engine::Components::RigConstraints>();
        auto* morphs = object->GetComponent<Engine::Components::RigMorphControls>();
        if (constraints) constraints->RestorePose();
        if (morphs) morphs->ApplyBoneMorphs();
        if (constraints) constraints->Apply();
        if (auto* binding = object->GetComponent<Engine::Components::RigPhysicsBinding>())
            binding->Apply();
        if (morphs) morphs->ApplyImpulseMorphs();
    };
    auto& jobs = Engine::Threading::JobScheduler::Get();
    std::vector<Engine::Core::Object*> independentRigs;
    const auto finishIndependentRigs = [&]
    {
        jobs.For(independentRigs.size(), 1, [&](size_t index)
        { applyRig(independentRigs[index]); });
        independentRigs.clear();
    };
    for (auto* object : poseObjects)
    {
        if (!object || !object->IsEnabledInHierarchy()) continue;
        const auto* constraints = object->GetComponent<Engine::Components::RigConstraints>();
        const auto* morphs = object->GetComponent<Engine::Components::RigMorphControls>();
        const auto* binding = object->GetComponent<Engine::Components::RigPhysicsBinding>();
        if (!constraints && !morphs && !binding) continue;
        // Top-level rigs own disjoint transform trees. External parents and
        // impulse/physics bindings can read or change another tree or Bullet,
        // so they retain their original owner-thread position in the order.
        const bool independent = !object->Parent &&
            !binding &&
            (!constraints || constraints->externalParents.empty()) &&
            (!morphs || morphs->impulseMorphs.empty());
        if (independent)
            independentRigs.push_back(object);
        else
        {
            finishIndependentRigs();
            applyRig(object);
        }
    }
    finishIndependentRigs();
    for (auto* object : poseObjects)
        if (object && object->IsEnabledInHierarchy())
            for (Engine::Core::Component* component : object->Components)
                if (auto* ik = dynamic_cast<Engine::Components::TwoBoneIK*>(component))
                    ik->Apply();
    std::unordered_map<Engine::Core::Object*,
        std::vector<Engine::Components::ChainIK*>> chainGroups;
    std::vector<Engine::Core::Object*> chainRoots;
    for (auto* object : poseObjects)
        if (object && object->IsEnabledInHierarchy())
            for (auto* component : object->Components)
                if (auto* ik = dynamic_cast<Engine::Components::ChainIK*>(component))
                {
                    if (chainGroups.find(object) == chainGroups.end())
                        chainRoots.push_back(object);
                    chainGroups[object].push_back(ik);
                }
    for (auto* root : chainRoots)
        Engine::Components::ChainIK::ApplyGroup(chainGroups[root]);
    // Animation and IK establish the input pose. Constraint targets and
    // constrained parents must be evaluated before their dependents.
    std::vector<Engine::Components::TransformConstraint*> constraints;
    std::unordered_map<Engine::Core::Object*,
        std::vector<Engine::Components::TransformConstraint*>> byOwner;
    for (auto* object : poseObjects)
        if (object && object->IsEnabledInHierarchy())
            for (auto* component : object->Components)
                if (auto* constraint = dynamic_cast<
                        Engine::Components::TransformConstraint*>(component))
                {
                    constraints.push_back(constraint);
                    byOwner[object].push_back(constraint);
                }
    std::stable_sort(constraints.begin(), constraints.end(),
        [](const auto* left, const auto* right)
        { return left->evaluationPriority < right->evaluationPriority; });
    for (auto& [owner, ownerConstraints] : byOwner)
        std::stable_sort(ownerConstraints.begin(), ownerConstraints.end(),
            [](const auto* left, const auto* right)
            { return left->evaluationPriority < right->evaluationPriority; });
    std::unordered_map<Engine::Components::TransformConstraint*, int> state;
    std::unordered_map<Engine::Components::TransformConstraint*, bool> valid;
    const auto evaluate = [&](auto&& self,
        Engine::Components::TransformConstraint* constraint) -> bool
    {
        const int current = state[constraint];
        if (current == 2) return valid[constraint];
        if (current == 1) return false;
        state[constraint] = 1;
        bool okay = constraint->ValidationError().empty();
        const auto dependency = [&](Engine::Core::Object* object)
        {
            if (!okay) return;
            if (auto found = byOwner.find(object); found != byOwner.end())
                for (auto* candidate : found->second)
                    if (candidate != constraint && !self(self, candidate))
                    { okay = false; break; }
        };
        if (auto same = byOwner.find(constraint->Owner); same != byOwner.end())
            for (auto* earlier : same->second)
            {
                if (earlier == constraint) break;
                if (!self(self, earlier)) { okay = false; break; }
            }
        for (auto* parent = constraint->Owner->Parent; parent;
            parent = parent->Parent) dependency(parent);
        if (okay)
            for (auto* target = constraint->Target(); target;
                target = target->Parent) dependency(target);
        if (okay) constraint->Apply();
        state[constraint] = 2;
        valid[constraint] = okay;
        return okay;
    };
    for (auto* constraint : constraints) evaluate(evaluate, constraint);
}

void Scene::UpdateFixedFrame()
{
    AssertOwnerThread();
    if (!m_clock.IsFixedStep())
        throw std::logic_error("UpdateFixedFrame requires a configured fixed time step");
    Update(0.f);
}

Engine::Core::Object* Scene::AddObject()
{
    return AddObjectWithId(m_nextObjectId.fetch_add(1, std::memory_order_relaxed));
}

Engine::Core::Object* Scene::AddObjectWithId(ObjectId id)
{
    AssertOwnerThread();
    auto obj = std::make_unique<Engine::Core::Object>();
    Engine::Core::Object* raw = obj.get();
    raw->OwnerScene = this;
    m_objectIds.emplace(raw, id);
    m_objectsById.emplace(id, raw);
    if (m_isUpdating || m_isCommitting)
        m_pendingObjectAdditions.push_back(std::move(obj));
    else
    {
        m_objects.push_back(std::move(obj));
        m_componentRegistry.Adopt(raw);
    }
    NotifyStructureChanged(raw);
    return raw;
}

Engine::Core::Object* Scene::AddObject(const std::string& name)
{
    Engine::Core::Object* obj = AddObject();
    obj->name = name;
    obj->OwnerScene = this;
    return obj;
}

Engine::Core::Object* Scene::FindObjectByName(const std::string& name)
{
    const Scene* constScene = this;
    return const_cast<Engine::Core::Object*>(constScene->FindObjectByName(name));
}

const Engine::Core::Object* Scene::FindObjectByName(const std::string& name) const
{
    const auto findNamed = [&name](const auto& objects)
        -> const Engine::Core::Object*
    {
        for (const auto& object : objects)
        {
            if (object && object->name == name)
                return object.get();
        }
        return nullptr;
    };
    if (const Engine::Core::Object* object = findNamed(m_objects))
        return object;
    return findNamed(m_pendingObjectAdditions);
}

bool Scene::TryGetObjectPath(const Engine::Core::Object* object, ObjectPath& path) const
{
    path.clear();
    if (!object)
        return false;

    for (const Engine::Core::Object* current = object; current; current = current->Parent)
    {
        std::size_t index = 0;
        bool found = false;
        if (current->Parent)
        {
            const auto& siblings = current->Parent->Children;
            const auto position =
                std::find(siblings.begin(), siblings.end(), current);
            if (position != siblings.end())
            {
                index = static_cast<std::size_t>(
                    position - siblings.begin());
                found = true;
            }
        }
        else
        {
            for (const auto& candidate : m_objects)
            {
                if (candidate->Parent)
                    continue;
                if (candidate.get() == current)
                {
                    found = true;
                    break;
                }
                ++index;
            }
        }

        if (!found)
        {
            path.clear();
            return false;
        }
        path.push_back(index);
    }

    std::reverse(path.begin(), path.end());
    return true;
}

Engine::Core::Object* Scene::FindObjectByPath(const ObjectPath& path) const
{
    if (path.empty())
        return nullptr;

    Engine::Core::Object* current = nullptr;
    std::size_t rootIndex = 0;
    for (const auto& candidate : m_objects)
    {
        if (candidate->Parent)
            continue;
        if (rootIndex++ == path.front())
        {
            current = candidate.get();
            break;
        }
    }
    if (!current)
        return nullptr;

    for (std::size_t depth = 1; depth < path.size(); ++depth)
    {
        if (path[depth] >= current->Children.size())
            return nullptr;
        current = current->Children[path[depth]];
    }
    return current;
}

bool Scene::MoveObject(
    Engine::Core::Object* object,
    Engine::Core::Object* target,
    ObjectPlacement placement)
{
    AssertOwnerThread();
    if (!object || object == target)
        return false;

    Engine::Core::Object* newParent = placement == ObjectPlacement::AsChild
        ? target
        : (target ? target->Parent : nullptr);
    for (Engine::Core::Object* ancestor = newParent; ancestor; ancestor = ancestor->Parent)
        if (ancestor == object)
            return false;

    const glm::mat4 oldWorld = object->transform.GetWorldMatrix();
    if (object->Parent)
    {
        auto& oldSiblings = object->Parent->Children;
        oldSiblings.erase(
            std::remove(oldSiblings.begin(), oldSiblings.end(), object),
            oldSiblings.end());
    }
    object->Parent = newParent;

    if (newParent)
    {
        auto& siblings = newParent->Children;
        if (placement == ObjectPlacement::AsChild || !target)
            siblings.push_back(object);
        else
        {
            auto position = std::find(siblings.begin(), siblings.end(), target);
            if (position == siblings.end())
                siblings.push_back(object);
            else
                siblings.insert(
                    placement == ObjectPlacement::After
                        ? position + 1
                        : position,
                    object);
        }
    }
    else
    {
        auto moving = std::find_if(
            m_objects.begin(),
            m_objects.end(),
            [object](const auto& value) { return value.get() == object; });
        if (moving == m_objects.end())
            return false;

        std::unique_ptr<Engine::Core::Object> owned = std::move(*moving);
        m_objects.erase(moving);
        auto position = target
            ? std::find_if(
                m_objects.begin(),
                m_objects.end(),
                [target](const auto& value)
                {
                    return value.get() == target;
                })
            : m_objects.end();
        if (position == m_objects.end())
            m_objects.push_back(std::move(owned));
        else
            m_objects.insert(
                placement == ObjectPlacement::After
                    ? position + 1
                    : position,
                std::move(owned));
    }

    const glm::mat4 parentWorld =
        newParent ? newParent->transform.GetWorldMatrix() : glm::mat4(1.f);
    const glm::mat4 local = glm::inverse(parentWorld) * oldWorld;
    object->transform.position = glm::vec3(local[3]);
    object->transform.scale = {
        glm::length(glm::vec3(local[0])),
        glm::length(glm::vec3(local[1])),
        glm::length(glm::vec3(local[2]))
    };

    glm::mat3 rotation(1.f);
    for (int column = 0; column < 3; ++column)
    {
        const float scale = object->transform.scale[column];
        if (scale > 0.000001f)
            rotation[column] = glm::vec3(local[column]) / scale;
    }

    const float y = std::asin(std::clamp(-rotation[0][2], -1.f, 1.f));
    const float cosineY = std::cos(y);
    const float x = std::abs(cosineY) > 0.00001f
        ? std::atan2(rotation[1][2], rotation[2][2])
        : std::atan2(-rotation[2][1], rotation[1][1]);
    const float z = std::abs(cosineY) > 0.00001f
        ? std::atan2(rotation[0][1], rotation[0][0])
        : 0.f;
    object->transform.rotation = { x, y, z };
    NotifyStructureChanged();
    return true;
}

bool Scene::SetParentLocal(Object* object, Object* parent)
{
    AssertOwnerThread();
    if (!object || object->OwnerScene != this ||
        (parent && parent->OwnerScene != this)) return false;
    for (Object* ancestor = parent; ancestor; ancestor = ancestor->Parent)
        if (ancestor == object) return false;
    if (object->Parent == parent) return true;
    if (object->Parent)
    {
        auto& siblings = object->Parent->Children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), object),
            siblings.end());
    }
    object->Parent = parent;
    if (parent) parent->Children.push_back(object);
    NotifyStructureChanged(object);
    return true;
}

void Scene::RemoveObject(Engine::Core::Object* obj)
{
    AssertOwnerThread();
    if (!obj)
        return;

    if (m_isUpdating || (m_isCommitting && !m_isFlushingRemovals))
    {
        RequestRemoveObject(obj);
        return;
    }

    // Scene jobs can retain references to any object. Finish them before
    // callbacks or destructors invalidate those references.
    WaitForSceneJobs();

    if (m_beforeGpuResourceDestruction)
        m_beforeGpuResourceDestruction();

    std::vector<Engine::Core::Object*> objectsToRemove;
    std::function<void(Engine::Core::Object*)> collect = [&](Engine::Core::Object* current)
    {
        if (!current)
            return;
        objectsToRemove.push_back(current);
        for (Engine::Core::Object* child : current->Children)
            collect(child);
    };
    collect(obj);

    for (Engine::Core::Object* object : objectsToRemove)
        for (Engine::Core::Component* component : object->Components)
            if (component) component->FenceBackgroundWork();

    if (std::find(objectsToRemove.begin(), objectsToRemove.end(),
            m_editorCameraFollowTarget) != objectsToRemove.end())
        StopEditorCameraFollow();

    // Sever external links while every endpoint and hierarchy path is still
    // valid. Component destructors then only need to clear local state.
    for (Engine::Core::Object* object : objectsToRemove)
        DisconnectSpatialManipulators(object);
    // Physics component state is keyed by component address. Release it before
    // detaching the object from the scene, or a later streaming cell can reuse
    // the address and inherit a stale state with no bound body.
    for (Engine::Core::Object* object : objectsToRemove)
        for (Engine::Core::Component* component : object->Components)
        {
            m_componentSnapshotCache.erase(component);
            if (auto* body = dynamic_cast<Engine::Components::RigidBody*>(component))
                m_physics->DestroyRigidBody(*body);
            else if (auto* cloth = dynamic_cast<Engine::Components::Cloth*>(component))
                m_physics->DestroyCloth(*cloth);
            else if (auto* bone = dynamic_cast<Engine::Components::IKBone*>(component))
                m_physics->DestroyIKBone(*bone);
        }
    for (Engine::Core::Object* object : objectsToRemove)
        if (object)
        {
            NotifyStructureChanged(object,true);
            m_objectsById.erase(GetObjectId(object));
            m_objectIds.erase(object);
            object->OwnerScene = nullptr;
        }

    if (obj->Parent)
    {
        auto& siblings = obj->Parent->Children;
        siblings.erase(
            std::remove(siblings.begin(), siblings.end(), obj),
            siblings.end());
        obj->Parent = nullptr;
    }
    if (m_selectedObject &&
        std::find(
            objectsToRemove.begin(),
            objectsToRemove.end(),
            m_selectedObject) != objectsToRemove.end())
    {
        m_selectedObject = nullptr;
    }
    if (m_previewObject &&
        std::find(objectsToRemove.begin(), objectsToRemove.end(),
            m_previewObject) != objectsToRemove.end())
    {
        m_previewObject = nullptr;
    }

    m_editorSelectedMesh = nullptr;
    m_objects.erase(
        std::remove_if(
            m_objects.begin(),
            m_objects.end(),
            [&](const std::unique_ptr<Engine::Core::Object>& candidate)
            {
                return std::find(
                    objectsToRemove.begin(),
                    objectsToRemove.end(),
                    candidate.get()) != objectsToRemove.end();
            }),
        m_objects.end());
}

void Scene::RequestRemoveObject(Engine::Core::Object* obj)
{
    AssertOwnerThread();
    if (!obj || std::find(m_pendingObjectRemovals.begin(),
            m_pendingObjectRemovals.end(), obj) != m_pendingObjectRemovals.end())
    {
        return;
    }
    m_pendingObjectRemovals.push_back(obj);
}

void Scene::FlushPendingObjectAdditions()
{
    std::vector<std::unique_ptr<Object>> pending;
    pending.swap(m_pendingObjectAdditions);
    for (auto& object : pending)
    {
        if (!object)
            continue;
        // A runtime-spawned object cannot receive Start() until all of its
        // components have been attached by its creator. That is now true at
        // this update boundary.
        if (m_hasStarted)
            object->Start();
        auto* added=object.get();
        m_objects.push_back(std::move(object));
        m_componentRegistry.Adopt(added); NotifyStructureChanged(added);
    }
}

void Scene::FlushPendingObjectRemovals()
{
    std::vector<Engine::Core::Object*> pending;
    pending.swap(m_pendingObjectRemovals);
    m_isFlushingRemovals = true;
    struct RemovalGuard { bool& flag; ~RemovalGuard() { flag = false; } }
        guard{m_isFlushingRemovals};
    for (Engine::Core::Object* object : pending)
    {
        // An earlier request can remove a parent. Do not dereference the
        // stale child pointer; compare it only against live hierarchy nodes.
        const bool isLive = std::any_of(m_objects.begin(), m_objects.end(),
            [object](const std::unique_ptr<Engine::Core::Object>& root)
            {
                return ContainsObject(root.get(), object);
            });
        if (isLive)
            RemoveObject(object);
    }
}

void Scene::ClearObjects()
{
    AssertOwnerThread();
    if (m_isUpdating)
        throw std::logic_error("Clearing a scene during Update requires a commit boundary");
    FenceBackgroundWork();
    if (m_beforeGpuResourceDestruction && !m_objects.empty())
        m_beforeGpuResourceDestruction();
    m_hasStarted = false;
    m_graphicsPreparationPending = false;
    {
        std::lock_guard<std::mutex> lock(m_queuedSceneMutationsMutex);
        m_queuedSceneMutations.clear();
    }
    m_pendingObjectAdditions.clear();
    m_pendingObjectRemovals.clear();
    m_preparedNativeUiLayout.clear();
    m_audio->Reset();
    m_physics->Reset();
    for (const auto& object : m_objects)
        DisconnectSpatialManipulators(object.get());
    for (const auto& object : m_objects)
        if (object)
            object->OwnerScene = nullptr;
    m_objects.clear();
    m_objectIds.clear();
    m_objectsById.clear();
    m_componentSnapshotCache.clear();
    m_selectedObject = nullptr;
    m_editorSelectedMesh = nullptr;
    m_previewObject = nullptr;
    NotifyStructureChanged();
    CommitPendingObjectChanges();
}

void Scene::PrepareNativeUiLayout(float viewportAspect)
{
    AssertOwnerThread();
    if (GetComponents<Engine::Components::Canvas>().empty())
    {
        m_preparedNativeUiLayout.clear();
        return;
    }
    m_preparedNativeUiLayout = Engine::UI::UILayout::Resolve(*this, viewportAspect);
}

Engine::Threading::JobScheduler::Handle Scene::ScheduleSceneJob(
    std::function<void(SceneGeneration)> work)
{
    AssertOwnerThread();
    auto& scheduler = Engine::Threading::JobScheduler::Get();
    m_sceneJobs.erase(std::remove_if(m_sceneJobs.begin(), m_sceneJobs.end(),
        [&scheduler](const auto& handle) { return scheduler.IsComplete(handle); }),
        m_sceneJobs.end());
    const SceneGeneration generation = CaptureSceneGeneration();
    auto handle = scheduler.Schedule([this, generation, work = std::move(work)]
    {
        if (CaptureSceneGeneration() == generation)
            work(generation);
    }, {}, false);
    m_sceneJobs.push_back(handle);
    return handle;
}

void Scene::FenceSceneGeneration()
{
    AssertOwnerThread();
    {
        std::lock_guard lock(m_queuedSceneMutationsMutex);
        SceneGeneration next = m_sceneGeneration.load(std::memory_order_relaxed) + 1;
        if (!next) ++next;
        m_sceneGeneration.store(next, std::memory_order_release);
        m_queuedSceneMutations.clear();
    }
    WaitForSceneJobs();
}

void Scene::WaitForSceneJobs()
{
    AssertOwnerThread();
    auto jobs = std::move(m_sceneJobs);
    m_sceneJobs.clear();
    auto& scheduler = Engine::Threading::JobScheduler::Get();
    // Drain every callback even when one job failed. Callers receive job
    // errors through their handles; lifecycle cleanup must still finish.
    for (const auto& job : jobs)
        try { scheduler.Wait(job); } catch (...) {}
}

bool Scene::QueueSceneMutation(SceneGeneration generation,
    std::function<void(Scene&)> mutation)
{
    std::lock_guard<std::mutex> lock(m_queuedSceneMutationsMutex);
    if (generation != m_sceneGeneration.load(std::memory_order_acquire))
        return false;
    m_queuedSceneMutations.push_back({generation, std::move(mutation)});
    return true;
}

Scene::ObjectId Scene::GetObjectId(const Object* object) const
{
    AssertOwnerThread();
    const auto found = m_objectIds.find(object);
    return found == m_objectIds.end() ? 0 : found->second;
}

Scene::Object* Scene::FindObjectById(ObjectId id) const
{
    AssertOwnerThread();
    const auto found = m_objectsById.find(id);
    return found == m_objectsById.end() ? nullptr : found->second;
}

Scene::ObjectId Scene::QueueAddObject(SceneGeneration generation, std::string name)
{
    const ObjectId id = m_nextObjectId.fetch_add(1, std::memory_order_relaxed);
    return QueueSceneMutation(generation, [id, name = std::move(name)](Scene& scene)
    {
        scene.AddObjectWithId(id)->name = name;
    }) ? id : 0;
}

void Scene::QueueRemoveObject(SceneGeneration generation, ObjectId id)
{
    QueueSceneMutation(generation, [id](Scene& scene)
    {
        if (Object* object = scene.FindObjectById(id))
            scene.RemoveObject(object);
    });
}

void Scene::QueueMoveObject(SceneGeneration generation, ObjectId id, ObjectId target,
    ObjectPlacement placement)
{
    QueueSceneMutation(generation, [id, target, placement](Scene& scene)
    {
        Object* object = scene.FindObjectById(id);
        Object* destination = target ? scene.FindObjectById(target) : nullptr;
        if (object && (!target || destination))
            scene.MoveObject(object, destination, placement);
    });
}

void Scene::QueueObjectFieldWrite(SceneGeneration generation, ObjectId id,
    std::function<void(Object&)> write)
{
    QueueSceneMutation(generation, [id, write = std::move(write)](Scene& scene)
    {
        if (Object* object = scene.FindObjectById(id))
            write(*object);
    });
}

void Scene::CommitPendingObjectChanges()
{
    AssertOwnerThread();
    if (m_isUpdating || m_isCommitting) return;
    m_isCommitting = true;
    struct CommitGuard { bool& flag; ~CommitGuard() { flag = false; } } guard{m_isCommitting};
    std::vector<QueuedSceneMutation> commands;
    {
        std::lock_guard<std::mutex> lock(m_queuedSceneMutationsMutex);
        commands.swap(m_queuedSceneMutations);
    }
    const SceneGeneration generation = CaptureSceneGeneration();
    for (auto& command : commands)
        if (command.generation == generation) command.apply(*this);
    FlushPendingObjectAdditions();
    if (m_hasStarted && m_lifecycleRevision != m_structureRevision)
    {
        m_lifecycleRevision = m_structureRevision;
        // Newly attached components begin at the next owner commit. The
        // per-component state makes this stable across repeated commits.
        const auto objects = [&]
        {
            std::vector<Object*> result;
            result.reserve(m_objects.size());
            for (const auto& object : m_objects) result.push_back(object.get());
            return result;
        }();
        for (Object* object : objects)
            if (m_componentRegistry.Contains(object)) object->Start();
    }
    FlushPendingObjectRemovals();
    m_componentRegistry.ReleaseSnapshots();
    if (!m_suppressSceneSnapshotPublication)
        PublishSceneSnapshot();
}

void Scene::PublishSceneSnapshot()
{
    AssertOwnerThread();
    if (!m_isCommitting)
        throw std::logic_error("Scene snapshots may only publish at a commit boundary");
    const auto publishBegin = std::chrono::steady_clock::now();
    const auto current = GetPublishedSceneSnapshot();
    bool unchanged = current && current->structureRevision == m_structureRevision &&
        current->objects.size() == m_objects.size();
    if (unchanged)
        for (size_t index = 0; index < m_objects.size(); ++index)
        {
            const Object* object = m_objects[index].get();
            const auto& prior = current->objects[index];
            if (prior.id != GetObjectId(object) ||
                prior.parentId != GetObjectId(object->Parent) ||
                prior.name != object->name || prior.enabled != object->enabled ||
                glm::any(glm::notEqual(prior.position, object->transform.position)) ||
                glm::any(glm::notEqual(prior.rotation, object->transform.rotation)) ||
                glm::any(glm::notEqual(prior.scale, object->transform.scale)) ||
                prior.components.size() != object->Components.size())
            { unchanged = false; break; }
            size_t componentIndex = 0;
            for (const auto* component : object->Components)
                if (!component ||
                    prior.components[componentIndex++].configurationRevision !=
                        component->GetConfigurationRevision())
                { unchanged = false; break; }
            if (!unchanged) break;
        }
    if (unchanged)
    {
        m_lastSnapshotPublishMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - publishBegin).count();
        return;
    }
    auto snapshot = std::make_shared<SceneSnapshot>();
    snapshot->generation = ++m_snapshotGeneration;
    snapshot->structureRevision = m_structureRevision;
    snapshot->objects.reserve(m_objects.size());
    for (const auto& owned : m_objects)
    {
        const Object* object = owned.get();
        SceneSnapshot::ObjectInfo state;
        state.id = GetObjectId(object);
        state.parentId = GetObjectId(object->Parent);
        state.name = object->name;
        state.enabled = object->enabled;
        state.position = object->transform.position;
        state.rotation = object->transform.rotation;
        state.scale = object->transform.scale;
        state.components.reserve(object->Components.size());
        for (const auto* component : object->Components)
            if (component)
            {
                const auto revision = component->GetConfigurationRevision();
                auto& cached = m_componentSnapshotCache[component];
                if (!cached.state || cached.revision != revision)
                {
                    cached.state = std::make_shared<const
                        Engine::Serialization::JsonValue>(component->Serialize());
                    cached.revision = revision;
                }
                state.components.push_back({component->GetTypeName(), revision,
                    cached.state});
            }
        snapshot->objects.push_back(std::move(state));
    }
    std::shared_ptr<const SceneSnapshot> immutable = std::move(snapshot);
    std::atomic_store_explicit(&m_publishedSceneSnapshot,
        std::move(immutable), std::memory_order_release);
    m_lastSnapshotPublishMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - publishBegin).count();
}

}
