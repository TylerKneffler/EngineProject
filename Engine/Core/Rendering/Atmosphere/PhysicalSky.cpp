#include "Core/Rendering/Atmosphere/PhysicalSky.h"
#include "Core/Components/Materials/Texture.h"
#include "Core/Rendering/Lighting/EnvironmentMap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace Engine::Rendering
{
namespace
{
// RGBA32F box-filtered mip chain, mip 0 first.
std::vector<uint8_t> WithMips(const std::vector<float>& base, uint32_t width, uint32_t height,
    uint32_t& levels)
{
    std::vector<float> chain = base;
    std::vector<float> level = base;
    levels = 1;
    uint32_t w = width, h = height;
    while (w > 1 || h > 1)
    {
        const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        std::vector<float> next(size_t(nw) * nh * 4, 0.f);
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x)
                for (uint32_t c = 0; c < 4; ++c)
                {
                    float sum = 0.f;
                    for (uint32_t dy = 0; dy < 2; ++dy)
                        for (uint32_t dx = 0; dx < 2; ++dx)
                            sum += level[(size_t(std::min(y * 2 + dy, h - 1)) * w +
                                std::min(x * 2 + dx, w - 1)) * 4 + c];
                    next[(size_t(y) * nw + x) * 4 + c] = sum * 0.25f;
                }
        chain.insert(chain.end(), next.begin(), next.end());
        level = std::move(next);
        w = nw; h = nh;
        ++levels;
    }
    std::vector<uint8_t> bytes(chain.size() * sizeof(float));
    std::memcpy(bytes.data(), chain.data(), bytes.size());
    return bytes;
}
}

PhysicalSky::~PhysicalSky()
{
    if (m_job.valid()) m_job.wait();
}

void PhysicalSky::Reset()
{
    if (m_job.valid()) m_job.wait();
    m_job = {};
    if (!m_panoramaPath.empty()) EnvironmentMap::Invalidate(m_panoramaPath);
    m_atmosphere.reset();
    m_panorama.reset();
    m_panoramaPath.clear();
    m_hasRequest = false;
}

bool PhysicalSky::Moved(const Inputs& previous, const Inputs& next)
{
    if (previous.parameters != next.parameters) return true;
    if (std::abs(previous.altitude - next.altitude) > 100.f) return true;
    if (glm::dot(previous.toSun, next.toSun) < std::cos(glm::radians(0.2f))) return true;
    const float before = std::max({ previous.sunIlluminance.r, previous.sunIlluminance.g,
        previous.sunIlluminance.b, 1e-6f });
    return glm::length(next.sunIlluminance - previous.sunIlluminance) > 0.01f * before;
}

void PhysicalSky::Update(const Inputs& inputs)
{
    if (m_job.valid() && m_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        Build build = m_job.get();
        if (build.atmosphere && build.panorama)
        {
            // The previous panorama's lighting is no longer reachable.
            if (!m_panoramaPath.empty()) EnvironmentMap::Invalidate(m_panoramaPath);
            m_atmosphere = std::move(build.atmosphere);
            m_panorama = std::move(build.panorama);
            m_panoramaPath = std::move(build.path);
        }
    }
    if (m_job.valid() || (m_hasRequest && !Moved(m_requested, inputs)))
        return;

    m_requested = inputs;
    m_hasRequest = true;
    Inputs request = inputs;
    request.toSun = glm::length(request.toSun) > 1e-6f
        ? glm::normalize(request.toSun) : glm::vec3(0.f, 1.f, 0.f);
    // Reuse the model (its LUTs) unless the media changed.
    std::shared_ptr<const PhysicalAtmosphere> reuse =
        m_atmosphere && m_atmosphere->Parameters() == request.parameters ? m_atmosphere : nullptr;
    const std::string path = "physical-sky/" + std::to_string(++m_generation) + ".hdr";
    m_job = std::async(std::launch::async, [request, reuse, path]() -> Build
    {
        Build build;
        build.atmosphere = reuse ? reuse
            : std::make_shared<const PhysicalAtmosphere>(request.parameters);
        const unsigned threads = std::max(1u, std::thread::hardware_concurrency() / 2u);
        const auto radiance = build.atmosphere->RenderPanorama(PanoramaWidth, PanoramaHeight,
            request.altitude, request.toSun, request.sunIlluminance, threads);
        uint32_t levels = 0;
        auto pixels = WithMips(radiance, PanoramaWidth, PanoramaHeight, levels);
        build.panorama = Engine::Components::Texture::CreateFromPixels(path, PanoramaWidth,
            PanoramaHeight, levels, Engine::Graphics::GraphicsTextureFormat::Rgba32Float,
            std::move(pixels), false);
        // Ambient SH and reflections from the same sky, built here rather
        // than on the scene thread (EnvironmentMap is cached by path).
        if (build.panorama)
            EnvironmentMap::Acquire(*build.panorama);
        build.path = path;
        return build;
    });
}
}
