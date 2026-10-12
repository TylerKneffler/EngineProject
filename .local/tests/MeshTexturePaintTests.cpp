#include "Editor/Core/Paint/MeshTexturePaint.h"
#include "Editor/Core/Paint/BrushTipMask.h"
#include "Editor/Core/Paint/MeshUvUnwrap.h"
#include "Editor/Core/Paint/PngPaintWriter.h"
#include "Editor/Core/Paint/AbrImporter.h"
#include "Editor/Core/Paint/BrushPreset.h"
#include "Core/Compoonents/Materials/Texture.h"

#include <array>
#include <cassert>
#include <deque>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
struct Vertex
{
    float uv[2]{};
    float uv1[2]{};
};
struct UnwrapVertex
{
    float pos[3]{};
    float uv[2]{};
    float uv1[2]{};
    float color[4]{1.f, .25f, .5f, 1.f};
};
}

int main()
{
    using namespace Engine::Editor::Paint;
    // The front triangle uses UV1 and crosses the repeat boundary at U=1.
    // A second triangle behind it points at a different part of the image.
    std::vector<Vertex> vertices{
        {{.94f, .1f}, {.94f, .1f}},
        {{1.06f, .1f}, {1.06f, .1f}},
        {{.94f, .9f}, {.94f, .9f}},
        {{.4f, .1f}, {.4f, .1f}},
        {{.6f, .1f}, {.6f, .1f}},
        {{.4f, .9f}, {.4f, .9f}}
    };
    std::vector<ProjectedPaintVertex> screen{
        {0.f, 0.f, .1f, 1.f, true},
        {8.f, 0.f, .1f, 1.f, true},
        {0.f, 8.f, .1f, 1.f, true},
        {0.f, 0.f, .9f, 1.f, true},
        {8.f, 0.f, .9f, 1.f, true},
        {0.f, 8.f, .9f, 1.f, true}
    };
    const std::vector<uint32_t> triangles{0, 1, 2, 3, 4, 5};
    std::vector<uint8_t> pixels(8 * 8 * 4, 255);
    const uint8_t red[4]{255, 0, 0, 255};
    assert(StampMeshTexture(pixels, 8, 8, vertices, triangles, screen,
        4.f, 2.f, 3.f, 1.f, 1.f, red, false, 1, 0));
    // Repeating UVs touch both the first and last texture columns.
    bool firstColumn = false, lastColumn = false, middleColumn = false;
    for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 8; ++x)
    {
        const size_t offset = (y * 8 + x) * 4;
        if (pixels[offset + 1] == 0)
        {
            firstColumn |= x == 0;
            lastColumn |= x == 7;
            middleColumn |= x == 4;
        }
    }
    assert(firstColumn && lastColumn && !middleColumn);
    const auto picked = PickMeshTextureTexel(8, 8, vertices, triangles,
        screen, 4.f, 2.f, 1);
    assert(picked && (*picked % 8 == 0 || *picked % 8 == 7));

    const std::vector<uint8_t> before(8 * 8 * 4, 255);
    const auto path = std::filesystem::temp_directory_path() /
        "engine-mesh-texture-paint-test.png";
    std::string error;
    assert(SavePng(path, 8, 8, pixels, error));
    auto loaded = Engine::Components::Texture::Acquire(path.string(), false);
    assert(loaded && loaded->Load());
    assert(std::equal(pixels.begin(), pixels.end(),
        loaded->GetPixels().begin()));
    std::deque<std::vector<uint8_t>> undo, redo;
    auto current = pixels;
    CommitTexturePaintHistory(undo, redo, before, 8);
    assert(StepTexturePaintPixels(current, undo, redo, false));
    assert(current == before && undo.empty() && redo.size() == 1);
    assert(SavePng(path, 8, 8, current, error));
    Engine::Components::Texture::Invalidate(path.string());
    assert(loaded->Load());
    assert(std::equal(before.begin(), before.end(),
        loaded->GetPixels().begin()));
    assert(StepTexturePaintPixels(current, undo, redo, true));
    assert(current == pixels && undo.size() == 1 && redo.empty());
    assert(SavePng(path, 8, 8, current, error));
    Engine::Components::Texture::Invalidate(path.string());
    assert(loaded->Load());
    assert(std::equal(pixels.begin(), pixels.end(),
        loaded->GetPixels().begin()));
    std::filesystem::remove(path);

    std::vector<uint8_t> locked(8 * 8 * 4, 255);
    locked[3] = 0;
    const auto untouched = locked;
    assert(StampMeshTexture(locked, 8, 8, vertices, triangles, screen,
        4.f, 2.f, 3.f, 1.f, 1.f, red, false, 1, 0, nullptr, 0, 0,
        false, true));
    assert(locked[3] == 0);
    for (size_t i = 3; i < locked.size(); i += 4)
        assert(locked[i] == untouched[i]);

    std::vector<UnwrapVertex> corner{
        {{0.f, 0.f, 0.f}}, {{1.f, 0.f, 0.f}},
        {{0.f, 1.f, 0.f}}, {{0.f, 0.f, 1.f}}
    };
    std::vector<uint32_t> faces{0, 1, 2, 0, 3, 1};
    assert(UnwrapMeshUv(corner, faces, 1, 45.f));
    assert(corner.size() == 6); // Shared seam vertices are duplicated.
    assert(faces.size() == 6);
    for (const auto& vertex : corner)
    {
        assert(vertex.uv1[0] >= 0.f && vertex.uv1[0] <= 1.f);
        assert(vertex.uv1[1] >= 0.f && vertex.uv1[1] <= 1.f);
        assert(vertex.color[1] == .25f);
    }
    assert(corner[faces[0]].uv1[0] != corner[faces[3]].uv1[0] ||
        corner[faces[0]].uv1[1] != corner[faces[3]].uv1[1]);

    const std::vector<uint8_t> tip{0, 0, 0, 255,
        255, 255, 255, 255};
    assert(TipUsesLuminance(tip, 2, 1));
    assert(SampleTipMask(tip, 2, 1, -.5f, 0.f, true) == 1.f);
    assert(SampleTipMask(tip, 2, 1, .5f, 0.f, true) == 0.f);

    // Version 1 sampled brush: a 2x2 raw mask and 30 percent spacing.
    std::vector<uint8_t> abr;
    const auto u16 = [&](uint16_t value)
    { abr.push_back(uint8_t(value >> 8)); abr.push_back(uint8_t(value)); };
    const auto u32 = [&](uint32_t value)
    { u16(uint16_t(value >> 16)); u16(uint16_t(value)); };
    u16(1); u16(1); u16(2); u32(38); u32(0); u16(30); abr.push_back(1);
    for (int i = 0; i < 4; ++i) u16(0);
    u32(0); u32(0); u32(2); u32(2);
    u16(8); abr.push_back(0);
    abr.insert(abr.end(), {0, 64, 128, 255});
    std::vector<AbrTip> decoded;
    assert(ReadAbrTips(abr, decoded, error));
    assert(decoded.size() == 1 && decoded[0].width == 2 &&
        decoded[0].height == 2 && decoded[0].spacing == .3f &&
        decoded[0].mask == std::vector<uint8_t>({0, 64, 128, 255}));
    const auto pack = std::filesystem::temp_directory_path() /
        "engine-paint-abr-test.abr";
    const auto assets = std::filesystem::temp_directory_path() /
        "engine-paint-abr-test-assets";
    { std::ofstream output(pack, std::ios::binary);
      output.write(reinterpret_cast<const char*>(abr.data()), abr.size()); }
    assert(ImportAbrPack(pack, assets, error));
    BrushPreset imported;
    assert(LoadBrushPreset(assets / "Brushes" /
        "engine-paint-abr-test Brush 1.brush", imported, error));
    assert(imported.spacing == .3f);
    auto importedTip = Engine::Components::Texture::Acquire(
        imported.tipPath, false);
    assert(importedTip && importedTip->Load());
    assert(importedTip->GetPixels()[3] == 0 &&
        importedTip->GetPixels()[15] == 255);
    // Version 6 sample section with two PackBits compressed rows.
    abr.clear();
    u16(6); u16(1);
    abr.insert(abr.end(), {'8', 'B', 'I', 'M', 's', 'a', 'm', 'p'});
    u32(80); u32(76);
    abr.insert(abr.end(), 47, 0);
    u32(0); u32(0); u32(2); u32(2);
    u16(8); abr.push_back(1);
    u16(3); u16(3);
    abr.insert(abr.end(), {1, 0, 64, 1, 128, 255});
    assert(ReadAbrTips(abr, decoded, error));
    assert(decoded.size() == 1 &&
        decoded[0].mask == std::vector<uint8_t>({0, 64, 128, 255}));
    std::filesystem::remove(pack);
    std::filesystem::remove_all(assets);
}
