#include "Core/Compoonents/Audio/AudioSource.h"

#include "Core/Audio/AudioMixer.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Editor/UI/IEditorUi.h"
#include <miniaudio.h>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <atomic>
#include <memory>
#include <vector>

extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
}

namespace Engine::Components
{

namespace
{
glm::mat4 SpatialAudioWorldMatrix(const Engine::Core::Object& object)
{
    glm::mat4 world = object.transform.GetWorldMatrix();
    if (object.transform.matrixLayer.enabled)
        world = object.transform.matrixLayer.localToLayer * world;
    if (Engine::Scene::Scene* scene = object.GetScene())
    {
        world = scene->MapSpatialMatrix(world,
            { Engine::Scene::Scene::SpatialQueryDomain::Audio, &object });
    }
    return world;
}
}

#ifndef ENGINE_ASSETS_PATH
#define ENGINE_ASSETS_PATH "Engine/Core/Assets/"
#endif

struct AudioSource::Impl
{
    ma_sound sound{};
    ma_audio_buffer oggBuffer{};
    struct Decoded
    {
        std::vector<short> samples;
        ma_uint64 frames = 0;
        int channels = 0;
        int sampleRate = 0;
        std::atomic<bool> ready{ false };
        bool succeeded = false;
    };
    std::shared_ptr<Decoded> pending;
    std::shared_ptr<std::atomic<bool>> commandAlive =
        std::make_shared<std::atomic<bool>>(true);
    std::vector<short> samples;
    bool playRequested = false;
    bool soundInitialized = false;
    bool bufferInitialized = false;
    std::string loadedPath;
    std::string loadedBus;
    int oggSampleRate = 0;
    uint64_t appliedTransformRevision = 0;
    bool transportPlaying = false;
    double deterministicStartTime = 0.0;
    double deterministicOffset = 0.0;
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
    if (m_impl->loadedPath != audioPath || m_impl->loadedBus != bus)
    {
        const bool resume = m_impl->playRequested;
        Unload();
        m_impl->loadedPath = audioPath;
        m_impl->loadedBus = bus;
        m_impl->playRequested = resume;
        m_impl->transportPlaying = resume;
        if (resume && UsesDeterministicClock())
            m_impl->deterministicStartTime = Owner->GetScene()->GetElapsedTime();
    }

    if (!m_impl->pending)
    {
        auto decoded = std::make_shared<Impl::Decoded>();
        m_impl->pending = decoded;
        const auto path = audioPath;
        Engine::Audio::AudioMixer::Get().SubmitDecode([decoded, path] {
            try
            {
            const std::filesystem::path resolved = ResolveAudioPath(path);
            if (Lower(resolved.extension().string()) == ".ogg")
            {
                short* samples = nullptr;
                int channels = 0, sampleRate = 0;
                const int frames = stb_vorbis_decode_filename(resolved.string().c_str(),
                    &channels, &sampleRate, &samples);
                if (frames > 0 && channels > 0 && sampleRate > 0 && samples)
                {
                    decoded->samples.assign(samples,
                        samples + static_cast<size_t>(frames) * channels);
                    decoded->frames = static_cast<ma_uint64>(frames);
                    decoded->channels = channels;
                    decoded->sampleRate = sampleRate;
                    decoded->succeeded = true;
                }
                std::free(samples);
            }
            else
            {
                ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
                ma_uint64 frames = 0;
                void* pcm = nullptr;
                if (ma_decode_file(resolved.string().c_str(), &config, &frames, &pcm) == MA_SUCCESS &&
                    pcm && frames > 0 && config.channels > 0 && config.sampleRate > 0)
                {
                    const auto* samples = static_cast<const short*>(pcm);
                    decoded->samples.assign(samples,
                        samples + static_cast<size_t>(frames) * config.channels);
                    decoded->frames = frames;
                    decoded->channels = static_cast<int>(config.channels);
                    decoded->sampleRate = static_cast<int>(config.sampleRate);
                    decoded->succeeded = true;
                }
                if (pcm) ma_free(pcm, nullptr);
            }
            }
            catch (...) { decoded->succeeded = false; }
            decoded->ready.store(true, std::memory_order_release);
        });
        return false;
    }
    if (!m_impl->pending->ready.load(std::memory_order_acquire)) return false;
    if (!m_impl->pending->succeeded) return false;

    auto* engine = static_cast<ma_engine*>(Engine::Audio::AudioMixer::Get().GetEngineHandle());
    auto* group = static_cast<ma_sound_group*>(Engine::Audio::AudioMixer::Get().GetBusHandle(bus));
    if (!engine || !group) return false;

    m_impl->samples = std::move(m_impl->pending->samples);
    m_impl->oggSampleRate = m_impl->pending->sampleRate;
    const ma_audio_buffer_config config = ma_audio_buffer_config_init(ma_format_s16,
        static_cast<ma_uint32>(m_impl->pending->channels), m_impl->pending->frames,
        m_impl->samples.data(), nullptr);
    ma_result result = ma_audio_buffer_init(&config, &m_impl->oggBuffer);
    m_impl->bufferInitialized = result == MA_SUCCESS;
    if (result == MA_SUCCESS)
        result = ma_sound_init_from_data_source(engine, &m_impl->oggBuffer, 0, group, &m_impl->sound);

    m_impl->soundInitialized = result == MA_SUCCESS;
    if (!m_impl->soundInitialized)
    {
        Unload();
        return false;
    }
    m_impl->pending.reset();
    ApplySettings();
    if (m_impl->playRequested)
    {
        auto alive = m_impl->commandAlive;
        auto* impl = m_impl;
        Engine::Audio::AudioMixer::Get().EnqueueCommand([alive, impl] {
            if (alive->load(std::memory_order_acquire) && impl->soundInitialized)
            {
                if (ma_sound_at_end(&impl->sound)) ma_sound_seek_to_pcm_frame(&impl->sound, 0);
                ma_sound_start(&impl->sound);
            }
        });
    }
    return true;
}

void AudioSource::Unload()
{
    if (!m_impl) return;
    m_impl->commandAlive->store(false, std::memory_order_release);
    m_impl->commandAlive = std::make_shared<std::atomic<bool>>(true);
    if (m_impl->soundInitialized)
        ma_sound_uninit(&m_impl->sound);
    if (m_impl->bufferInitialized)
        ma_audio_buffer_uninit(&m_impl->oggBuffer);
    m_impl->samples.clear();
    m_impl->pending.reset();
    m_impl->soundInitialized = false;
    m_impl->bufferInitialized = false;
    m_impl->loadedPath.clear();
    m_impl->loadedBus.clear();
    m_impl->oggSampleRate = 0;
    m_impl->appliedTransformRevision = 0;
    m_impl->transportPlaying = false;
    m_impl->playRequested = false;
    m_impl->deterministicStartTime = 0.0;
    m_impl->deterministicOffset = 0.0;
}

bool AudioSource::Play()
{
    if (!m_impl || audioPath.empty()) return false;
    const bool loaded = EnsureLoaded();
    m_impl->playRequested = true;
    m_impl->transportPlaying = true;
    if (UsesDeterministicClock())
        m_impl->deterministicStartTime = Owner->GetScene()->GetElapsedTime();
    if (!loaded) return true;
    ApplySettings();
    if (UsesDeterministicClock())
    {
        SynchronizeToSceneClock();
        return true;
    }
    auto alive = m_impl->commandAlive;
    auto* impl = m_impl;
    Engine::Audio::AudioMixer::Get().EnqueueCommand([alive, impl] {
        if (alive->load(std::memory_order_acquire) && impl->soundInitialized)
        {
            if (ma_sound_at_end(&impl->sound)) ma_sound_seek_to_pcm_frame(&impl->sound, 0);
            ma_sound_start(&impl->sound);
        }
    });
    return true;
}

void AudioSource::Pause()
{
    if (m_impl)
    {
        m_impl->playRequested = false;
        if (!m_impl->soundInitialized) m_impl->transportPlaying = false;
    }
    if (m_impl && m_impl->soundInitialized)
    {
        if (UsesDeterministicClock())
        {
            SynchronizeToSceneClock();
            ma_uint64 cursor = 0;
            ma_uint32 sampleRate = 0;
            if (ma_sound_get_cursor_in_pcm_frames(&m_impl->sound, &cursor) == MA_SUCCESS &&
                ma_sound_get_data_format(&m_impl->sound, nullptr, nullptr,
                    &sampleRate, nullptr, 0) == MA_SUCCESS && sampleRate > 0)
                m_impl->deterministicOffset = static_cast<double>(cursor) / sampleRate;
        }
        m_impl->transportPlaying = false;
        auto alive = m_impl->commandAlive;
        auto* impl = m_impl;
        Engine::Audio::AudioMixer::Get().EnqueueCommand([alive, impl] {
            if (alive->load(std::memory_order_acquire) && impl->soundInitialized)
                ma_sound_stop(&impl->sound);
        });
    }
}

void AudioSource::Stop()
{
    Pause();
    if (m_impl) m_impl->deterministicOffset = 0.0;
    if (m_impl && m_impl->soundInitialized)
    {
        auto alive = m_impl->commandAlive;
        auto* impl = m_impl;
        Engine::Audio::AudioMixer::Get().EnqueueCommand([alive, impl] {
            if (alive->load(std::memory_order_acquire) && impl->soundInitialized)
                ma_sound_seek_to_pcm_frame(&impl->sound, 0);
        });
    }
}

bool AudioSource::IsPlaying() const
{
    return m_impl && m_impl->soundInitialized &&
        (UsesDeterministicClock() ? m_impl->transportPlaying
                                  : ma_sound_is_playing(&m_impl->sound));
}

bool AudioSource::DrawProperties(::Engine::Editor::IEditorUi& ui)
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
    if (UsesDeterministicClock())
    {
        if (m_impl->transportPlaying)
            SynchronizeToSceneClock();
        return;
    }
    if (wasPlaying && !IsPlaying() && !ma_sound_at_end(&m_impl->sound))
    {
        auto alive = m_impl->commandAlive;
        auto* impl = m_impl;
        Engine::Audio::AudioMixer::Get().EnqueueCommand([alive, impl] {
            if (alive->load(std::memory_order_acquire) && impl->soundInitialized)
                ma_sound_start(&impl->sound);
        });
    }
}

bool AudioSource::UsesDeterministicClock() const
{
    return Owner && Owner->GetScene() && Owner->GetScene()->GetClock().IsFixedStep();
}

void AudioSource::SynchronizeToSceneClock()
{
    if (!Owner || !Owner->GetScene()) return;
    SynchronizeToTime(Owner->GetScene()->GetElapsedTime());
}

void AudioSource::SynchronizeToTime(double sceneTime)
{
    if (!m_impl || !m_impl->soundInitialized || !UsesDeterministicClock() ||
        !m_impl->transportPlaying)
        return;
    ma_uint32 sampleRate = 0;
    ma_uint64 length = 0;
    if (ma_sound_get_data_format(&m_impl->sound, nullptr, nullptr, &sampleRate,
            nullptr, 0) != MA_SUCCESS || sampleRate == 0 ||
        ma_sound_get_length_in_pcm_frames(&m_impl->sound, &length) != MA_SUCCESS ||
        length == 0)
        return;
    const double elapsed = std::max(0.0,
        sceneTime - m_impl->deterministicStartTime);
    double sourceSeconds = m_impl->deterministicOffset + elapsed *
        static_cast<double>(std::max(0.01f, pitch));
    const double lengthSeconds = static_cast<double>(length) / sampleRate;
    if (loop)
        sourceSeconds = std::fmod(sourceSeconds, lengthSeconds);
    else if (sourceSeconds >= lengthSeconds)
    {
        sourceSeconds = lengthSeconds;
        m_impl->transportPlaying = false;
    }
    const ma_uint64 frame = std::min(length,
        static_cast<ma_uint64>(sourceSeconds * sampleRate));
    ma_uint64 currentFrame = 0u;
    const bool cursorAvailable = ma_sound_get_cursor_in_pcm_frames(
        &m_impl->sound, &currentFrame) == MA_SUCCESS;
    if (!cursorAvailable || currentFrame != frame)
    {
        ma_sound_stop(&m_impl->sound);
        ma_sound_seek_to_pcm_frame(&m_impl->sound, frame);
    }
    if (frame < length && !ma_sound_is_playing(&m_impl->sound))
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
        auto* engine = static_cast<ma_engine*>(Engine::Audio::AudioMixer::Get().GetEngineHandle());
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
    uint64_t transformRevision = 0;
    if (Owner)
    {
        transformRevision = Owner->transform.GetWorldRevision();
        const glm::mat4 world = SpatialAudioWorldMatrix(*Owner);
        position = glm::vec3(world[3]);
        forward = SafeDirection(glm::vec3(world[2]), forward);
    }
    // A containing nonlinear space can change while the source's Euclidean
    // transform revision stays fixed, so mapped spatial audio is refreshed.
    ma_sound_set_position(&m_impl->sound, position.x, position.y, position.z);
    ma_sound_set_direction(&m_impl->sound, forward.x, forward.y, forward.z);
    m_impl->appliedTransformRevision = transformRevision;
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
    Engine::Scene::Scene* scene = Owner->GetScene();
    Camera* camera = listenerCameraReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Camera>(Owner, listenerCameraReference)
        : scene->FindGameCamera();
    Object* cameraObject = camera ? camera->Owner : &scene->editorCamera;
    camera = camera ? camera : (listenerCameraReference.IsAssigned()
        ? nullptr : cameraObject->GetComponent<Camera>());
    if (!camera || !cameraObject) return;

    const glm::mat4 world = SpatialAudioWorldMatrix(*cameraObject);
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
    Engine::Audio::AudioMixer::Get().SetListener(position, forward, up);
}
}
