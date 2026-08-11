#include "Core/Audio/Audio.h"

#include "Core/Compoonents/AudioSource.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"

Audio::Audio(Scene& scene) : m_scene(&scene) {}

void Audio::Update(float)
{
    if (!m_scene) return;

    AudioSource* fallback = nullptr;
    AudioSource* assigned = nullptr;
    for (const auto& object : m_scene->GetObjects())
    {
        if (!object->IsEnabledInHierarchy()) continue;
        for (Component* component : object->Components)
        {
            auto* source = dynamic_cast<AudioSource*>(component);
            if (!source) continue;
            if (!fallback) fallback = source;
            if (source->listenerCameraReference.IsAssigned())
            {
                assigned = source;
                break;
            }
        }
        if (assigned) break;
    }

    // miniaudio exposes one engine listener. An explicit source reference wins;
    // otherwise every source follows the same active scene camera.
    if (AudioSource* source = assigned ? assigned : fallback)
        source->UpdateListener();
}

void Audio::Reset()
{
    if (!m_scene) return;
    for (const auto& object : m_scene->GetObjects())
        for (Component* component : object->Components)
            if (auto* source = dynamic_cast<AudioSource*>(component))
                source->Stop();
}
