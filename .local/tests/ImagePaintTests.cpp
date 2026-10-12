#include "Editor/Core/Paint/ImagePaint.h"
#include "Editor/Core/Paint/PngPaintWriter.h"
#include "Editor/Core/Paint/PaintInput.h"
#include "Core/Compoonents/Materials/Texture.h"
#include <cassert>
#include <filesystem>
#include <cstdio>

int main()
{
    namespace fs = std::filesystem;
    using namespace Engine::Editor::Paint;
    penPressure.store(.25f);
    assert(std::abs(PressureScale(0.f) - 1.f) < 1e-6f);
    assert(std::abs(PressureScale(1.f) - .25f) < 1e-6f);
    assert(std::abs(PressureScale(.5f) - .625f) < 1e-6f);
    penPressure.store(1.f);
    std::vector<uint8_t> pixels(4 * 4 * 4, 0);
    const uint8_t red[4]{ 255, 0, 0, 255 };
    assert(Stamp(pixels, 4, 4, 1.5f, 1.5f, 1.f, 1.f, 1.f,
        red, false));
    assert(pixels[(1 * 4 + 1) * 4] == 255);
    const auto once = pixels;
    std::vector<uint8_t> repeated(4 * 4 * 4, 0);
    assert(Stamp(repeated, 4, 4, 1.5f, 1.5f, 1.f, 1.f, 1.f,
        red, false));
    assert(repeated == once);
    std::vector<uint8_t> tip(2 * 2 * 4, 255);
    tip[3] = 0;
    std::vector<uint8_t> tipped(4 * 4 * 4, 0);
    assert(Stamp(tipped, 4, 4, 1.5f, 1.5f, 2.f, 1.f, 1.f,
        red, false, 0, &tip, 2, 2));
    assert(tipped[3] == 0);
    assert(tipped[(1 * 4 + 1) * 4 + 3] > 0);
    std::vector<uint8_t> grayscaleTip(2 * 2 * 4, 255);
    grayscaleTip[4] = grayscaleTip[5] = grayscaleTip[6] = 0;
    std::vector<uint8_t> grayscalePaint(4 * 4 * 4, 0);
    assert(Stamp(grayscalePaint, 4, 4, 1.5f, 1.5f, 2.f, 1.f, 1.f,
        red, false, 0, &grayscaleTip, 2, 2, true));
    assert(grayscalePaint[3] == 0);
    assert(grayscalePaint[(0 * 4 + 2) * 4 + 3] > 0);
    std::vector<uint8_t> alphaLocked(4 * 4 * 4, 0);
    assert(!Stamp(alphaLocked, 4, 4, 1.5f, 1.5f, 1.f, 1.f,
        1.f, red, true, 0, nullptr, 0, 0, false, true));
    alphaLocked[(1 * 4 + 1) * 4 + 3] = 180;
    const uint8_t green[4]{ 0, 255, 0, 255 };
    assert(Stamp(alphaLocked, 4, 4, 1.5f, 1.5f, 1.f, 1.f,
        1.f, green, false, 0, nullptr, 0, 0, false, true));
    assert(alphaLocked[(1 * 4 + 1) * 4 + 3] == 180);
    assert(Stamp(pixels, 4, 4, 1.5f, 1.5f, 1.f, 1.f, 1.f,
        red, true));
    assert(pixels[(1 * 4 + 1) * 4 + 3] == 0);
    const uint8_t blue[4]{ 0, 0, 255, 255 };
    assert(Fill(pixels, 4, 4, 0, 0, 0.f, blue));
    assert(pixels[2] == 255);
    assert(pixels[(1 * 4 + 1) * 4 + 2] == 0);
    uint8_t average[4]{};
    AverageColor(pixels, 4, 4, 1.5f, 1.5f, 2.f, average);
    assert(average[2] > 0 && average[2] < 255);
    const auto beforeBlur = pixels;
    assert(BlurStamp(pixels, 4, 4, 1.5f, 1.5f, 2.f, 1.f));
    std::vector<uint8_t> repeatedBlur = beforeBlur;
    assert(BlurStamp(repeatedBlur, 4, 4, 1.5f, 1.5f, 2.f, 1.f));
    assert(repeatedBlur == pixels);

    const fs::path path = fs::temp_directory_path() /
        "engine-image-paint-regression.png";
    std::string error;
    const bool saved = SavePng(path, 4, 4, pixels, error);
    if (!saved) std::fprintf(stderr, "%s\n", error.c_str());
    assert(saved);
    Engine::Components::Texture::Invalidate(path.string());
    auto loaded = Engine::Components::Texture::Acquire(path.string(), false);
    assert(loaded->Load());
    assert(loaded->GetWidth() == 4 && loaded->GetHeight() == 4);
    assert(std::equal(pixels.begin(), pixels.end(),
        loaded->GetPixels().begin()));
    fs::remove(path);
}
