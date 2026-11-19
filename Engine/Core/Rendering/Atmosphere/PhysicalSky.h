#pragma once

#include "Core/Rendering/Atmosphere/PhysicalAtmosphere.h"

#include <future>
#include <memory>
#include <string>

namespace Engine::Components { class Texture; }

namespace Engine::Rendering
{
// Owns the physical atmosphere model and the sky panorama it renders, and
// rebuilds them on a worker when their inputs move: the atmosphere
// parameters, the sun direction (by more than ~0.2 degrees), the sun
// illuminance (by more than 1%) or the camera altitude (by more than 100 m;
// the sky changes slowly with height). Each panorama also gets its
// environment lighting (EnvironmentMap) built on the worker, so the scene
// thread only uploads. Until the first build finishes, Atmosphere() and
// Panorama() are null and callers fall back to the HDRI sky.
class PhysicalSky
{
public:
    struct Inputs
    {
        PhysicalAtmosphereParameters parameters;
        float altitude = 0.f;          // camera height above the planet surface
        glm::vec3 toSun{ 0.f, 1.f, 0.f };
        glm::vec3 sunIlluminance{ 1.f }; // before the atmosphere
    };

    PhysicalSky() = default;
    PhysicalSky(const PhysicalSky&) = delete;
    PhysicalSky& operator=(const PhysicalSky&) = delete;
    ~PhysicalSky();

    // Scene thread: collects a finished build and starts the next one when
    // the inputs moved enough.
    void Update(const Inputs& inputs);
    void Reset();

    const std::shared_ptr<const PhysicalAtmosphere>& Atmosphere() const { return m_atmosphere; }
    // RGBA32F equirect sky radiance (with mips), in world space.
    const std::shared_ptr<Engine::Components::Texture>& Panorama() const { return m_panorama; }

    static constexpr uint32_t PanoramaWidth = 512, PanoramaHeight = 256;

private:
    struct Build
    {
        std::shared_ptr<const PhysicalAtmosphere> atmosphere;
        std::shared_ptr<Engine::Components::Texture> panorama;
        std::string path;
    };
    static bool Moved(const Inputs& previous, const Inputs& next);

    std::future<Build> m_job;
    Inputs m_requested;
    bool m_hasRequest = false;
    uint64_t m_generation = 0;
    std::shared_ptr<const PhysicalAtmosphere> m_atmosphere;
    std::shared_ptr<Engine::Components::Texture> m_panorama;
    std::string m_panoramaPath;
};
}
