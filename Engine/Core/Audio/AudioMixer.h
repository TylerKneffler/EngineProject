#pragma once

#include <glm/glm.hpp>
#include <string>

namespace Engine::Audio
{
// One process-wide output device and node graph. AudioSource components feed
// named buses into this mixer; buses are created lazily and default to unity.
class AudioMixer
{
public:
    static AudioMixer& Get();

    bool IsAvailable() const;
    void SetMasterVolume(float volume);
    float GetMasterVolume() const;
    void SetBusVolume(const std::string& bus, float volume);
    float GetBusVolume(const std::string& bus) const;

    void SetListener(const glm::vec3& position, const glm::vec3& forward,
        const glm::vec3& up, const glm::vec3& velocity = glm::vec3(0.f));

    // Internal bridge used by AudioSource without leaking miniaudio types into
    // public component headers.
    void* GetEngineHandle();
    void* GetBusHandle(const std::string& bus);

private:
    AudioMixer();
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    struct Impl;
    Impl* m_impl = nullptr;
};
}
