#pragma once

#include "Core/Graphics/IGraphicsTexture.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/vec4.hpp>

namespace Engine::Components { class Texture; }
namespace Engine::Graphics { class IGraphicsProvider; }

namespace Engine::Rendering
{
// Derived image-based-lighting data for an equirectangular source image.
// Cube faces are stored in a 3x2 atlas at every mip so the product remains
// portable across the render backends while retaining cube-map directions.
class EnvironmentMap final
{
public:
    static std::shared_ptr<EnvironmentMap> Acquire(
        const Engine::Components::Texture& source);
    static void Invalidate(const std::string& sourcePath);

    bool Prepare(Engine::Graphics::IGraphicsProvider* graphicsProvider);

    uint32_t GetFaceSize() const { return m_faceSize; }
    uint32_t GetMipLevels() const { return m_mipLevels; }
    uint32_t GetIrradianceFaceSize() const { return m_irradianceFaceSize; }
    const std::vector<uint8_t>& GetSpecularPixels() const
    { return m_specularPixels; }
    const std::vector<uint8_t>& GetIrradiancePixels() const
    { return m_irradiancePixels; }
    const std::array<glm::vec4, 9>& GetRadianceSH() const
    { return m_radianceSH; }
    const Engine::Graphics::IGraphicsTexture* GetSpecularTexture() const
    { return m_specularTexture.get(); }
    const Engine::Graphics::IGraphicsTexture* GetIrradianceTexture() const
    { return m_irradianceTexture.get(); }

private:
    explicit EnvironmentMap(const Engine::Components::Texture& source);
    bool Build(const Engine::Components::Texture& source);

    std::string m_sourcePath;
    uint32_t m_faceSize = 0;
    uint32_t m_mipLevels = 0;
    uint32_t m_irradianceFaceSize = 0;
    std::vector<uint8_t> m_specularPixels;
    std::vector<uint8_t> m_irradiancePixels;
    std::array<glm::vec4, 9> m_radianceSH{};
    Engine::Graphics::IGraphicsProvider* m_preparedProvider = nullptr;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> m_specularTexture;
    std::shared_ptr<Engine::Graphics::IGraphicsTexture> m_irradianceTexture;
};
}
