#pragma once

class Scene;

// Per-scene audio coordination. The process-wide output device and buses remain
// owned by AudioMixer; this class selects one listener and manages scene reset.
class Audio
{
public:
    explicit Audio(Scene& scene);

    void Update(float deltaTime);
    void Reset();

private:
    Scene* m_scene = nullptr;
};
