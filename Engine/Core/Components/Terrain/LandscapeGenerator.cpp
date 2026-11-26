#include "Core/Components/Terrain/LandscapeGenerator.h"

#include "Core/Components/Terrain/TerrainFoliage.h"
#include "Core/Components/Terrain/TerrainHeightArchive.h"
#include "Core/Components/Terrain/TerrainObjectArchive.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include "Core/Components/Terrain/TerrainVolumeBuild.h"
#include "Core/Scene/Object.h"
#include "Core/Serialization/Json.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace Engine::Components
{
namespace
{
namespace fs = std::filesystem;

// The landscape's layout (TerrainManager on landscape_demo.scene).
constexpr int Cells = 32;            // quads per cell edge
constexpr double Spacing = 4.0;      // metres per quad
constexpr int Coarse = 4;            // stored sample stride outside the play area
constexpr double Macro = Spacing * Coarse;
constexpr double CellSpan = Cells * Spacing;
constexpr float Scale = 16.f;        // stored units per metre
constexpr double MaxHeight = 2040.0;
constexpr uint64_t ArchiveMagic = 0x31544748524E5254ull;

// Features placed in metres around the start (build_landscape_demo.py).
constexpr double StartX = 0.0, StartZ = 40.0;
struct Lake { double x, z, radius; };
constexpr std::array<Lake, 5> Lakes{{{180.0, 560.0, 430.0}, {-1500.0, -700.0, 560.0},
    {1350.0, -1300.0, 520.0}, {1500.0, 900.0, 380.0}, {-700.0, 1650.0, 300.0}}};
constexpr double HillHeight = 150.0, HillClear0 = 260.0, HillClear1 = 620.0,
    HillReach0 = 1900.0, HillReach1 = 2900.0;
constexpr double EscarpCentreX = -150.0, EscarpCentreZ = -660.0, EscarpFacingX = 0.0, EscarpFacingZ = 1.0,
    EscarpHeight = 80.0, EscarpHalfLength = 430.0, EscarpBack = 650.0;

double SmoothStep(double a, double b, double x)
{
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// --- noise (build_landscape_demo.py, the same values) -----------------------

double Lattice(int64_t ix, int64_t iz, int64_t seed)
{
    const uint64_t mask = 0xFFFFFFFFull;
    uint64_t h = ((uint64_t(ix) * 0x9E3779B1ull) ^ (uint64_t(iz) * 0x85EBCA77ull) ^
        (uint64_t(seed * 0xC2B2AE3Dll) & mask)) & mask;
    h ^= h >> 15;
    h = (h * 0x2C1B3C6Dull) & mask;
    h ^= h >> 12;
    h = (h * 0x297A2D39ull) & mask;
    h ^= h >> 15;
    return double(h & 0xFFFFFFull) / double(0xFFFFFF);
}

double GradientNoise(double x, double z, double period, int64_t seed)
{
    x /= period; z /= period;
    const double fx = std::floor(x), fz = std::floor(z);
    const int64_t ix = int64_t(fx) + 100000, iz = int64_t(fz) + 100000;
    const double tx = x - fx, tz = z - fz;
    const auto corner = [&](int dx, int dz) {
        const double angle = Lattice(ix + dx, iz + dz, seed) * 6.283185307179586;
        return std::cos(angle) * (tx - dx) + std::sin(angle) * (tz - dz);
    };
    const double ux = tx * tx * tx * (tx * (tx * 6 - 15) + 10);
    const double uz = tz * tz * tz * (tz * (tz * 6 - 15) + 10);
    const double bottom = corner(0, 0) * (1 - ux) + corner(1, 0) * ux;
    const double top = corner(0, 1) * (1 - ux) + corner(1, 1) * ux;
    return bottom * (1 - uz) + top * uz;
}

void OctaveFrame(double x, double z, int octave, int64_t seed, double& ox, double& oz)
{
    const double angle = 0.61 + octave * 1.13 + double(seed) * 0.37;
    const double c = std::cos(angle), s = std::sin(angle);
    const int64_t m = ((seed * 7 + octave * 13) % 17 + 17) % 17;
    const double shift = 1000.0 * double(m);
    ox = c * x - s * z + shift;
    oz = s * x + c * z - shift;
}

struct Noise
{
    int64_t variant = 0;   // added to every seed (0: the demo map)
    int64_t S(int64_t seed) const { return seed + variant * 7919; }

    double Fbm(double x, double z, double period, int64_t seed, int octaves = 4) const
    {
        seed = S(seed);
        double total = 0.0, amplitude = 1.0, norm = 0.0;
        for (int octave = 0; octave < octaves; ++octave)
        {
            double ox, oz;
            OctaveFrame(x, z, octave, seed, ox, oz);
            total += amplitude * GradientNoise(ox, oz, period / std::pow(2.0, octave), seed + octave * 17);
            norm += amplitude;
            amplitude *= 0.5;
        }
        return std::clamp(0.5 + total / norm, 0.0, 1.0);
    }

    double Ridged(double x, double z, double period, int64_t seed, int octaves = 5) const
    {
        seed = S(seed);
        double total = 0.0, amplitude = 1.0, norm = 0.0, weight = 1.0;
        for (int octave = 0; octave < octaves; ++octave)
        {
            double ox, oz;
            OctaveFrame(x, z, octave, seed, ox, oz);
            const double n = GradientNoise(ox, oz, period / std::pow(2.0, octave), seed + octave * 31);
            const double r = std::pow(std::clamp(1.0 - std::abs(n) * 1.5, 0.0, 1.0), 2.0);
            total += amplitude * r * weight;
            weight = std::clamp(r * 1.8, 0.0, 1.0);
            norm += amplitude;
            amplitude *= 0.5;
        }
        return total / norm;
    }
};

double Bump(double x, double z, double cx, double cz, double radius)
{
    return 1.0 - SmoothStep(0.15, 1.0, std::hypot(x - cx, z - cz) / radius);
}

// --- the map ------------------------------------------------------------------

struct Shapes
{
    const LandscapeGeneratorSettings& settings;
    Noise noise;

    double MacroHeight(double x, double z) const
    {
        const double wx = x + (noise.Fbm(x, z, 9000.0, 7, 2) - 0.5) * 2400.0;
        const double wz = z + (noise.Fbm(x, z, 9000.0, 19, 2) - 0.5) * 2400.0;
        const double distance = std::hypot(x - StartX, (z - StartZ) * 0.85);
        const double rim = 2700.0 + (noise.Fbm(x, z, 2600.0, 41, 3) - 0.5) * 2400.0;
        const double openLand = SmoothStep(rim - 700.0, rim + 1800.0, distance);
        const double ranges = SmoothStep(0.40, 0.66, noise.Fbm(wx, wz, 9000.0, 101, 3));
        const double uplift = noise.Fbm(wx, wz, 14000.0, 113, 2);
        const double crests = noise.Ridged(wx, wz, 4200.0, 211, 5);
        const double peaks = 150.0 + 2100.0 * ranges * (0.6 + 0.4 * uplift);
        const double mountains = openLand * peaks * std::pow(crests, 1.6);
        const double foothills = SmoothStep(rim - 1600.0, rim, distance) * (0.35 + 0.65 * ranges) *
            180.0 * noise.Fbm(x, z, 1100.0, 307, 4);
        const double rolling = (noise.Fbm(x, z, 1400.0, 3, 4) - 0.5) * 2.0 * 26.0;
        return -22.0 + rolling + settings.relief * (foothills + mountains);
    }

    double LowlandHills(double x, double z) const
    {
        const double distance = std::hypot(x - StartX, z - StartZ);
        double region = (1.0 - SmoothStep(HillReach0, HillReach1, distance)) *
            SmoothStep(HillClear0, HillClear1, distance);
        if (region <= 0.0) return 0.0;
        for (const auto& lake : Lakes)
            region *= SmoothStep(lake.radius * 1.05, lake.radius * 1.05 + 450.0,
                std::hypot(x - lake.x, z - lake.z));
        if (region <= 0.0) return 0.0;
        const double shape = SmoothStep(0.40, 0.72, noise.Fbm(x, z, 1500.0, 601, 2));
        const double relief = 0.75 + 0.5 * noise.Fbm(x, z, 420.0, 613, 3);
        return HillHeight * region * shape * relief;
    }

    double Escarpment(double x, double z) const
    {
        const double u = -(x - EscarpCentreX) * EscarpFacingZ + (z - EscarpCentreZ) * EscarpFacingX;
        const double v = -((x - EscarpCentreX) * EscarpFacingX + (z - EscarpCentreZ) * EscarpFacingZ);
        if (!(v > -160.0 && v < EscarpBack && std::abs(u) < EscarpHalfLength)) return 0.0;
        const double ends = 1.0 - SmoothStep(0.55 * EscarpHalfLength, EscarpHalfLength, std::abs(u));
        const double back = 1.0 - SmoothStep(120.0, EscarpBack, v);
        const double line = v + 45.0 * std::sin(u / 240.0) + (noise.Fbm(x, z, 150.0, 631, 3) - 0.5) * 2.0 * 24.0;
        const double halfWidth = 13.0 + 34.0 * SmoothStep(0.5, 0.8, noise.Fbm(x, z, 210.0, 641, 2));
        const double face = SmoothStep(-halfWidth, halfWidth, line);
        const double rim = 0.82 + 0.36 * noise.Fbm(x, z, 380.0, 651, 2);
        const double talus = 12.0 * std::pow(1.0 - SmoothStep(0.0, 70.0, -line), 2.0) * (1.0 - face);
        return ends * (EscarpHeight * rim * back * face + talus);
    }

    // The lowland around the start with its hills and escarpment, the lake
    // basins and a dry start pad.
    double Carve(double height, double x, double z) const
    {
        const double water = settings.waterLevel;
        const double lowland = Bump(x, z, StartX, StartZ, 900.0);
        if (lowland > 0.0)
        {
            const double rolling = (noise.Fbm(x, z, 900.0, 3) - 0.5) * 2.0 * 24.0;
            const double hills = (noise.Fbm(x, z, 330.0, 53, 3) - 0.5) * 2.0 * 13.0;
            const double detail = (noise.Fbm(x, z, 60.0, 307, 3) - 0.5) * 3.0;
            const double valley = -27.0 + rolling * 0.4 + hills * 0.6 + detail;
            height = height * (1.0 - lowland) + valley * lowland;
        }
        if (settings.hills) height += LowlandHills(x, z);
        if (settings.escarpment) height += Escarpment(x, z);
        if (settings.lakes)
            for (size_t index = 0; index < Lakes.size(); ++index)
            {
                const auto& lake = Lakes[index];
                if (std::hypot(x - lake.x, z - lake.z) > lake.radius * 2.2) continue;
                const double wx = x + (noise.Fbm(x, z, 420.0, 401 + int64_t(index) * 7, 3) - 0.5) * 2.0 * lake.radius * 0.55;
                const double wz = z + (noise.Fbm(x, z, 420.0, 433 + int64_t(index) * 7, 3) - 0.5) * 2.0 * lake.radius * 0.55;
                const double d = std::hypot(wx - lake.x, (wz - lake.z) * 1.25) / lake.radius;
                const double basin = 1.0 - SmoothStep(0.45, 1.0, d);
                const double floor = water - 3.0 - 13.0 * (1.0 - SmoothStep(0.0, 0.7, d));
                height = height * (1.0 - basin) + std::min(height, floor) * basin;
            }
        const double pad = Bump(x, z, StartX, StartZ, 45.0);
        return height * (1.0 - pad) + std::max(height, water + 7.0) * pad;
    }
};

template <typename Function>
void ParallelRows(int rows, const Function& function)
{
    const unsigned workers = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<int> next{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < workers; ++t)
        threads.emplace_back([&] { for (int row; (row = next++) < rows;) function(row); });
    for (auto& thread : threads) thread.join();
}

// A square grid, row-major (row = z).
struct Grid
{
    int side = 0;
    std::vector<double> values;
    double& At(int row, int col) { return values[size_t(row) * side + col]; }
    double At(int row, int col) const { return values[size_t(row) * side + col]; }
};

// Gradient as numpy.gradient: central inside, one-sided at the borders.
void Gradient(const Grid& grid, double spacing, int row, int col, double& gx, double& gz)
{
    const int last = grid.side - 1;
    const auto d = [&](int r0, int c0, int r1, int c1, double steps) {
        return (grid.At(r1, c1) - grid.At(r0, c0)) / (steps * spacing);
    };
    gx = col == 0 ? d(row, 0, row, 1, 1) : col == last ? d(row, last - 1, row, last, 1)
        : d(row, col - 1, row, col + 1, 2);
    gz = row == 0 ? d(0, col, 1, col, 1) : row == last ? d(last - 1, col, last, col, 1)
        : d(row - 1, col, row + 1, col, 2);
}

// Particle hydraulic erosion (after Beyer 2015), in units of unit * spacing
// metres as the Python tool: droplets run downhill, erode through a radial
// brush under capacity and deposit bilinearly over it. Droplets run one after
// another from a fixed-seed generator.
void Erode(Grid& grid, double spacing, double dropletsPerTexel, const std::atomic<bool>* cancel,
    const std::function<void(double)>& progress)
{
    constexpr int MaxSteps = 100, Radius = 2;
    constexpr double Inertia = 0.1, Capacity = 4.0, MinCapacity = 0.01, ErodeSpeed = 0.3,
        DepositSpeed = 0.3, Evaporate = 0.02, Gravity = 4.0, Unit = 60.0;
    const int side = grid.side;
    const double scale = Unit * spacing;
    std::vector<double> h(grid.values.size());
    for (size_t i = 0; i < h.size(); ++i) h[i] = grid.values[i] / scale;
    std::vector<std::pair<int, double>> brush;
    double total = 0.0;
    for (int oy = -Radius; oy <= Radius; ++oy)
        for (int ox = -Radius; ox <= Radius; ++ox)
        {
            const double d = std::hypot(ox, oy);
            if (d < Radius) { brush.emplace_back(oy * side + ox, Radius - d); total += Radius - d; }
        }
    for (auto& [offset, weight] : brush) weight /= total;
    const double margin = Radius + 1;
    std::mt19937_64 random(9);
    const auto uniform = [&](double low, double high) {
        return low + (high - low) * double(random() >> 11) * (1.0 / 9007199254740992.0);
    };
    const auto sample = [&](double px, double py, double& gx, double& gy, size_t& index, double& fx, double& fy) {
        const int ix = int(px), iy = int(py);
        fx = px - ix; fy = py - iy;
        index = size_t(iy) * side + ix;
        const double h00 = h[index], h10 = h[index + 1], h01 = h[index + side], h11 = h[index + side + 1];
        gx = (h10 - h00) * (1 - fy) + (h11 - h01) * fy;
        gy = (h01 - h00) * (1 - fx) + (h11 - h10) * fx;
        return h00 * (1 - fx) * (1 - fy) + h10 * fx * (1 - fy) + h01 * (1 - fx) * fy + h11 * fx * fy;
    };
    const size_t droplets = size_t(double(h.size()) * dropletsPerTexel);
    for (size_t droplet = 0; droplet < droplets; ++droplet)
    {
        if ((droplet & 0x3fff) == 0)
        {
            if (cancel && *cancel) return;
            progress(double(droplet) / double(droplets));
        }
        double px = uniform(margin, side - 1 - margin), py = uniform(margin, side - 1 - margin);
        double dx = 0, dy = 0, speed = 1, water = 1, sediment = 0;
        for (int step = 0; step < MaxSteps; ++step)
        {
            double gx, gy, fx, fy;
            size_t index;
            const double value = sample(px, py, gx, gy, index, fx, fy);
            dx = dx * Inertia - gx * (1 - Inertia);
            dy = dy * Inertia - gy * (1 - Inertia);
            double length = std::hypot(dx, dy);
            const bool moving = length > 1e-12;
            if (!moving) length = 1.0;
            dx /= length; dy /= length;
            const double nx = px + dx, ny = py + dy;
            const bool inside = moving && nx >= margin && nx < side - 1 - margin && ny >= margin &&
                ny < side - 1 - margin;
            double ngx, ngy, nfx, nfy;
            size_t nindex;
            const double deltaH = inside ? sample(nx, ny, ngx, ngy, nindex, nfx, nfy) - value : 0.0;
            const double cap = std::max(-deltaH * speed * water * Capacity, MinCapacity);
            const bool depositing = sediment > cap || deltaH > 0;
            double deposit = deltaH > 0 ? std::min(deltaH, sediment) : (sediment - cap) * DepositSpeed;
            deposit = depositing && inside ? deposit : 0.0;
            const double take = !depositing && inside ? std::min((cap - sediment) * ErodeSpeed, -deltaH) : 0.0;
            if (!inside) deposit = sediment;
            sediment = sediment - deposit + take;
            if (deposit != 0.0)
            {
                h[index] += deposit * (1 - fx) * (1 - fy);
                h[index + 1] += deposit * fx * (1 - fy);
                h[index + side] += deposit * (1 - fx) * fy;
                h[index + side + 1] += deposit * fx * fy;
            }
            if (take > 0.0)
                for (const auto& [offset, weight] : brush) h[size_t(int64_t(index) + offset)] -= take * weight;
            speed = std::sqrt(std::max(speed * speed - deltaH * Gravity, 0.0));
            water *= 1.0 - Evaporate;
            if (!inside) break;
            px = nx; py = ny;
        }
    }
    for (size_t i = 0; i < h.size(); ++i) grid.values[i] = h[i] * scale;
}

int64_t FloorDiv(int64_t a, int64_t b) { const int64_t q = a / b; return q - ((a % b != 0) && ((a < 0) != (b < 0))); }

int64_t Quantize(double height)
{
    return std::clamp<int64_t>(int64_t(std::nearbyint(height * Scale)), -32768, 32767);
}

// Two-bit rock class from slope (build_landscape_demo.py rock_class).
std::vector<uint8_t> RockClass(const std::vector<int64_t>& q, int side, double spacing, double water)
{
    Grid grid{side, std::vector<double>(q.size())};
    for (size_t i = 0; i < q.size(); ++i) grid.values[i] = double(q[i]) / Scale;
    std::vector<uint8_t> classes(q.size(), 0);
    ParallelRows(side, [&](int row) {
        for (int col = 0; col < side; ++col)
        {
            double gx, gz;
            Gradient(grid, spacing, row, col, gx, gz);
            const double slope = 1.0 - 1.0 / std::sqrt(1.0 + gx * gx + gz * gz);
            uint8_t c = slope > 0.26 ? 3 : slope > 0.19 ? 2 : slope > 0.13 ? 1 : 0;
            if (grid.At(row, col) < water + 1.0) c = 0;
            classes[size_t(row) * side + col] = c;
        }
    });
    return classes;
}

template <typename T> void Put(std::string& out, const T& value)
{
    out.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

double CatmullRom(double p0, double p1, double p2, double p3, double t)
{
    return 0.5 * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t +
        (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
}

bool WriteFile(const std::string& path, const std::string& bytes)
{
    std::error_code error;
    fs::create_directories(fs::path(path).parent_path(), error);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), std::streamsize(bytes.size()));
    return bool(file);
}

bool Replace(const std::string& from, const std::string& to)
{
    std::error_code error;
    fs::rename(from, to, error);
    if (!error) return true;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
    if (error) return false;
    fs::remove(from, error);
    return true;
}
}

bool GenerateLandscape(const LandscapeGeneratorSettings& settings, const LandscapeGeneratorPaths& paths,
    std::string& error, const LandscapeGeneratorProgress& progress, const std::atomic<bool>* cancel,
    LandscapeGeneratorReport* report)
{
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    LandscapeGeneratorReport result;
    const auto stage = [&](float fraction, const char* name) { if (progress) progress(fraction, name); };
    const auto cancelled = [&] { return cancel && *cancel; };
    const int extent = std::clamp(settings.halfExtentCells, 1, 1000);
    const int fineExtent = std::clamp(settings.fineHalfExtentCells, 0, extent);
    if (paths.heights.empty()) { error = "No height archive path."; return false; }
    const Shapes shapes{settings, Noise{settings.seed}};

    // Macro field at 16 m over the whole map.
    stage(0.f, "Ranges");
    const double origin = -extent * CellSpan;
    Grid macro{(2 * extent + 1) * Cells / Coarse + 1, {}};
    macro.values.resize(size_t(macro.side) * macro.side);
    ParallelRows(macro.side, [&](int row) {
        const double z = origin + row * Macro;
        for (int col = 0; col < macro.side; ++col) macro.At(row, col) = shapes.MacroHeight(origin + col * Macro, z);
    });
    if (cancelled()) { error = "Cancelled."; return false; }

    // Droplet erosion.
    stage(.25f, "Erosion");
    Grid eroded = macro;
    if (settings.erosion > 0.f)
        Erode(eroded, Macro, settings.erosion, cancel, [&](double f) { stage(.25f + .35f * float(f), "Erosion"); });
    if (cancelled()) { error = "Cancelled."; return false; }

    // Lowland, hills, escarpment, lakes on the eroded field.
    stage(.6f, "Valleys and lakes");
    std::vector<int64_t> macroQ(macro.values.size());
    ParallelRows(macro.side, [&](int row) {
        const double z = origin + row * Macro;
        for (int col = 0; col < macro.side; ++col)
            macroQ[size_t(row) * macro.side + col] = Quantize(std::clamp(
                shapes.Carve(eroded.At(row, col), origin + col * Macro, z), -200.0, MaxHeight));
    });

    // The play area at 4 m: the eroded field resampled (before the lakes,
    // carved again at full resolution) plus detail, blended into the coarse
    // reconstruction over its outer two cells so borders match.
    stage(.7f, "Play area detail");
    const int fineSide = (2 * fineExtent + 1) * Cells + 1;
    const int first = (extent - fineExtent) * Cells / Coarse;
    const double fineOrigin = -fineExtent * CellSpan;
    Grid base{fineSide, std::vector<double>(size_t(fineSide) * fineSide)};
    ParallelRows(fineSide, [&](int row) {
        const double gr = double(row) / Coarse;
        const int r = int(gr);
        const double tr = gr - r;
        for (int col = 0; col < fineSide; ++col)
        {
            const double gc = double(col) / Coarse;
            const int c = int(gc);
            const double tc = gc - c;
            double rows[4];
            for (int k = 0; k < 4; ++k)
            {
                const int rr = std::clamp(first + r - 1 + k, 0, eroded.side - 1);
                const auto at = [&](int cc) { return eroded.At(rr, std::clamp(first + cc, 0, eroded.side - 1)); };
                rows[k] = CatmullRom(at(c - 1), at(c), at(c + 1), at(c + 2), tc);
            }
            base.At(row, col) = CatmullRom(rows[0], rows[1], rows[2], rows[3], tr);
        }
    });
    std::vector<int64_t> fineQ(base.values.size());
    const int coarseSpan = (2 * fineExtent + 1) * Cells / Coarse;
    ParallelRows(fineSide, [&](int row) {
        const double z = fineOrigin + row * Spacing;
        const int r0 = std::min(row / Coarse, coarseSpan - 1);
        const int fy = row - r0 * Coarse;
        const double edgeZ = std::min(row, fineSide - 1 - row);
        for (int col = 0; col < fineSide; ++col)
        {
            const double x = fineOrigin + col * Spacing;
            double gx, gz;
            Gradient(base, Spacing, row, col, gx, gz);
            const double steep = SmoothStep(0.25, 0.9, std::sqrt(gx * gx + gz * gz));
            const double spurs = std::pow(1.0 - std::abs(2.0 * shapes.noise.Fbm(x, z, 170.0, 223, 3) - 1.0), 1.5) - 0.35;
            const double bumps = (shapes.noise.Fbm(x, z, 60.0, 311, 3) - 0.5) * 3.0;
            const double fine = std::clamp(shapes.Carve(base.At(row, col) + bumps + steep * spurs * 18.0, x, z),
                -200.0, MaxHeight);
            // TerrainHeightArchive::Upsample of the coarse samples, exactly.
            const int c0 = std::min(col / Coarse, coarseSpan - 1);
            const int fx = col - c0 * Coarse;
            const auto q = [&](int dr, int dc) {
                return macroQ[size_t(first + r0 + dr) * macro.side + size_t(first + c0 + dc)];
            };
            const int64_t total = q(0, 0) * (Coarse - fx) * (Coarse - fy) + q(0, 1) * fx * (Coarse - fy) +
                q(1, 0) * (Coarse - fx) * fy + q(1, 1) * fx * fy;
            const int64_t area = Coarse * Coarse;
            const int64_t recon = FloorDiv(2 * total + area, 2 * area);
            const double edge = std::min<double>(edgeZ, std::min(col, fineSide - 1 - col));
            const double mask = SmoothStep(0.0, 2.0 * Cells, edge);
            fineQ[size_t(row) * fineSide + col] = std::clamp<int64_t>(
                recon + int64_t(std::nearbyint((fine * Scale - double(recon)) * mask)), -32768, 32767);
        }
    });
    if (cancelled()) { error = "Cancelled."; return false; }

    // The archive: cells in file order (z, then x), each its samples then
    // its two-bit rock classes.
    stage(.85f, "Writing");
    const auto macroCodes = RockClass(macroQ, macro.side, Macro, settings.waterLevel);
    const auto fineCodes = RockClass(fineQ, fineSide, Spacing, settings.waterLevel);
    const int stride = Cells / Coarse;
    std::string body;
    std::vector<std::pair<size_t, size_t>> blocks;   // offset, size per cell
    float lowest = 1e9f, highest = -1e9f;
    for (int cz = -extent; cz <= extent; ++cz)
        for (int cx = -extent; cx <= extent; ++cx)
        {
            const bool fineCell = std::max(std::abs(cx), std::abs(cz)) <= fineExtent;
            const int n = fineCell ? Cells + 1 : stride + 1;
            const auto& q = fineCell ? fineQ : macroQ;
            const auto& codes = fineCell ? fineCodes : macroCodes;
            const int gridSide = fineCell ? fineSide : macro.side;
            const int r0 = fineCell ? (cz + fineExtent) * Cells : (cz + extent) * stride;
            const int c0 = fineCell ? (cx + fineExtent) * Cells : (cx + extent) * stride;
            const size_t start = body.size();
            for (int r = 0; r < n; ++r)
                for (int c = 0; c < n; ++c)
                {
                    const auto value = int16_t(q[size_t(r0 + r) * gridSide + size_t(c0 + c)]);
                    Put(body, value);
                    lowest = std::min(lowest, value / Scale); highest = std::max(highest, value / Scale);
                }
            std::vector<uint8_t> packed((size_t(n) * n + 3) / 4, 0);
            for (int i = 0; i < n * n; ++i)
                packed[size_t(i) / 4] |= uint8_t(codes[size_t(r0 + i / n) * gridSide + size_t(c0 + i % n)] << ((i % 4) * 2));
            body.append(reinterpret_cast<const char*>(packed.data()), packed.size());
            blocks.emplace_back(start, body.size() - start);
        }
    uint64_t signature = 1469598103934665603ull;
    for (unsigned char byte : body) signature = (signature ^ byte) * 1099511628211ull;
    signature = signature ? signature : 1;

    std::string archive;
    Put(archive, ArchiveMagic); Put(archive, uint32_t(3)); Put(archive, signature);
    Put(archive, int32_t(extent)); Put(archive, int32_t(Cells)); Put(archive, int32_t(Cells));
    Put(archive, Scale); Put(archive, int32_t(fineExtent)); Put(archive, int32_t(Coarse));
    const size_t headerSize = archive.size();
    archive += body;
    const std::string heightsNew = paths.heights + ".new";
    if (!WriteFile(heightsNew, archive))
    { error = "Could not write " + heightsNew; return false; }

    // The old archive, for the cells that changed and to re-seat what stood
    // on them.
    int oldExtent = -1;
    uint64_t oldSignature = 0;
    std::string oldBytes;
    {
        std::ifstream file(paths.heights, std::ios::binary);
        oldBytes.assign(std::istreambuf_iterator<char>(file), {});
        if (oldBytes.size() >= headerSize && oldBytes.compare(0, 8, archive, 0, 8) == 0)
        {
            std::memcpy(&oldSignature, oldBytes.data() + 12, 8);
            std::memcpy(&oldExtent, oldBytes.data() + 20, 4);
        }
    }
    const bool sameLayout = oldExtent == extent && oldBytes.size() == archive.size() &&
        oldBytes.compare(20, headerSize - 20, archive, 20, headerSize - 20) == 0;
    std::set<std::pair<int, int>> changed;
    {
        size_t index = 0;
        for (int cz = -extent; cz <= extent; ++cz)
            for (int cx = -extent; cx <= extent; ++cx, ++index)
            {
                const auto [offset, size] = blocks[index];
                if (!sameLayout || oldBytes.compare(headerSize + offset, size, body, offset, size) != 0)
                    changed.insert({cx, cz});
            }
    }
    oldBytes.clear();
    oldBytes.shrink_to_fit();
    result.changedCells = changed.size();
    if (cancelled()) { fs::remove(heightsNew); error = "Cancelled."; return false; }

    // Ground change per cell: new heights minus old (none where the old
    // archive cannot say).
    const bool haveOld = oldExtent >= 0 && oldSignature != 0;
    std::map<std::pair<int, int>, std::pair<std::vector<float>, std::vector<float>>> cellHeights;
    const auto heightsOf = [&](std::pair<int, int> cell) -> const std::pair<std::vector<float>, std::vector<float>>* {
        auto found = cellHeights.find(cell);
        if (found == cellHeights.end())
        {
            std::pair<std::vector<float>, std::vector<float>> entry;
            std::vector<uint8_t> codes;
            const bool old = haveOld && std::max(std::abs(cell.first), std::abs(cell.second)) <= oldExtent &&
                TerrainHeightArchive::ReadCellCodes(paths.heights, oldSignature, oldExtent, Cells, Cells,
                    cell.first, cell.second, entry.first, codes);
            const bool fresh = std::max(std::abs(cell.first), std::abs(cell.second)) <= extent &&
                TerrainHeightArchive::ReadCellCodes(heightsNew, signature, extent, Cells, Cells,
                    cell.first, cell.second, entry.second, codes);
            if (!old || !fresh) entry = {};
            found = cellHeights.emplace(cell, std::move(entry)).first;
        }
        return found->second.first.empty() ? nullptr : &found->second;
    };
    // Metres the ground rose at a point of a cell (its own space).
    const auto rise = [&](std::pair<int, int> cell, float x, float z) -> float {
        if (!changed.count(cell)) return 0.f;
        const auto* h = heightsOf(cell);
        if (!h) return 0.f;
        x = std::clamp(x, 0.f, float(CellSpan)); z = std::clamp(z, 0.f, float(CellSpan));
        float before = 0.f, after = 0.f;
        SampleTerrainSurface(h->first, Cells, Cells, float(Spacing), float(Spacing), x, z, before);
        SampleTerrainSurface(h->second, Cells, Cells, float(Spacing), float(Spacing), x, z, after);
        return after - before;
    };
    const auto cellOfName = [](const fs::path& file, std::pair<int, int>& cell) {
        return std::sscanf(file.stem().string().c_str(), "%d_%d", &cell.first, &cell.second) == 2;
    };

    // The object archive: every placement moves with the ground under it.
    stage(.9f, "Re-seating objects");
    std::string objectsNew;
    if (!paths.objectArchive.empty() && fs::exists(paths.objectArchive))
        if (const auto objects = TerrainObjectArchive::Open(paths.objectArchive))
        {
            objectsNew = paths.objectArchive + ".new";
            std::atomic<size_t> moved{0};
            std::mutex heightsMutex;
            const bool written = TerrainObjectArchive::Write(objectsNew, objects->GetHeader(), objects->Templates(),
                [&](TerrainObjectArchive::Cell cell) {
                    std::vector<TerrainObjectArchive::Layer> layers;
                    objects->ReadCell(cell, layers);
                    if (!changed.count(cell)) return layers;
                    std::lock_guard lock(heightsMutex);
                    for (auto& layer : layers)
                        for (auto& placement : layer.placements)
                        {
                            const float dy = rise(cell, placement.position.x, placement.position.z);
                            placement.position.y += dy;
                            moved += dy != 0.f ? 1 : 0;
                        }
                    return layers;
                });
            if (!written) { fs::remove(heightsNew); error = "Could not write " + objectsNew; return false; }
            result.placementsMoved = moved;
        }

    // Each cell's files: edits, objects, volume edits, foliage.
    std::error_code fsError;
    if (!paths.cellData.empty() && fs::is_directory(paths.cellData, fsError))
        for (const auto& entry : fs::directory_iterator(paths.cellData, fsError))
        {
            std::pair<int, int> cell;
            if (!entry.is_regular_file() || !cellOfName(entry.path(), cell) || !changed.count(cell)) continue;
            const auto extension = entry.path().extension().string();
            const std::string file = entry.path().string();
            if (extension == ".terrainedit")
            {
                // Sculpts store absolute heights per vertex: each keeps its
                // offset from the ground (the vertex grids are the same).
                // Paint weights stay. Unreadable edits are left alone.
                TerrainHeightArchive::EditEntries edits;
                const auto* h = heightsOf(cell);
                if (!h || !TerrainHeightArchive::ReadEditEntries(file, Cells, Cells, edits)) continue;
                for (auto& [index, height] : edits.heights)
                    if (index < h->first.size()) height += h->second[index] - h->first[index];
                std::string bytes;
                Put(bytes, uint64_t(0x31544944454e5254ull)); Put(bytes, uint32_t(3));
                Put(bytes, uint32_t(Cells)); Put(bytes, uint32_t(Cells)); Put(bytes, edits.layers);
                Put(bytes, uint32_t(edits.heights.size())); Put(bytes, uint32_t(edits.weights.size()));
                for (const auto& [index, height] : edits.heights) { Put(bytes, index); Put(bytes, height); }
                for (const auto& [index, weights] : edits.weights)
                {
                    Put(bytes, index);
                    for (float weight : weights) Put(bytes, weight);
                }
                if (WriteFile(file + ".tmp", bytes) && Replace(file + ".tmp", file)) ++result.sculptsMoved;
            }
            else if (extension == ".volumeedit")
            {
                std::vector<TerrainVolumeEdit> edits;
                if (!LoadTerrainVolumeEdits(file, edits) || edits.empty()) continue;
                for (auto& edit : edits)
                {
                    const float dy = rise(cell, edit.a.x, edit.a.z);
                    edit.a.y += dy;
                    // A capsule's far end moves with the ground under it; a
                    // box's b is its half extents.
                    if (edit.shape == TerrainVolumeEdit::Shape::Capsule) edit.b.y += rise(cell, edit.b.x, edit.b.z);
                }
                if (SaveTerrainVolumeEdits(file, edits)) ++result.volumeEditsMoved;
            }
            else if (extension == ".objects")
            {
                try
                {
                    auto document = Engine::Serialization::AssetParseFile(file);
                    auto& layers = document["layers"];
                    Engine::Serialization::JsonValue moved = Engine::Serialization::JsonValue::MakeArray();
                    for (size_t i = 0; i < layers.ArraySize(); ++i)
                    {
                        Engine::Core::Object holder;
                        auto* layer = holder.AddComponent<TerrainObjectLayer>();
                        layer->Deserialize(layers.ArrayAt(i));
                        for (auto& placement : layer->placements)
                            placement.position.y += rise(cell, placement.position.x, placement.position.z);
                        moved.Push(layer->Serialize());
                    }
                    // The same revision: the file is the same edit, re-seated.
                    document.Set("layers", std::move(moved));
                    if (WriteFile(file + ".tmp", Engine::Serialization::AssetWriteBinary(document)) &&
                        Replace(file + ".tmp", file))
                        ++result.objectFilesMoved;
                }
                catch (...) {}
            }
            else if (extension == ".foliage")
            {
                TerrainFoliageCell foliage, moved;
                if (!TerrainFoliageCell::Load(file, foliage) || foliage.Empty()) continue;
                for (const auto& [name, layer] : foliage.layers)
                    for (const auto& [key, chunk] : layer.chunks)
                        for (int i = 0; i < TerrainFoliageCell::ChunkVoxels; ++i)
                        {
                            if (chunk[size_t(i)] == 128) continue;   // offset 0: nothing painted
                            const glm::ivec3 voxel(std::get<0>(key) * TerrainFoliageCell::ChunkSide + i % TerrainFoliageCell::ChunkSide,
                                std::get<1>(key) * TerrainFoliageCell::ChunkSide + (i / TerrainFoliageCell::ChunkSide) % TerrainFoliageCell::ChunkSide,
                                std::get<2>(key) * TerrainFoliageCell::ChunkSide + i / (TerrainFoliageCell::ChunkSide * TerrainFoliageCell::ChunkSide));
                            const glm::vec3 centre = TerrainFoliageCell::VoxelCentre(voxel);
                            const glm::ivec3 to = voxel + glm::ivec3(0, int(std::lround(
                                rise(cell, centre.x, centre.z) / TerrainFoliageCell::VoxelSize)), 0);
                            auto& target = moved.layers[name].chunks[TerrainFoliageCell::ChunkOf(to)];
                            if (target[0] == 0 && std::all_of(target.begin(), target.end(), [](uint8_t v) { return v == 0; }))
                                target.fill(128);
                            target[size_t(TerrainFoliageCell::IndexInChunk(to))] = chunk[size_t(i)];
                        }
                if (TerrainFoliageCell::Save(file, moved)) ++result.foliageCellsMoved;
            }
        }
    if (cancelled()) { fs::remove(heightsNew); if (!objectsNew.empty()) fs::remove(objectsNew);
        error = "Cancelled."; return false; }

    // Swap the files in, and renew the cell data generation so a scene saved
    // before restores none of the old files.
    if (!Replace(heightsNew, paths.heights) ||
        (!objectsNew.empty() && !Replace(objectsNew, paths.objectArchive)))
    { error = "Could not replace the archives (are they open elsewhere?)"; return false; }
    if (!paths.cellData.empty())
    {
        std::random_device device;
        const uint64_t generation = (uint64_t(device()) << 32) ^ device() ^
            uint64_t(Clock::now().time_since_epoch().count());
        WriteFile((fs::path(paths.cellData) / "cell_data.generation").string(), std::to_string(generation | 1));
    }
    result.signature = signature;
    result.lowest = lowest;
    result.highest = highest;
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    stage(1.f, "Done");
    if (report) *report = result;
    return true;
}
}
