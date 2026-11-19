#include "Core/Rendering/Atmosphere/PhysicalAtmosphere.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace Engine::Rendering
{
namespace
{
constexpr double Pi = 3.14159265358979323846;
constexpr double Rg = PhysicalAtmosphere::GroundRadius;
constexpr double Rt = PhysicalAtmosphere::TopRadius;

double SafeSqrt(double value) { return std::sqrt(std::max(value, 0.0)); }

// Planet-scale geometry in double: radii of ~6.4e6 m squared lose all
// precision in float.
bool RayHitsGround(double r, double mu)
{
    return mu < 0.0 && r * r * (mu * mu - 1.0) + Rg * Rg >= 0.0;
}

double DistanceToTop(double r, double mu)
{
    return std::max(0.0, -r * mu + SafeSqrt(r * r * (mu * mu - 1.0) + Rt * Rt));
}

double DistanceToGround(double r, double mu)
{
    return std::max(0.0, -r * mu - SafeSqrt(r * r * (mu * mu - 1.0) + Rg * Rg));
}

float RayleighPhase(float cosTheta)
{
    return static_cast<float>(3.0 / (16.0 * Pi)) * (1.f + cosTheta * cosTheta);
}

float MiePhase(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denominator = std::max(1.f + g2 - 2.f * g * cosTheta, 1e-4f);
    return static_cast<float>(1.0 / (4.0 * Pi)) * (1.f - g2) /
        (denominator * std::sqrt(denominator));
}

// Integral of a constant source S over a step of extinction sigma and length
// dt, seen through the step itself: S * (1 - exp(-sigma dt)) / sigma.
glm::vec3 StepIntegral(const glm::vec3& source, const glm::vec3& extinction, float dt)
{
    glm::vec3 result;
    for (int c = 0; c < 3; ++c)
        result[c] = extinction[c] > 1e-12f
            ? source[c] * (1.f - std::exp(-extinction[c] * dt)) / extinction[c]
            : source[c] * dt;
    return result;
}

glm::vec3 Exp(const glm::vec3& value)
{
    return { std::exp(value.x), std::exp(value.y), std::exp(value.z) };
}

glm::vec3 Bilinear(const std::vector<glm::vec3>& table, int width, int height,
    float u, float v)
{
    const float x = std::clamp(u * width - 0.5f, 0.f, float(width - 1));
    const float y = std::clamp(v * height - 0.5f, 0.f, float(height - 1));
    const int x0 = std::min(static_cast<int>(x), width - 2 < 0 ? 0 : width - 2);
    const int y0 = std::min(static_cast<int>(y), height - 2 < 0 ? 0 : height - 2);
    const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const float tx = x - x0, ty = y - y0;
    const auto at = [&](int px, int py) { return table[size_t(py) * width + px]; };
    return glm::mix(glm::mix(at(x0, y0), at(x1, y0), tx),
        glm::mix(at(x0, y1), at(x1, y1), tx), ty);
}
}

PhysicalAtmosphere::PhysicalAtmosphere(const PhysicalAtmosphereParameters& parameters)
    : m_parameters(parameters)
{
    m_parameters.mieScaleHeight = std::max(m_parameters.mieScaleHeight, 10.f);
    m_parameters.mieAnisotropy = std::clamp(m_parameters.mieAnisotropy, 0.f, 0.99f);

    // Transmittance LUT, Bruneton's mapping: x = distance to the top between
    // its minimum and maximum, y = sqrt-distributed radius.
    const double horizon = SafeSqrt(Rt * Rt - Rg * Rg);
    m_transmittance.resize(size_t(TransmittanceWidth) * TransmittanceHeight);
    for (int y = 0; y < TransmittanceHeight; ++y)
        for (int x = 0; x < TransmittanceWidth; ++x)
        {
            const double xMu = (x + 0.5) / TransmittanceWidth;
            const double xR = (y + 0.5) / TransmittanceHeight;
            const double rho = horizon * xR;
            const double r = std::sqrt(rho * rho + Rg * Rg);
            const double dMin = Rt - r, dMax = rho + horizon;
            const double d = dMin + xMu * (dMax - dMin);
            const double mu = d <= 0.0 ? 1.0
                : std::clamp((horizon * horizon - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
            m_transmittance[size_t(y) * TransmittanceWidth + x] =
                IntegrateTransmittance(static_cast<float>(r), static_cast<float>(mu));
        }

    m_multipleScattering.resize(size_t(MultipleScatteringSize) * MultipleScatteringSize);
    for (int y = 0; y < MultipleScatteringSize; ++y)
        for (int x = 0; x < MultipleScatteringSize; ++x)
        {
            const float cosSun = ((x + 0.5f) / MultipleScatteringSize) * 2.f - 1.f;
            const float altitude = ((y + 0.5f) / MultipleScatteringSize) * float(Rt - Rg);
            m_multipleScattering[size_t(y) * MultipleScatteringSize + x] =
                IntegrateMultipleScattering(altitude, cosSun);
        }
}

PhysicalAtmosphere::Medium PhysicalAtmosphere::SampleMedium(float altitude) const
{
    altitude = std::max(altitude, 0.f);
    const float rayleighDensity = std::exp(-altitude / RayleighScaleHeight);
    const float mieDensity = std::exp(-altitude / m_parameters.mieScaleHeight);
    const float ozoneDensity = std::max(0.f, 1.f - std::abs(altitude - 25000.f) / 15000.f);
    Medium medium;
    medium.rayleighScattering = RayleighScattering * m_parameters.rayleighScale * rayleighDensity;
    medium.mieScattering = MieScattering * m_parameters.mieScale * mieDensity;
    medium.extinction = medium.rayleighScattering +
        glm::vec3(MieExtinction * m_parameters.mieScale * mieDensity) +
        OzoneAbsorption * m_parameters.ozoneScale * ozoneDensity;
    return medium;
}

glm::vec3 PhysicalAtmosphere::IntegrateTransmittance(float radius, float cosZenith) const
{
    constexpr int Steps = 40;
    const double r = radius, mu = cosZenith;
    const double length = DistanceToTop(r, mu);
    const double dt = length / Steps;
    glm::vec3 opticalDepth(0.f);
    for (int i = 0; i < Steps; ++i)
    {
        const double t = (i + 0.5) * dt;
        const double sampleRadius = SafeSqrt(r * r + t * t + 2.0 * r * mu * t);
        opticalDepth += SampleMedium(static_cast<float>(sampleRadius - Rg)).extinction *
            static_cast<float>(dt);
    }
    return Exp(-opticalDepth);
}

glm::vec3 PhysicalAtmosphere::Transmittance(float altitude, float cosZenith) const
{
    const double r = Rg + std::clamp(static_cast<double>(altitude), 0.0, Rt - Rg - 1.0);
    const double mu = std::clamp(static_cast<double>(cosZenith), -1.0, 1.0);
    if (RayHitsGround(r, mu))
        return glm::vec3(0.f);
    const double horizon = SafeSqrt(Rt * Rt - Rg * Rg);
    const double rho = SafeSqrt(r * r - Rg * Rg);
    const double d = DistanceToTop(r, mu);
    const double dMin = Rt - r, dMax = rho + horizon;
    const float u = static_cast<float>(dMax > dMin ? (d - dMin) / (dMax - dMin) : 0.0);
    const float v = static_cast<float>(rho / horizon);
    return Bilinear(m_transmittance, TransmittanceWidth, TransmittanceHeight, u, v);
}

glm::vec3 PhysicalAtmosphere::IntegrateMultipleScattering(float altitude, float cosSunZenith) const
{
    // Hillaire 2020, section 5.5: second-order isotropic scattering L2 and the
    // transfer factor f_ms over the sphere of directions; Psi = L2 / (1 - f_ms)
    // sums the geometric series of all higher orders.
    constexpr int SqrtDirections = 8, Steps = 20;
    const double r = Rg + std::max(static_cast<double>(altitude), 1.0);
    const glm::dvec3 position(0.0, r, 0.0);
    const glm::vec3 toSun(SafeSqrt(1.0 - double(cosSunZenith) * cosSunZenith), cosSunZenith, 0.f);
    const float isotropicPhase = static_cast<float>(1.0 / (4.0 * Pi));
    glm::vec3 secondOrder(0.f), transfer(0.f);
    for (int i = 0; i < SqrtDirections; ++i)
        for (int j = 0; j < SqrtDirections; ++j)
        {
            const double cosTheta = 1.0 - 2.0 * (j + 0.5) / SqrtDirections;
            const double phi = 2.0 * Pi * (i + 0.5) / SqrtDirections;
            const double sinTheta = SafeSqrt(1.0 - cosTheta * cosTheta);
            const glm::dvec3 direction(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
            const bool ground = RayHitsGround(r, cosTheta);
            const double length = ground ? DistanceToGround(r, cosTheta) : DistanceToTop(r, cosTheta);
            const float dt = static_cast<float>(length / Steps);
            glm::vec3 transmittance(1.f), radiance(0.f), scattered(0.f);
            for (int k = 0; k < Steps; ++k)
            {
                const glm::dvec3 sample = position + direction * ((k + 0.5) * double(dt));
                const double sampleRadius = glm::length(sample);
                const float sampleAltitude = static_cast<float>(sampleRadius - Rg);
                const Medium medium = SampleMedium(sampleAltitude);
                const glm::vec3 scattering = medium.rayleighScattering + glm::vec3(medium.mieScattering);
                const float sunCos = static_cast<float>(glm::dot(sample / sampleRadius, glm::dvec3(toSun)));
                const glm::vec3 sunTransmittance = Transmittance(sampleAltitude, sunCos);
                radiance += transmittance * StepIntegral(scattering * sunTransmittance * isotropicPhase,
                    medium.extinction, dt);
                scattered += transmittance * StepIntegral(scattering, medium.extinction, dt);
                transmittance *= Exp(-medium.extinction * dt);
            }
            if (ground)
            {
                const glm::dvec3 hit = position + direction * length;
                const float sunCos = static_cast<float>(glm::dot(glm::normalize(hit), glm::dvec3(toSun)));
                radiance += transmittance * Transmittance(0.f, sunCos) * std::max(sunCos, 0.f) *
                    m_parameters.groundAlbedo * static_cast<float>(1.0 / Pi);
            }
            secondOrder += radiance;
            transfer += scattered;
        }
    const float directions = float(SqrtDirections * SqrtDirections);
    secondOrder /= directions;
    transfer = transfer * isotropicPhase * 4.f * static_cast<float>(Pi) / directions;
    return secondOrder / glm::max(glm::vec3(1.f) - transfer, glm::vec3(1e-3f));
}

glm::vec3 PhysicalAtmosphere::MultipleScattering(float altitude, float cosSunZenith) const
{
    return Bilinear(m_multipleScattering, MultipleScatteringSize, MultipleScatteringSize,
        std::clamp(cosSunZenith * 0.5f + 0.5f, 0.f, 1.f),
        std::clamp(altitude / float(Rt - Rg), 0.f, 1.f));
}

glm::vec3 PhysicalAtmosphere::SkyRadiance(float altitude, const glm::vec3& direction,
    const glm::vec3& toSun) const
{
    constexpr int Steps = 32;
    const double r = Rg + std::clamp(static_cast<double>(altitude), 1.0, Rt - Rg - 1.0);
    const glm::dvec3 position(0.0, r, 0.0);
    const glm::dvec3 view = glm::normalize(glm::dvec3(direction));
    const bool ground = RayHitsGround(r, view.y);
    const double length = ground ? DistanceToGround(r, view.y) : DistanceToTop(r, view.y);
    const float cosTheta = glm::dot(glm::vec3(view), toSun);
    const float rayleighPhase = RayleighPhase(cosTheta);
    const float miePhase = MiePhase(cosTheta, m_parameters.mieAnisotropy);
    glm::vec3 transmittance(1.f), radiance(0.f);
    // Quadratically spaced steps: dense near the camera, where the ray
    // crosses the bottom of the atmosphere and most of the scattering is.
    double previous = 0.0;
    for (int k = 0; k < Steps; ++k)
    {
        const double next = length * std::pow((k + 1.0) / Steps, 2.0);
        const double t = 0.5 * (previous + next);
        const float dt = static_cast<float>(next - previous);
        previous = next;
        const glm::dvec3 sample = position + view * t;
        const double sampleRadius = glm::length(sample);
        const float sampleAltitude = static_cast<float>(sampleRadius - Rg);
        const Medium medium = SampleMedium(sampleAltitude);
        const float sunCos = static_cast<float>(glm::dot(sample / sampleRadius, glm::dvec3(toSun)));
        const glm::vec3 sunTransmittance = Transmittance(sampleAltitude, sunCos);
        const glm::vec3 multiple = MultipleScattering(sampleAltitude, sunCos);
        const glm::vec3 source =
            (medium.rayleighScattering * rayleighPhase + glm::vec3(medium.mieScattering * miePhase)) *
                sunTransmittance +
            (medium.rayleighScattering + glm::vec3(medium.mieScattering)) * multiple;
        radiance += transmittance * StepIntegral(source, medium.extinction, dt);
        transmittance *= Exp(-medium.extinction * dt);
    }
    if (ground)
    {
        const glm::dvec3 hit = position + view * length;
        const float sunCos = static_cast<float>(glm::dot(glm::normalize(hit), glm::dvec3(toSun)));
        radiance += transmittance * Transmittance(0.f, sunCos) * std::max(sunCos, 0.f) *
            m_parameters.groundAlbedo * static_cast<float>(1.0 / Pi);
    }
    return radiance;
}

std::vector<float> PhysicalAtmosphere::RenderPanorama(uint32_t width, uint32_t height,
    float altitude, const glm::vec3& toSun, const glm::vec3& sunIlluminance,
    unsigned threads) const
{
    std::vector<float> pixels(size_t(width) * height * 4, 1.f);
    const glm::vec3 sun = glm::length(toSun) > 1e-6f ? glm::normalize(toSun) : glm::vec3(0.f, 1.f, 0.f);
    const auto renderRows = [&](uint32_t first, uint32_t step)
    {
        for (uint32_t y = first; y < height; y += step)
        {
            const double theta = Pi * (y + 0.5) / height;
            const float cosTheta = static_cast<float>(std::cos(theta));
            const float sinTheta = static_cast<float>(std::sin(theta));
            for (uint32_t x = 0; x < width; ++x)
            {
                const double phi = 2.0 * Pi * ((x + 0.5) / width - 0.5);
                const glm::vec3 direction(sinTheta * static_cast<float>(std::cos(phi)), cosTheta,
                    sinTheta * static_cast<float>(std::sin(phi)));
                const glm::vec3 radiance = SkyRadiance(altitude, direction, sun) * sunIlluminance;
                float* pixel = pixels.data() + (size_t(y) * width + x) * 4;
                pixel[0] = radiance.r; pixel[1] = radiance.g; pixel[2] = radiance.b; pixel[3] = 1.f;
            }
        }
    };
    threads = std::clamp(threads, 1u, 16u);
    std::vector<std::thread> workers;
    for (unsigned i = 1; i < threads; ++i)
        workers.emplace_back(renderRows, i, threads);
    renderRows(0, threads);
    for (auto& worker : workers) worker.join();
    return pixels;
}
}
