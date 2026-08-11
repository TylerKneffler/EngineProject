#include "Core/Audio/AudioMixer.h"

#include <miniaudio.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>

struct AudioMixer::Impl
{
    ma_engine engine{};
    bool initialized = false;
    float masterVolume = 1.f;
    std::unordered_map<std::string, float> busVolumes;
    std::unordered_map<std::string, std::unique_ptr<ma_sound_group>> buses;
    std::mutex mutex;
};

AudioMixer& AudioMixer::Get()
{
    static AudioMixer mixer;
    return mixer;
}

AudioMixer::AudioMixer() : m_impl(new Impl())
{
    ma_engine_config config = ma_engine_config_init();
    ma_result result = ma_engine_init(&config, &m_impl->engine);
    if (result != MA_SUCCESS)
    {
        // Keep decoding and component tests usable on machines without an
        // output device. A later process with a device will use normal output.
        config.noDevice = MA_TRUE;
        result = ma_engine_init(&config, &m_impl->engine);
    }
    m_impl->initialized = result == MA_SUCCESS;
}

AudioMixer::~AudioMixer()
{
    if (m_impl)
    {
        if (m_impl->initialized)
        {
            for (auto& [name, group] : m_impl->buses)
                ma_sound_group_uninit(group.get());
            m_impl->buses.clear();
            ma_engine_uninit(&m_impl->engine);
        }
        delete m_impl;
    }
}

bool AudioMixer::IsAvailable() const
{
    return m_impl && m_impl->initialized;
}

void AudioMixer::SetMasterVolume(float volume)
{
    if (!m_impl) return;
    m_impl->masterVolume = std::max(0.f, volume);
    if (m_impl->initialized)
        ma_engine_set_volume(&m_impl->engine, m_impl->masterVolume);
}

float AudioMixer::GetMasterVolume() const
{
    return m_impl ? m_impl->masterVolume : 0.f;
}

void AudioMixer::SetBusVolume(const std::string& bus, float volume)
{
    if (!m_impl) return;
    const float clamped = std::max(0.f, volume);
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->busVolumes[bus] = clamped;
    auto found = m_impl->buses.find(bus);
    if (found != m_impl->buses.end())
        ma_sound_group_set_volume(found->second.get(), clamped);
}

float AudioMixer::GetBusVolume(const std::string& bus) const
{
    if (!m_impl) return 0.f;
    const auto found = m_impl->busVolumes.find(bus);
    return found == m_impl->busVolumes.end() ? 1.f : found->second;
}

void AudioMixer::SetListener(const glm::vec3& position, const glm::vec3& forward,
    const glm::vec3& up, const glm::vec3& velocity)
{
    if (!IsAvailable()) return;
    ma_engine_listener_set_position(&m_impl->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&m_impl->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&m_impl->engine, 0, up.x, up.y, up.z);
    ma_engine_listener_set_velocity(&m_impl->engine, 0, velocity.x, velocity.y, velocity.z);
}

void* AudioMixer::GetEngineHandle()
{
    return IsAvailable() ? &m_impl->engine : nullptr;
}

void* AudioMixer::GetBusHandle(const std::string& requestedBus)
{
    if (!IsAvailable()) return nullptr;
    const std::string bus = requestedBus.empty() ? "SFX" : requestedBus;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    auto found = m_impl->buses.find(bus);
    if (found != m_impl->buses.end())
        return found->second.get();

    auto group = std::make_unique<ma_sound_group>();
    if (ma_sound_group_init(&m_impl->engine, 0, nullptr, group.get()) != MA_SUCCESS)
        return nullptr;
    const auto volume = m_impl->busVolumes.find(bus);
    ma_sound_group_set_volume(group.get(), volume == m_impl->busVolumes.end() ? 1.f : volume->second);
    ma_sound_group* result = group.get();
    m_impl->buses.emplace(bus, std::move(group));
    return result;
}
