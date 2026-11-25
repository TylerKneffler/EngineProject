// TerrainField (todo.md "Terrain: volume-first conversion", step 1): ground
// queries over seed heights plus volume edits.
#include "Core/Components/Terrain/TerrainField.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include "Core/Components/Terrain/TerrainVolumeBuild.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <thread>
#include <vector>

using Engine::Components::SampleTerrainSurface;
using Engine::Components::TerrainField;
using Engine::Components::TerrainFieldLayout;
using Engine::Components::TerrainVolumeEdit;
using Engine::Components::TerrainVolumeInput;

namespace
{
void Require(bool value, const char* what)
{
    if (value) return;
    std::fprintf(stderr, "TerrainFieldTests failed: %s\n", what);
    std::abort();
}

// 8 x 8 samples of 4 m: 32 m cells.
const TerrainFieldLayout Layout{8, 8, 4.f, 4.f};

float WorldHeight(float x, float z)
{
    return 10.f * std::sin(x * .05f) + 5.f * std::cos(z * .07f) + 2.f * std::sin((x + z) * .13f);
}

bool Seed(TerrainField::Cell cell, std::vector<float>& heights)
{
    // A finite world: cells past +-4 cannot be given.
    if (std::abs(cell.first) > 4 || std::abs(cell.second) > 4) return false;
    heights.resize(size_t(Layout.cellsX + 1) * (Layout.cellsY + 1));
    for (int row = 0; row <= Layout.cellsY; ++row)
        for (int col = 0; col <= Layout.cellsX; ++col)
            heights[size_t(row) * (Layout.cellsX + 1) + col] = WorldHeight(
                cell.first * Layout.SpanX() + col * Layout.cellWidth,
                cell.second * Layout.SpanZ() + row * Layout.cellDepth);
    return true;
}

float GridHeight(float x, float z, glm::vec3* normal = nullptr)
{
    const TerrainField::Cell cell{int(std::floor(x / Layout.SpanX())), int(std::floor(z / Layout.SpanZ()))};
    std::vector<float> heights;
    Seed(cell, heights);
    float h = 0.f;
    SampleTerrainSurface(heights, Layout.cellsX, Layout.cellsY, Layout.cellWidth, Layout.cellDepth,
        x - cell.first * Layout.SpanX(), z - cell.second * Layout.SpanZ(), h, normal);
    return h;
}

struct Edits
{
    std::map<TerrainField::Cell, std::vector<TerrainVolumeEdit>> cells;
    TerrainField::EditSource Source()
    {
        return [this](TerrainField::Cell cell, std::vector<TerrainVolumeEdit>& edits) {
            const auto found = cells.find(cell);
            if (found == cells.end()) return false;
            edits = found->second;
            return true;
        };
    }
};

TerrainVolumeEdit Sphere(glm::vec3 centre, float radius, bool subtract, float blend = 1.f)
{
    TerrainVolumeEdit edit;
    edit.shape = TerrainVolumeEdit::Shape::Sphere;
    edit.a = centre;
    edit.radius = radius;
    edit.subtract = subtract;
    edit.blend = blend;
    return edit;
}
}

int main()
{
    std::mt19937 random(7);
    std::uniform_real_distribution<float> across(-120.f, 120.f);

    // Plain ground: the field is the height grid, exactly.
    {
        Edits none;
        TerrainField field(Layout, Seed, none.Source());
        for (int i = 0; i < 2000; ++i)
        {
            const float x = across(random), z = across(random);
            glm::vec3 normal;
            const float h = GridHeight(x, z, &normal);
            const auto surface = field.SurfaceAt(x, z);
            Require(surface && std::abs(surface->height - h) < 1e-4f, "plain SurfaceAt is the grid height");
            Require(glm::length(surface->normal - normal) < 1e-4f, "plain SurfaceAt normal is the grid's");
            Require(field.Density({x, h + .5f, z}) < 0.f && field.Density({x, h - .5f, z}) > 0.f,
                "density: air above, ground below");
            Require(!field.SurfaceAt(x, z, h - 1.f), "solid below a plain surface");
            const auto down = field.Raycast({x, 60.f, z}, {0.f, -1.f, 0.f}, 200.f);
            Require(down && std::abs(down->position.y - h) < 2e-3f, "vertical ray hits the grid height");
            // A slanted ray lands on the surface.
            const auto slant = field.Raycast({x, 50.f, z}, {.6f, -1.f, .3f}, 300.f);
            Require(slant && std::abs(slant->position.y - GridHeight(slant->position.x, slant->position.z)) < 5e-3f,
                "slanted ray lands on the surface");
        }
        Require(!field.SurfaceAt(1000.f, 0.f), "no seed: no surface");
        Require(!field.HasEdits({0, 0}), "no edits");
    }

    // A crater carved at the surface of cell (1, 1).
    {
        Edits edits;
        const glm::vec3 centreWorld(48.f, GridHeight(48.f, 48.f), 48.f);
        edits.cells[{1, 1}] = {Sphere(centreWorld - glm::vec3(32.f, 0.f, 32.f), 5.f, true)};
        TerrainField field(Layout, Seed, edits.Source());
        // Mid-cell, 11 m from every border: only its owner.
        Require(field.HasEdits({1, 1}) && !field.HasEdits({0, 1}) && !field.HasEdits({3, 3}),
            "an edit belongs to the cells it reaches");
        const auto inside = field.SurfaceAt(48.f, 48.f);
        Require(inside && inside->height < centreWorld.y - 4.f, "crater floor about a radius down");
        Require(inside->normal.y > .8f, "crater floor faces up");
        Require(field.Density(centreWorld) < 0.f, "crater centre is air");
        const auto outside = field.SurfaceAt(70.f, 70.f);
        Require(outside && std::abs(outside->height - GridHeight(70.f, 70.f)) < 1e-4f, "ground away from it");

        // The field is what BuildTerrainVolume meshes for that cell.
        TerrainVolumeInput input;
        input.cellsX = Layout.cellsX; input.cellsY = Layout.cellsY;
        input.cellWidth = Layout.cellWidth; input.cellDepth = Layout.cellDepth;
        Seed({1, 1}, input.heights);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (dx || dz) Seed({1 + dx, 1 + dz}, input.neighbourHeights[size_t((dz + 1) * 3 + dx + 1)]);
        input.edits = edits.cells[{1, 1}];
        const auto volume = Engine::Components::BuildTerrainVolume(input, 1.f, 8.f);
        Require(volume != nullptr, "volume built");
        int compared = 0;
        float worst = 0.f;
        for (int y = 0; y < volume->sizeY; ++y)
            for (int z = 2; z < volume->sizeZ - 2; ++z)
                for (int x = 2; x < volume->sizeX - 2; ++x)
                {
                    const glm::vec3 local = volume->origin + glm::vec3(x, y, z) * volume->spacing;
                    if (local.x < 0.f || local.z < 0.f || local.x > 32.f || local.z > 32.f) continue;
                    const float meshed = volume->At(x, y, z);
                    if (std::abs(meshed) > 6.f) continue; // clamped band
                    const float exact = field.Density(local + glm::vec3(32.f, 0.f, 32.f));
                    Require((exact >= 0.f) == (meshed >= 0.f) || std::abs(exact) < .02f,
                        "same side of the surface as the meshed volume");
                    // Values where they place the surface. Deeper in, the
                    // build applies an edit to whole chunks its bounds touch
                    // and the field only inside the bounds: the depth reads
                    // differently there, never the side.
                    if (std::abs(meshed) > .25f) continue;
                    worst = std::max(worst, std::abs(exact - meshed));
                    ++compared;
                }
        Require(compared > 100 && worst < .02f, "field matches the meshed density");
        std::fprintf(stderr, "crater: %d samples, worst density difference %.4f m\n", compared, worst);
    }

    // An edit owned by cell (0, 0) reaching across into cell (1, 0).
    {
        Edits edits;
        const float y = GridHeight(30.f, 10.f);
        edits.cells[{0, 0}] = {Sphere({30.f, y, 10.f}, 5.f, true)};
        TerrainField field(Layout, Seed, edits.Source());
        Require(field.Density({33.f, y, 10.f}) < 0.f, "carved across the cell border");
        const auto across = field.SurfaceAt(33.f, 10.f);
        Require(across && across->height < GridHeight(33.f, 10.f) - 2.f, "surface across the border is lowered");
    }

    // An arch: rock added 8 m above the ground.
    {
        Edits edits;
        const float ground = GridHeight(-50.f, -50.f);
        TerrainVolumeEdit capsule;
        capsule.shape = TerrainVolumeEdit::Shape::Capsule;
        capsule.subtract = false;
        capsule.a = {-60.f + 64.f, ground + 8.f, -50.f + 64.f};
        capsule.b = {-40.f + 64.f, ground + 8.f, -50.f + 64.f};
        capsule.radius = 2.f;
        capsule.blend = .5f;
        edits.cells[{-2, -2}] = {capsule};
        TerrainField field(Layout, Seed, edits.Source());
        const auto top = field.SurfaceAt(-50.f, -50.f);
        Require(top && std::abs(top->height - (ground + 10.f)) < .3f, "open sky: the arch's top");
        const auto under = field.SurfaceAt(-50.f, -50.f, ground + 5.f);
        Require(under && std::abs(under->height - ground) < .05f, "under the arch: the ground");
        const auto ray = field.Raycast({-50.f, ground + 3.f, -70.f}, {0.f, 0.f, 1.f}, 40.f);
        Require(!ray || ray->distance > 15.f || std::abs(ray->position.y - (ground + 3.f)) < 1e-3f,
            "a ray under the arch passes below it");
        const auto through = field.Raycast({-50.f, ground + 8.f, -70.f}, {0.f, 0.f, 1.f}, 40.f);
        Require(through && std::abs(through->position.z - (-52.f)) < .3f, "a ray at its height meets its side");

        // Invalidation picks up a removed edit.
        edits.cells.erase({-2, -2});
        field.Invalidate({-2, -2});
        const auto cleared = field.SurfaceAt(-50.f, -50.f);
        Require(cleared && std::abs(cleared->height - ground) < 1e-4f, "invalidate drops the removed arch");
    }

    // Queries from many threads agree with one thread.
    {
        Edits edits;
        edits.cells[{1, 1}] = {Sphere({16.f, GridHeight(48.f, 48.f), 16.f}, 6.f, true)};
        TerrainField field(Layout, Seed, edits.Source(), 32);
        std::vector<glm::vec2> points(4000);
        for (auto& point : points) point = {across(random), across(random)};
        std::vector<float> expected(points.size());
        {
            TerrainField reference(Layout, Seed, edits.Source());
            for (size_t i = 0; i < points.size(); ++i)
                expected[i] = reference.SurfaceAt(points[i].x, points[i].y)->height;
        }
        std::atomic<int> mismatches{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([&, t] {
                for (size_t i = t; i < points.size(); i += 3)
                {
                    const auto surface = field.SurfaceAt(points[i].x, points[i].y);
                    if (!surface || surface->height != expected[i]) ++mismatches;
                }
            });
        for (auto& thread : threads) thread.join();
        Require(mismatches == 0, "threaded queries match (small cache, evicting)");
    }

    std::printf("TerrainFieldTests passed\n");
    return 0;
}
