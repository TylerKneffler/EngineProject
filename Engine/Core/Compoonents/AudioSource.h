#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <string>

class AudioSource : public Script
{
public:
    AudioSource();
    ~AudioSource() override;

    PROPERTY(Inspector, EditAnywhere, Category = "Audio")
    std::string audioPath;
    PROPERTY(Inspector, EditAnywhere, Category = "Audio")
    std::string bus = "SFX";
    PROPERTY(Inspector, EditAnywhere, Category = "Audio")
    bool playOnStart = true;
    PROPERTY(Inspector, EditAnywhere, Category = "Audio")
    bool loop = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Audio", ClampMin = "0")
    float volume = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Audio", ClampMin = "0.01")
    float pitch = 1.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Audio | Spatial")
    bool spatial = true;
    // Empty follows the active game camera (or editor camera fallback).
    PROPERTY(Inspector, EditAnywhere, Category = "Audio | Spatial")
    ComponentReference listenerCameraReference { "Camera" };
    PROPERTY(Inspector, EditAnywhere, Category = "Audio | Spatial")
    std::string attenuation = "Inverse"; // None, Inverse, Linear or Exponential
    PROPERTY(Inspector, EditAnywhere, Category = "Audio | Spatial", ClampMin = "0")
    float minDistance = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Audio | Spatial", ClampMin = "0")
    float maxDistance = 100.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Audio | Spatial", ClampMin = "0")
    float rolloff = 1.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Audio | Spatial", ClampMin = "0")
    float dopplerFactor = 1.f;

    PROPERTY(Inspector, EditAnywhere, Category = "Audio | Directional")
    bool directional = false;
    PROPERTY(Inspector, EditAnywhere, Category = "Audio | Directional", Range = "0, 360")
    float coneInnerAngle = 90.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Audio | Directional", Range = "0, 360")
    float coneOuterAngle = 180.f;
    PROPERTY(Inspector, EditAnywhere, Category = "Audio | Directional", Range = "0, 1")
    float coneOuterGain = 0.f;

    bool Play();
    void Pause();
    void Stop();
    bool IsPlaying() const;
    bool DrawProperties(IEditorUi& ui) override;

    void Start() override;
    void Update() override;
    void Disabled() override;
    void OnDestroy() override;

private:
    bool EnsureLoaded();
    void Unload();
    void ApplySettings();
    void UpdateListener();

    struct Impl;
    Impl* m_impl = nullptr;
};
