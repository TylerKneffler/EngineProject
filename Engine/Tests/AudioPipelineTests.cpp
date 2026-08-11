#include "Core/Audio/AudioMixer.h"
#include "Core/Compoonents/AudioSource.h"
#include "Core/Serialization/SceneSerializer.h"

#include <cmath>
#include <iostream>
#include <memory>

namespace
{
bool CheckCodec(AudioSource& source, const char* label, const char* path)
{
    source.Stop();
    source.audioPath = path;
    if (!source.Play())
    {
        std::cerr << label << " failed to decode or start: " << path << '\n';
        return false;
    }
    source.Pause();
    source.Play(); // Pause preserves the cursor and Play resumes it.
    source.Stop();
    std::cout << label << " decoded and created a mixer voice\n";
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "Expected WAV, OGG, and MP3 fixture paths\n";
        return 2;
    }

    AudioMixer& mixer = AudioMixer::Get();
    if (!mixer.IsAvailable())
    {
        std::cerr << "Audio mixer failed to initialize, including its no-device fallback\n";
        return 1;
    }
    mixer.SetMasterVolume(0.75f);
    mixer.SetBusVolume("SFX", 0.5f);
    if (std::abs(mixer.GetMasterVolume() - 0.75f) > 0.001f ||
        std::abs(mixer.GetBusVolume("SFX") - 0.5f) > 0.001f)
    {
        std::cerr << "Mixer volume controls did not round-trip\n";
        return 1;
    }

    AudioSource source;
    source.bus = "SFX";
    source.loop = true;
    source.spatial = true;
    source.directional = true;
    source.minDistance = 2.f;
    source.maxDistance = 30.f;
    if (!CheckCodec(source, "WAV", argv[1]) ||
        !CheckCodec(source, "OGG", argv[2]) ||
        !CheckCodec(source, "MP3", argv[3]))
        return 1;

    const JsonValue serialized = source.Serialize();
    AudioSource restored;
    restored.Deserialize(serialized);
    if (!restored.loop || !restored.spatial || !restored.directional ||
        restored.audioPath != argv[3] || restored.minDistance != 2.f ||
        restored.maxDistance != 30.f)
    {
        std::cerr << "AudioSource settings failed to serialize\n";
        return 1;
    }

    std::unique_ptr<Component> registered(
        SceneSerializer::CreateRegisteredComponent("AudioSource"));
    if (!dynamic_cast<AudioSource*>(registered.get()))
    {
        std::cerr << "AudioSource is absent from the component registry\n";
        return 1;
    }
    return 0;
}
