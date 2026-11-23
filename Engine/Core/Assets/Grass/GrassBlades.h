#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace Engine::Scene { class Scene; }
namespace Engine::Graphics { class IGraphicsProvider; class IGraphicsBuffer; }
namespace Engine::Rendering { struct GpuBladeField; }

// GPU grass blades for the meadow (.md/todo.md "Next" 2): one scene-wide
// carrier object whose blades the renderer generates on the GPU every frame
// (Rendering/GpuGrassBlades.h, Shaders/GrassBlades/GrassBlades.hlsl). Meadow
// grass fields in blade mode (GrassField::gpuBlades) register their cell's
// height and grass paint grids here and stop building their fine grass on the
// CPU; shrubs, accents and bushes stay mesh placements.
// On by default (the scene script sets gpuBlades); ENGINE_GPU_GRASS=0 keeps
// the mesh grass, as does a renderer that cannot draw blades.
class GrassBlades
{
public:
    struct Settings
    {
        float shortDensity = 300.f;    // blades per m2
        float tallDensity = 350.f;    // blades per m2 inside a tall field
        float fieldSize = 28.f, fieldCoverage = .85f, shortUnderTall = .25f;
        int seed = 18731;
        float keepDistance = 7.f;    // full density inside, (d0 / d)^2 beyond
        float reach = 60.f, fadeBand = 10.f;
        float clumpSize = .8f;
        // Pockets between tussocks (GrassBlades.hlsl header 23-27): tussock
        // spacing, pocket width in tussock sizes from the border, short
        // blades thinned there, colour darkening and short height lost at
        // the border itself.
        float tussockSize = 2.4f, pocketWidth = .3f, pocketThinning = .9f, pocketShade = .7f, pocketLowering = .7f;
        float shortHeight[2] = {.15f, .45f}, tallHeight[2] = {.7f, 1.05f};
        float shortWidth = .010f, tallWidth = .009f;
        float dryShare = .12f;
        float paint[2] = {.35f, .65f};
        float windStrength = .6f;
        int projectLayer = 0;
        uint32_t capacity = 400000;
    };

    static bool Enabled();
    static std::shared_ptr<GrassBlades> For(Engine::Scene::Scene& scene);

    // A meadow cell: world origin of its grid, samples per side, spacing (m),
    // heights relative to origin.y and grass paint weight, row-major (z, x).
    void SetCell(const void* key, glm::vec3 origin, int samples, float spacing,
        std::vector<float> heights, std::vector<float> paint);
    void RemoveCell(const void* key);
    void SetSettings(const Settings& settings);
    // Rebuilds the GPU data when cells changed and creates the carrier once.
    void Update(Engine::Scene::Scene& scene, Engine::Graphics::IGraphicsProvider* provider);

private:
    struct Cell
    {
        glm::vec3 origin{0.f};
        int samples = 0;
        float spacing = 4.f;
        std::vector<float> heights, paint;
    };
    std::map<const void*, Cell> m_cells;
    Settings m_settings;
    bool m_dirty = true, m_hasSettings = false;
    uint64_t m_carrierId = 0;
    std::shared_ptr<Engine::Rendering::GpuBladeField> m_field;
    // Earlier data buffers, kept a few rebuilds for frames still in flight.
    std::deque<std::shared_ptr<Engine::Graphics::IGraphicsBuffer>> m_retired;
};
