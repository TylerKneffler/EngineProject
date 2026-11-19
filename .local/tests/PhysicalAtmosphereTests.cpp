// Sanity checks for Rendering/Atmosphere/PhysicalAtmosphere (Hillaire 2020
// model): transmittance, multiple scattering and sky radiance behave like an
// Earth atmosphere seen from the ground. Also checks the split-sum BRDF
// integration that turns the sky's environment map into reflections.
#include "Core/Rendering/Atmosphere/PhysicalAtmosphere.h"
#include "Core/Rendering/RenderUtilities.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using Engine::Rendering::PhysicalAtmosphere;
using Engine::Rendering::PhysicalAtmosphereParameters;

namespace
{
int g_failures = 0;
void Require(bool condition, const char* what)
{
    if (!condition) { std::printf("FAILED: %s\n", what); ++g_failures; }
}
void Print(const char* label, const glm::vec3& value)
{
    std::printf("%-34s %.5g %.5g %.5g\n", label, value.r, value.g, value.b);
}
glm::vec3 FromElevation(float degrees, float azimuthDegrees = 0.f)
{
    const float e = glm::radians(degrees), a = glm::radians(azimuthDegrees);
    return { std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a) };
}
}

int main()
{
    const PhysicalAtmosphere atmosphere{ PhysicalAtmosphereParameters{} };

    const glm::vec3 zenithT = atmosphere.Transmittance(0.f, 1.f);
    const glm::vec3 lowT = atmosphere.Transmittance(0.f, std::sin(glm::radians(5.f)));
    Print("transmittance, zenith", zenithT);
    Print("transmittance, 5 deg elevation", lowT);
    Require(zenithT.r > 0.8f && zenithT.r < 0.99f, "zenith transmittance near 0.9 in red");
    Require(zenithT.b < zenithT.g && zenithT.g < zenithT.r, "blue extinguished most");
    Require(lowT.r > lowT.b * 2.f, "low sun is reddened");
    Require(atmosphere.Transmittance(0.f, -0.2f) == glm::vec3(0.f), "below horizon is occluded");
    Require(atmosphere.Transmittance(30000.f, 1.f).b > zenithT.b, "thinner air above");

    const glm::vec3 psi = atmosphere.MultipleScattering(0.f, std::sin(glm::radians(40.f)));
    Print("multiple scattering psi, 40 deg", psi);
    Require(psi.b > 0.f && psi.b < 1.f, "psi small and positive");

    const glm::vec3 sun = FromElevation(40.f);
    const glm::vec3 zenith = atmosphere.SkyRadiance(500.f, { 0.f, 1.f, 0.f }, sun);
    const glm::vec3 horizonAway = atmosphere.SkyRadiance(500.f, FromElevation(2.f, 180.f), sun);
    const glm::vec3 nearSun = atmosphere.SkyRadiance(500.f, FromElevation(35.f), sun);
    const glm::vec3 ground = atmosphere.SkyRadiance(500.f, FromElevation(-30.f), sun);
    Print("sky radiance, zenith", zenith);
    Print("sky radiance, horizon away", horizonAway);
    Print("sky radiance, 5 deg from sun", nearSun);
    Print("radiance looking down", ground);
    Require(zenith.b > zenith.r * 1.5f, "zenith is blue");
    Require(zenith.b > 0.01f && zenith.b < 0.5f, "zenith radiance plausible per unit illuminance");
    Require(horizonAway.r / horizonAway.b > zenith.r / zenith.b, "horizon whiter than zenith");
    Require(glm::length(nearSun) > glm::length(zenith), "aureole around the sun");
    Require(ground.g > 0.f, "ground reflection");

    const glm::vec3 sunset = atmosphere.SkyRadiance(500.f, FromElevation(2.f), FromElevation(1.f));
    Print("sky near a sunset sun", sunset);
    Require(sunset.r > sunset.b, "sunset glow is warm");

    const auto panorama = atmosphere.RenderPanorama(64, 32, 500.f, sun, glm::vec3(3.f), 4);
    Require(panorama.size() == 64u * 32u * 4u, "panorama size");
    // Row 0 is the zenith (v = acos(y) / pi).
    Require(std::abs(panorama[2] - zenith.b * 3.f) < zenith.b * 0.3f, "panorama top row is the zenith");

    // Split-sum BRDF LUT (scale, bias): a directional albedo, so each term
    // and their sum stay within [0, 1] (Fresnel = 1 gives scale + bias).
    float worstSum = 0.f;
    for (float noV : { 0.004f, 0.1f, 0.5f, 1.f })
        for (float roughness : { 0.05f, 0.5f, 0.86f, 1.f })
        {
            const glm::vec2 brdf = Engine::Rendering::IntegrateBrdf(noV, roughness);
            worstSum = std::max(worstSum, brdf.x + brdf.y);
            Require(brdf.x >= 0.f && brdf.y >= 0.f, "BRDF LUT terms non-negative");
        }
    std::printf("%-34s %.5g\n", "BRDF LUT max scale + bias", worstSum);
    Require(worstSum <= 1.02f && worstSum > 0.5f, "BRDF LUT scale + bias within [0, 1]");

    if (g_failures) return EXIT_FAILURE;
    std::printf("PhysicalAtmosphereTests passed\n");
    return EXIT_SUCCESS;
}
