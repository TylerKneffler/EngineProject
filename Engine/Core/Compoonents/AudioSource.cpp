#include "Core/Compoonents/AudioSource.h"

#include "Core/Audio/AudioMixer.h"
#include "Core/Compoonents/Camera.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Editor/UI/IEditorUi.h"
#include <miniaudio.h>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
}

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

struct AudioSource::Impl
{
    ma_sound sound{};
    ma_audio_buffer oggBuffer{};
    short* oggSamples = nullptr;
    bool soundInitialized = false;
    bool bufferInitialized = false;
    std::string loadedPath;
    std::string loadedBus;
    int oggSampleRate = 0;
};

namespace
{
std::filesystem::path ResolveAudioPath(const std::string& path)
{
    const std::filesystem::path requested(path);
    if (std::filesystem::exists(requested)) return requested;
    const std::filesystem::path relative = requested.lexically_relative("Assets");
    if (!relative.empty() && *relative.begin() != "..")
    {
        const std::filesystem::path bundled = std::filesystem::path(ENGINE_ASSETS_PATH) / relative;
        if (std::filesystem::exists(bundled)) return bundled;
    }
    return requested;
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

glm::vec3 SafeDirection(const glm::vec3& value, const glm::vec3& fallback)
{
    const float length = glm::length(value);
    return length > 0.0001f ? value / length : fallback;
}
}

AudioSource::AudioSource() : m_impl(new Impl())
{
    SetTypeName(COMPONENT_TYPE_NAME(AudioSource));
    singlecomponent = true;
    RegisterField("audioPath", audioPath);
    RegisterField("bus", bus);
    RegisterField("playOnStart", playOnStart);
    RegisterField("loop", loop);
    RegisterField("volume", volume);
    RegisterField("pitch", pitch);
    RegisterField("spatial", spatial);
    RegisterField("listenerCameraReference", listenerCameraReference);
    RegisterField("attenuation", attenuation);
    RegisterField("minDistance", minDistance);
    RegisterField("maxDistance", maxDistance);
    RegisterField("rolloff", rolloff);
    RegisterField("dopplerFactor", dopplerFactor);
    RegisterField("directional", directional);
    RegisterField("coneInnerAngle", coneInnerAngle);
    RegisterField("coneOuterAngle", coneOuterAngle);
    RegisterField("coneOuterGain", coneOuterGain);
}

AudioSource::~AudioSource()
{
    Unload();
    delete m_impl;
}

bool AudioSource::EnsureLoaded()
{
    if (!m_impl || audioPath.empty()) return false;
    if (m_impl->soundInitialized && m_impl->loadedPath == audioPath && m_impl->loadedBus == bus)
        return true;
    Unload();

    auto* engine = static_cast<ma_engine*>(AudioMixer::Get().GetEngineHandle());
    auto* group = static_cast<ma_sound_group*>(AudioMixer::Get().GetBusHandle(bus));
    if (!engine || !group) return false;

    const std::filesystem::path resolved = ResolveAudioPath(audioPath);
    ma_result result = MA_ERROR;
    if (Lower(resolved.extension().string()) == ".ogg")
    {
        int channels = 0;
        int sampleRate = 0;
        const int frames = stb_vorbis_decode_filename(resolved.string().c_str(), &channels,
            &sampleRate, &m_impl->oggSamples);
        if (frames <= 0 || channels <= 0 || sampleRate <= 0) return false;
        m_impl->oggSampleRate = sampleRate;
        const ma_audio_buffer_config config = ma_audio_buffer_config_init(ma_format_s16,
            static_cast<ma_uint32>(channels), static_cast<ma_uint64>(frames),
            m_impl->oggSamples, nullptr);
        result = ma_audio_buffer_init(&config, &m_impl->oggBuffer);
        m_impl->bufferInitialized = result == MA_SUCCESS;
        if (result == MA_SUCCESS)
            result = ma_sound_init_from_data_source(engine, &m_impl->oggBuffer, 0, group, &m_impl->sound);
    }
    else
    {
        result = ma_sound_init_from_file(engine, resolved.string().c_str(), 0, group, nullptr,
            &m_impl->sound);
    }

    m_impl->soundInitialized = result == MA_SUCCESS;
    if (!m_impl->soundInitialized)
    {
        Unload();
        return false;
    }
    m_impl->loadedPath = audioPath;
    m_impl->loadedBus = bus;
    ApplySettings();
    return true;
}

void AudioSource::Unload()
{
    if (!m_impl) return;
    if (m_impl->soundInitialized)
        ma_sound_uninit(&m_impl->sound);
    if (m_impl->bufferInitialized)
        ma_audio_buffer_uninit(&m_impl->oggBuffer);
    std::free(m_impl->oggSamples);
    m_impl->oggSamples = nullptr;
    m_impl->soundInitialized = false;
    m_impl->bufferInitialized = false;
    m_impl->loadedPath.clear();
    m_impl->loadedBus.clear();
    m_impl->oggSampleRate = 0;
}

bool AudioSource::Play()
{
    if (!EnsureLoaded()) return false;
    ApplySettings();
    if (ma_sound_at_end(&m_impl->sound))
        ma_sound_seek_to_pcm_frame(&m_impl->sound, 0);
    return ma_sound_start(&m_impl->sound) == MA_SUCCESS;
}

void AudioSource::Pause()
{
    if (m_impl && m_impl->soundInitialized)
        ma_sound_stop(&m_impl->sound);
}

void AudioSource::Stop()
{
    Pause();
    if (m_impl && m_impl->soundInitialized)
        ma_sound_seek_to_pcm_frame(&m_impl->sound, 0);
}

bool AudioSource::IsPlaying() const
{
    return m_impl && m_impl->soundInitialized && ma_sound_is_playing(&m_impl->sound);
}

bool AudioSource::DrawProperties(IEditorUi& ui)
{
    const bool changed = Component::DrawProperties(ui);
    ui.Separator();
    if (ui.Button(IsPlaying() ? "Restart" : "Play", 70.f))
    {
        Stop();
        Play();
    }
    ui.SameLine();
    if (ui.Button("Pause", 70.f)) Pause();
    ui.SameLine();
    if (ui.Button("Stop", 70.f)) Stop();
    return changed;
}

void AudioSource::Start()
{
    if (playOnStart) Play();
    else EnsureLoaded();
}

void AudioSource::Update()
{
    const bool wasPlaying = IsPlaying();
    if (!EnsureLoaded()) return;
    ApplySettings();
    if (wasPlaying && !IsPlaying() && !ma_sound_at_end(&m_impl->sound))
        ma_sound_start(&m_impl->sound);
}

void AudioSource::Disabled() { Stop(); }
void AudioSource::OnDestroy() { Unload(); }

void AudioSource::ApplySettings()
{
    if (!m_impl || !m_impl->soundInitialized) return;
    ma_sound_set_volume(&m_impl->sound, std::max(0.f, volume));
    float sourceRateScale = 1.f;
    if (m_impl->oggSampleRate > 0)
    {
        auto* engine = static_cast<ma_engine*>(AudioMixer::Get().GetEngineHandle());
        const ma_uint32 outputRate = engine ? ma_engine_get_sample_rate(engine) : 0;
        if (outputRate > 0)
            sourceRateScale = static_cast<float>(m_impl->oggSampleRate) / outputRate;
    }
    ma_sound_set_pitch(&m_impl->sound, std::max(0.01f, pitch) * sourceRateScale);
    ma_sound_set_looping(&m_impl->sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_spatialization_enabled(&m_impl->sound, spatial ? MA_TRUE : MA_FALSE);
    ma_sound_set_min_distance(&m_impl->sound, std::max(0.f, minDistance));
    ma_sound_set_max_distance(&m_impl->sound, std::max(minDistance, maxDistance));
    ma_sound_set_rolloff(&m_impl->sound, std::max(0.f, rolloff));
    ma_sound_set_doppler_factor(&m_impl->sound, std::max(0.f, dopplerFactor));

    const std::string model = Lower(attenuation);
    const ma_attenuation_model attenuationModel = model == "none"
        ? ma_attenuation_model_none : model == "linear"
        ? ma_attenuation_model_linear : model == "exponential"
        ? ma_attenuation_model_exponential : ma_attenuation_model_inverse;
    ma_sound_set_attenuation_model(&m_impl->sound, attenuationModel);

    glm::vec3 position(0.f);
    glm::vec3 forward(0.f, 0.f, 1.f);
    if (Owner)
    {
        const glm::mat4 world = Owner->transform.GetWorldMatrix();
        position = glm::vec3(world[3]);
        forward = SafeDirection(glm::vec3(world[2]), forward);
    }
    ma_sound_set_position(&m_impl->sound, position.x, position.y, position.z);
    ma_sound_set_direction(&m_impl->sound, forward.x, forward.y, forward.z);
    const float clampedInner = std::clamp(coneInnerAngle, 0.f, 360.f);
    const float inner = directional ? glm::radians(clampedInner)
                                    : glm::two_pi<float>();
    const float outer = directional ? glm::radians(std::clamp(coneOuterAngle, clampedInner, 360.f))
                                    : glm::two_pi<float>();
    ma_sound_set_cone(&m_impl->sound, inner, outer, std::clamp(coneOuterGain, 0.f, 1.f));
}

void AudioSource::UpdateListener()
{
    if (!Owner || !Owner->GetScene()) return;
    Scene* scene = Owner->GetScene();
    Camera* camera = listenerCameraReference.IsAssigned()
        ? ResolveComponentReference<Camera>(Owner, listenerCameraReference)
        : scene->FindGameCamera();
    Object* cameraObject = camera ? camera->Owner : &scene->editorCamera;
    camera = camera ? camera : (listenerCameraReference.IsAssigned()
        ? nullptr : cameraObject->GetComponent<Camera>());
    if (!camera || !cameraObject) return;

    const glm::mat4 world = cameraObject->transform.GetWorldMatrix();
    const glm::vec3 position(world[3]);
    glm::vec3 forward;
    glm::vec3 up;
    if (!camera->useTransformRotation)
    {
        forward = SafeDirection(camera->target - position, glm::vec3(0.f, 0.f, 1.f));
        up = SafeDirection(camera->up, glm::vec3(0.f, 1.f, 0.f));
    }
    else
    {
        forward = SafeDirection(glm::vec3(world[2]), glm::vec3(0.f, 0.f, 1.f));
        up = SafeDirection(glm::vec3(world[1]), glm::vec3(0.f, 1.f, 0.f));
    }
    AudioMixer::Get().SetListener(position, forward, up);
}
