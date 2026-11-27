#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Scene/Object.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// Macro color map: a scene script composes one albedo per map sample (16 m on
// the landscape demo) on a worker; this uploads it as an sRGB texture with a
// full mip chain and hands terrain draws its world mapping (texture slot 5,
// ObjectGPUData::terrainMacroMap). Polled from draw preparation, so edits to
// the script's settings rebuild it in the editor as well as in play.
namespace Engine::Components
{
namespace
{
uint8_t EncodeSrgb(float linear)
{
    linear = std::clamp(linear, 0.f, 1.f);
    const float srgb = linear <= .0031308f
        ? linear * 12.92f : 1.055f * std::pow(linear, 1.f / 2.4f) - .055f;
    return static_cast<uint8_t>(std::lround(srgb * 255.f));
}

// How often draw preparation polls the source's revision.
constexpr auto PollInterval = std::chrono::milliseconds(100);
}

TerrainManager::MacroColorImage TerrainManager::EncodeMacroColorImage(int width,
    int height, float originX, float originZ, float spacing,
    const std::vector<glm::vec4>& linear, const std::vector<glm::vec3>* normals)
{
    MacroColorImage image;
    if (width <= 0 || height <= 0 || linear.size() != size_t(width) * height)
        return image;
    image.width = width; image.height = height;
    image.linear = std::make_shared<const std::vector<glm::vec4>>(linear);
    if (normals && normals->size() == linear.size())
        image.normals = std::make_shared<const std::vector<glm::vec3>>(*normals);
    image.originX = originX; image.originZ = originZ; image.spacing = spacing;
    // Box-filtered mips in linear space, so distant hills keep their mean
    // color instead of aliasing between forest and meadow texels.
    std::vector<glm::vec4> level = linear;
    // Normals ride along in a second chain: averaged per mip, then
    // renormalized when encoded, so distant mips shade with the mean slope.
    const bool withNormals = normals && normals->size() == linear.size();
    std::vector<glm::vec3> normalLevel = withNormals ? *normals : std::vector<glm::vec3>{};
    int levelWidth = width, levelHeight = height;
    while (true)
    {
        for (const glm::vec4& value : level)
        {
            image.mips.push_back(EncodeSrgb(value.r));
            image.mips.push_back(EncodeSrgb(value.g));
            image.mips.push_back(EncodeSrgb(value.b));
            image.mips.push_back(static_cast<uint8_t>(
                std::lround(std::clamp(value.a, 0.f, 1.f) * 255.f)));
        }
        for (const glm::vec3& value : normalLevel)
        {
            const float length = glm::length(value);
            const glm::vec3 unit = length > 1e-6f ? value / length : glm::vec3(0.f, 1.f, 0.f);
            for (int axis = 0; axis < 3; ++axis)
                image.normalMips.push_back(static_cast<uint8_t>(
                    std::lround(std::clamp(unit[axis] * .5f + .5f, 0.f, 1.f) * 255.f)));
            // Alpha: the grass share a build carried as length - 1 (G1;
            // TerrainManagerArchiveMap). Unit normals (edited patches, maps
            // without it) read as no grass.
            image.normalMips.push_back(static_cast<uint8_t>(
                std::lround(std::clamp(length - 1.f, 0.f, 1.f) * 255.f)));
        }
        ++image.mipLevels;
        if (levelWidth == 1 && levelHeight == 1) break;
        const int nextWidth = std::max(1, levelWidth / 2);
        const int nextHeight = std::max(1, levelHeight / 2);
        std::vector<glm::vec4> next(size_t(nextWidth) * nextHeight);
        std::vector<glm::vec3> nextNormals(withNormals ? next.size() : 0);
        for (int y = 0; y < nextHeight; ++y)
            for (int x = 0; x < nextWidth; ++x)
            {
                glm::vec4 sum(0.f);
                glm::vec3 normalSum(0.f);
                int count = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const int sx = std::min(x * 2 + dx, levelWidth - 1);
                        const int sy = std::min(y * 2 + dy, levelHeight - 1);
                        sum += level[size_t(sy) * levelWidth + sx];
                        if (withNormals) normalSum += normalLevel[size_t(sy) * levelWidth + sx];
                        ++count;
                    }
                next[size_t(y) * nextWidth + x] = sum / float(count);
                if (withNormals) nextNormals[size_t(y) * nextWidth + x] = normalSum / float(count);
            }
        level = std::move(next);
        normalLevel = std::move(nextNormals);
        levelWidth = nextWidth;
        levelHeight = nextHeight;
    }
    return image;
}

TerrainManager::MacroColorImage TerrainManager::PatchMacroColorImage(
    const MacroColorImage& image, const std::vector<MacroColorPatch>& patches)
{
    if (!image.linear || image.width <= 0 || image.height <= 0 ||
        image.linear->size() != size_t(image.width) * image.height) return {};
    std::vector<glm::vec4> colors = *image.linear;
    std::vector<glm::vec3> normals = image.normals ? *image.normals : std::vector<glm::vec3>{};
    const bool withNormals = normals.size() == colors.size();
    for (const auto& patch : patches)
    {
        if (patch.width <= 0 || patch.height <= 0 ||
            patch.colors.size() != size_t(patch.width) * patch.height) continue;
        const bool patchNormals = withNormals && patch.normals.size() == patch.colors.size();
        for (int row = 0; row < patch.height; ++row)
            for (int col = 0; col < patch.width; ++col)
            {
                const int x = patch.x0 + col, y = patch.y0 + row;
                if (x < 0 || y < 0 || x >= image.width || y >= image.height) continue;
                const size_t target = size_t(y) * image.width + x;
                const size_t source = size_t(row) * patch.width + col;
                colors[target] = patch.colors[source];
                if (patchNormals) normals[target] = patch.normals[source];
            }
    }
    return EncodeMacroColorImage(image.width, image.height, image.originX, image.originZ,
        image.spacing, colors, withNormals ? &normals : nullptr);
}

void TerrainManager::MarkMacroCellsDirty(Cell cell)
{
    if (!m_macroSource.makeCellUpdate) return;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
            m_macroDirtyCells.insert({cell.first + dx, cell.second + dz});
}

void TerrainManager::UpdateFarGridColors(const std::vector<Cell>& cells)
{
    const int side = m_farPalette.gridHalfExtent * 2 + 2;
    if (m_farPalette.gridHalfExtent <= 0 || m_farPalette.gridColors.size() != size_t(side) * side ||
        !m_macroImage.linear || m_macroImage.spacing <= 0.f) return;
    const auto& samples = *m_macroImage.linear;
    const double spanX = double(cellsX) * cellWidth, spanZ = double(cellsY) * cellDepth;
    bool changed = false;
    for (const Cell cell : cells)
        for (int cz = cell.second; cz <= cell.second + 1; ++cz)
            for (int cx = cell.first; cx <= cell.first + 1; ++cx)
            {
                const int gx = cx + m_farPalette.gridHalfExtent, gy = cz + m_farPalette.gridHalfExtent;
                const int sx = int(std::lround((cx * spanX - m_macroImage.originX) / m_macroImage.spacing));
                const int sy = int(std::lround((cz * spanZ - m_macroImage.originZ) / m_macroImage.spacing));
                if (gx < 0 || gy < 0 || gx >= side || gy >= side || sx < 0 || sy < 0 ||
                    sx >= m_macroImage.width || sy >= m_macroImage.height) continue;
                m_farPalette.gridColors[size_t(gy) * side + gx] =
                    glm::vec3(samples[size_t(sy) * m_macroImage.width + sx]);
                changed = true;
            }
    if (!changed) return;
    m_farPaletteShared = std::make_shared<const FarTerrainPalette>(m_farPalette);
    for (const Cell cell : cells) MarkFarVolumeDirty(cell);
}

void TerrainManager::SetMacroColorSource(MacroColorSource source)
{
    if (m_macroJob.valid()) m_macroJob.wait();
    m_macroJob = {};
    m_macroSource = std::move(source);
    m_macroRequested = 0;
    m_macroChecked = {};
    m_macroDirtyCells.clear();
    m_macroJobCells.clear();
    if (!m_macroSource.revision)
    {
        m_macroTexture.reset();
        m_macroImage = {};
    }
}

TerrainManager::MacroColorBinding TerrainManager::GetMacroColorMap(
    IGraphicsProvider* provider)
{
    MacroColorBinding result;
    if (!macroColorMap || !provider || !Owner || !m_macroSource.revision ||
        !m_macroSource.makeBuild)
        return result;
    if (provider != m_macroProvider)
    {
        m_macroTexture.reset();
        m_macroProvider = provider;
        m_macroRequested = 0;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - m_macroChecked >= PollInterval)
    {
        m_macroChecked = now;
        if (m_macroJob.valid() &&
            m_macroJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            MacroColorImage image = m_macroJob.get();
            const bool whole = m_macroJobWhole;
            const auto patched = std::move(m_macroJobCells);
            m_macroJobCells.clear();
            if (image.mipLevels && !image.mips.empty())
                if (auto* factory = provider->GetTextureFactory())
                    if (auto texture = factory->CreateTexture2D(uint32_t(image.width),
                            uint32_t(image.height), image.mips.data(), image.mipLevels,
                            Engine::Graphics::GraphicsTextureFormat::Rgba8, true))
                    {
                        m_macroTexture = std::move(texture);
                        m_macroNormalTexture.reset();
                        if (!image.normalMips.empty())
                            m_macroNormalTexture = factory->CreateTexture2D(
                                uint32_t(image.width), uint32_t(image.height),
                                image.normalMips.data(), image.mipLevels,
                                Engine::Graphics::GraphicsTextureFormat::Rgba8, false);
                        image.mips.clear();
                        image.mips.shrink_to_fit();
                        image.normalMips.clear();
                        image.normalMips.shrink_to_fit();
                        m_macroImage = std::move(image);
                        if (whole)
                        {
                            // Built from the source as generated: the edited
                            // cells come next, from the terrain as it is.
                            for (const Cell cell : m_editedProcedural) MarkMacroCellsDirty(cell);
                            for (const Cell cell : VolumeEditOwners()) MarkMacroCellsDirty(cell);
                            // Saved objects (painted trees) shape the canopy.
                            for (const Cell cell : ObjectFileCells()) MarkMacroCellsDirty(cell);
                            for (const auto& [cell, data] : m_cells) MarkMacroCellsDirty(cell);
                        }
                        else
                            UpdateFarGridColors(patched);
                        // ENGINE_TERRAIN_MACRO_LOG: each build and patch installed.
                        static const bool log = std::getenv("ENGINE_TERRAIN_MACRO_LOG") != nullptr;
                        if (log)
                            std::printf("[TerrainManager] macro color %s installed (%zu cells)\n",
                                whole ? "build" : "patch", whole ? size_t(0) : patched.size());
                    }
        }
        // A new revision while a build runs is picked up when it finishes.
        if (!m_macroJob.valid())
            if (const uint64_t revision = m_macroSource.revision();
                revision && revision != m_macroRequested)
                if (auto build = m_macroSource.makeBuild())
                {
                    m_macroRequested = revision;
                    m_macroJobWhole = true;
                    m_macroJob = std::async(std::launch::async, std::move(build));
                }
        // Edited cells, recomposed and written over the current map (edits
        // during a stroke gather until the running patch is in).
        if (!m_macroJob.valid() && !m_macroDirtyCells.empty() && m_macroSource.makeCellUpdate &&
            m_macroImage.linear)
        {
            std::vector<Cell> cells(m_macroDirtyCells.begin(), m_macroDirtyCells.end());
            m_macroDirtyCells.clear();
            MacroColorImage base = m_macroImage;
            base.mips.clear();
            base.normalMips.clear();
            if (auto update = m_macroSource.makeCellUpdate(cells, base))
            {
                m_macroJobWhole = false;
                m_macroJobCells = std::move(cells);
                m_macroJob = std::async(std::launch::async,
                    [update = std::move(update), base = std::move(base)]
                    { return PatchMacroColorImage(base, update()); });
            }
        }
    }
    if (!m_macroTexture || m_macroImage.width <= 0 || m_macroImage.height <= 0)
        return result;
    // Texel centres sit on the samples, so the texture corner is half a
    // spacing before sample 0.
    const glm::dvec3 origin = Owner->transform.GetWorldPositionD();
    const float spacing = m_macroImage.spacing;
    result.texture = m_macroTexture.get();
    result.normalTexture = m_macroNormalTexture.get();
    result.mapping = glm::vec4(
        float(origin.x) + m_macroImage.originX - .5f * spacing,
        float(origin.z) + m_macroImage.originZ - .5f * spacing,
        1.f / (spacing * float(m_macroImage.width)),
        1.f / (spacing * float(m_macroImage.height)));
    return result;
}
}
