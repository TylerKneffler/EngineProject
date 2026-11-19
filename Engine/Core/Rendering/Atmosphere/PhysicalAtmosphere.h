#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Engine::Rendering
{
// Physically based atmosphere after Hillaire 2020 ("A Scalable and
// Production Ready Sky and Atmosphere Rendering Technique"): an Earth-like
// planet (radius 6360 km, atmosphere 100 km) with Rayleigh scattering, Mie
// (aerosol) scattering and absorption, and an ozone absorption layer.
// Everything is computed on the CPU from two small LUTs:
//   transmittance LUT   256 x 64   (cos zenith x altitude), Bruneton mapping
//   multiple scattering 32 x 32    (cos sun zenith x altitude), Hillaire's
//                                  isotropic infinite-order approximation
// and used to render the sky radiance into an equirectangular panorama in
// world space (it replaces the sky-view LUT: the engine's sky, environment
// lighting and reflections all consume an equirect panorama), the sun's
// transmittance at the camera and the multiple-scattering source used by
// the analytic aerial perspective (Shaders/AtmosphereFog.hlsli).
// Radiance values are per unit of sun illuminance unless an illuminance is
// passed in. Distances are metres; altitude is above the planet surface.
struct PhysicalAtmosphereParameters
{
    float rayleighScale = 1.f;      // multiplies the Rayleigh coefficients
    float mieScale = 1.f;           // multiplies aerosol density (haze)
    float mieScaleHeight = 1200.f;  // metres
    float mieAnisotropy = 0.8f;     // Henyey-Greenstein g
    float ozoneScale = 1.f;
    glm::vec3 groundAlbedo{ 0.3f };

    bool operator==(const PhysicalAtmosphereParameters& other) const
    {
        return rayleighScale == other.rayleighScale && mieScale == other.mieScale &&
            mieScaleHeight == other.mieScaleHeight && mieAnisotropy == other.mieAnisotropy &&
            ozoneScale == other.ozoneScale && groundAlbedo == other.groundAlbedo;
    }
    bool operator!=(const PhysicalAtmosphereParameters& other) const { return !(*this == other); }
};

class PhysicalAtmosphere
{
public:
    static constexpr float GroundRadius = 6360000.f;
    static constexpr float TopRadius = 6460000.f;
    static constexpr float RayleighScaleHeight = 8000.f;
    // Sea-level coefficients (per metre), Hillaire 2020 / Bruneton 2017.
    inline static const glm::vec3 RayleighScattering{ 5.802e-6f, 13.558e-6f, 33.1e-6f };
    static constexpr float MieScattering = 3.996e-6f;
    static constexpr float MieExtinction = 4.40e-6f;
    inline static const glm::vec3 OzoneAbsorption{ 0.650e-6f, 1.881e-6f, 0.085e-6f };

    explicit PhysicalAtmosphere(const PhysicalAtmosphereParameters& parameters);

    const PhysicalAtmosphereParameters& Parameters() const { return m_parameters; }

    // Transmittance from a point at `altitude` along a ray with the given
    // cosine of its zenith angle to the top of the atmosphere; zero when the
    // ray hits the ground (e.g. toward a sun below the horizon).
    glm::vec3 Transmittance(float altitude, float cosZenith) const;
    // Multiple-scattering source Psi_ms: radiance per unit scattering
    // coefficient and sun illuminance, isotropic.
    glm::vec3 MultipleScattering(float altitude, float cosSunZenith) const;
    // Sky radiance seen from `altitude` looking along `direction` (unit,
    // world: +Y up), per unit sun illuminance arriving from `toSun`. Rays
    // that reach the ground include its Lambertian reflection. The sun disc
    // itself is not included (the skybox draws it).
    glm::vec3 SkyRadiance(float altitude, const glm::vec3& direction,
        const glm::vec3& toSun) const;
    // RGBA32F equirectangular panorama in the engine's convention
    // (u = 0.5 + atan2(z, x) / 2pi, v = acos(y) / pi, rows top to bottom),
    // scaled by sunIlluminance. Uses up to `threads` worker threads.
    std::vector<float> RenderPanorama(uint32_t width, uint32_t height,
        float altitude, const glm::vec3& toSun, const glm::vec3& sunIlluminance,
        unsigned threads) const;

    // Media at an altitude: scattering (Rayleigh rgb, Mie) and extinction.
    struct Medium
    {
        glm::vec3 rayleighScattering{ 0.f };
        float mieScattering = 0.f;
        glm::vec3 extinction{ 0.f };
    };
    Medium SampleMedium(float altitude) const;

private:
    glm::vec3 IntegrateTransmittance(float radius, float cosZenith) const;
    glm::vec3 IntegrateMultipleScattering(float altitude, float cosSunZenith) const;

    PhysicalAtmosphereParameters m_parameters;
    static constexpr int TransmittanceWidth = 256, TransmittanceHeight = 64;
    static constexpr int MultipleScatteringSize = 32;
    std::vector<glm::vec3> m_transmittance;
    std::vector<glm::vec3> m_multipleScattering;
};
}
