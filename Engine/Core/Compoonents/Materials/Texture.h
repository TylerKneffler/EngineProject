#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class IGraphicsProvider;
class IGraphicsTexture;

class Texture
{
public:
    static std::shared_ptr<Texture> Acquire(const std::string& path);

    bool Load();
    bool Prepare(IGraphicsProvider* graphicsProvider);

    const std::string& GetFilePath() const { return m_filePath; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    bool HasPixels() const { return !m_pixels.empty(); }
    const IGraphicsTexture* GetGraphicsTexture() const { return m_graphicsTexture.get(); }

private:
    explicit Texture(std::string path) : m_filePath(std::move(path)) {}

    std::string m_filePath;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    std::vector<uint8_t> m_pixels;
    IGraphicsProvider* m_preparedProvider = nullptr;
    std::shared_ptr<IGraphicsTexture> m_graphicsTexture;
};
