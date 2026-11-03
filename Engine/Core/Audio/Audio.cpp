#include "Core/Audio/Audio.h"
#include "Core/Audio/AudioMixer.h"

#include "Core/Compoonents/Audio/AudioSource.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"


namespace Engine::Audio
{
Audio::Audio(Engine::Scene::Scene& scene) : m_scene(&scene) {}

void Audio::Update(float)
{
    if (!m_scene) return;

    Engine::Components::AudioSource* fallback = nullptr;
    Engine::Components::AudioSource* assigned = nullptr;
    // Scene's typed registry is rebuilt only when components change. GetObjects
    // already contains descendants; recursively walking it revisits sources.
    const auto& sources = m_scene->GetComponents<Engine::Components::AudioSource>();
    for (auto it = sources.rbegin(); it != sources.rend(); ++it)
    {
        auto* source = *it;
        if (!source->Owner || !source->Owner->IsEnabledInHierarchy()) continue;
        if (!fallback) fallback = source;
        if (source->listenerCameraReference.IsAssigned())
        {
            assigned = source;
            break;
        }
    }

    // miniaudio exposes one engine listener. An explicit source reference wins;
    // otherwise every source follows the same active scene camera.
    if (Engine::Components::AudioSource* source = assigned ? assigned : fallback)
        source->UpdateListener();
    AudioMixer::Get().DrainCommands();
}

void Audio::Reset()
{
    if (!m_scene) return;
    for (auto* source : m_scene->GetComponents<Engine::Components::AudioSource>())
        source->Stop();
}

void Audio::SynchronizeToTime(double sceneTime)
{
    if (!m_scene) return;
    for (auto* source : m_scene->GetComponents<Engine::Components::AudioSource>())
        source->SynchronizeToTime(sceneTime);
}
}
