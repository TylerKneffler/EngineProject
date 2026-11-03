#include "Core/Audio/AudioMixer.h"
#include "Core/Compoonents/Audio/AudioSource.h"

#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 4) return 2;
    auto& mixer = Engine::Audio::AudioMixer::Get();
    if (!mixer.ConfigureOffline(48000, 2)) return 3;

    for (int file = 1; file != argc; ++file)
    {
        Engine::Components::AudioSource source;
        source.audioPath = argv[file];
        source.spatial = false;
        if (!source.Play()) return 4;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!source.IsPlaying() && std::chrono::steady_clock::now() < deadline)
        {
            source.Update();
            mixer.DrainCommands();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!source.IsPlaying())
        {
            std::cerr << "Audio failed to preload and start: " << argv[file] << '\n';
            return 5;
        }

        std::array<float, 4096> pcm{};
        if (!mixer.ReadOfflineFrames(pcm.data(), pcm.size() / 2)) return 6;
        bool audible = false;
        for (float sample : pcm) audible |= std::abs(sample) > 0.00001f;
        if (!audible)
        {
            std::cerr << "Decoded audio produced silence: " << argv[file] << '\n';
            return 7;
        }
        source.Stop();
        mixer.DrainCommands();
    }
    return 0;
}
