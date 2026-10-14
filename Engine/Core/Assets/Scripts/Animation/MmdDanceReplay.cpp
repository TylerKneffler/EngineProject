#include "Core/Assets/Scripts/Animation/MmdDanceReplay.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Serialization/SceneSerializer.h"

namespace
{
struct Registration
{
    Registration()
    {
        Engine::Serialization::RegisterComponentType<MmdDanceReplay>("MmdDanceReplay");
    }
} registration;
}

MmdDanceReplay::MmdDanceReplay()
{
    SetTypeName(COMPONENT_TYPE_NAME(MmdDanceReplay));
    RegisterField("characterObjectName", characterObjectName, "Animation");
}

Engine::Components::AnimationManager* MmdDanceReplay::FindAnimation() const
{
    auto* character = FindObjectInSceneByName(characterObjectName);
    return character ? character->GetComponent<Engine::Components::AnimationManager>() : nullptr;
}

void MmdDanceReplay::Restart(Engine::Components::AnimationManager& animation)
{
    // Play() ignores the already selected clip. Reset its clock explicitly and
    // restart the facial layers on the same frame as the body.
    animation.time = 0.f;
    animation.playing = true;
    for (auto& layer : animation.layers) layer.time = 0.f;
    animation.Tick(0.f);
}

void MmdDanceReplay::Start()
{
    auto* animation = FindAnimation();
    if (!animation || !animation->FindClip(animation->clip)) return;
    animation->looping = false;
    for (auto& layer : animation->layers) layer.looping = false;
    Restart(*animation);
}

void MmdDanceReplay::Update()
{
    auto* animation = FindAnimation();
    const auto* clip = animation ? animation->FindClip(animation->clip) : nullptr;
    if (!clip || clip->duration <= 0.f) return;
    // Non-looping playback clamps at the last frame without clearing playing.
    if (!animation->playing || animation->time >= clip->duration)
        Restart(*animation);
}
