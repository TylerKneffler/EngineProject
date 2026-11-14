// Terrain editing tool regressions: brush falloff, flatten, layer erase,
// seam-consistent flatten across manager cells, and object scatter / erase
// through the scene view's SceneTerrainTools.
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Terrain/Terrain.h"
#include "Core/Compoonents/Terrain/TerrainManager.h"
#include "Core/Compoonents/Terrain/TerrainObjectLayer.h"
#include "Core/Compoonents/Terrain/TerrainSurfaceSample.h"
#include "Editor/Core/View/Views/Scene/SceneTerrainTools.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
int g_failures = 0;
void RequireAt(bool condition, const char* text, int line)
{
    if (condition) return;
    std::cerr << "TerrainEditingTools failed at line " << line << ": " << text << '\n';
    ++g_failures;
}
#define Require(condition) RequireAt((condition), #condition, __LINE__)

bool Near(float a, float b, float tolerance = 1e-4f) { return std::abs(a - b) <= tolerance; }

using Engine::Components::Terrain;
using Engine::Components::TerrainManager;
using Engine::Components::TerrainObjectLayer;

Terrain* MakeTerrain(Engine::Scene::Scene& scene, int layers)
{
    auto* object = scene.AddObject("Terrain");
    auto* terrain = object->AddComponent<Terrain>();
    terrain->cellsX = terrain->cellsY = 16;
    terrain->cellWidth = terrain->cellDepth = 1.f;
    terrain->heights.assign(17 * 17, 0.f);
    terrain->layers.assign(layers, Terrain::Layer{});
    terrain->weights.assign(terrain->heights.size() * layers, 0.f);
    for (size_t i = 0; i < terrain->heights.size(); ++i) terrain->weights[i * layers] = 1.f;
    terrain->Rebuild();
    return terrain;
}

void TestFalloff()
{
    Require(Near(Terrain::BrushFalloff(0.f, 4.f, .5f), 1.f));
    Require(Near(Terrain::BrushFalloff(2.f, 4.f, .5f), 1.f));
    Require(Near(Terrain::BrushFalloff(3.f, 4.f, .5f), .5f));   // smoothstep midpoint
    Require(Near(Terrain::BrushFalloff(4.f, 4.f, .5f), 0.f));
    Require(Near(Terrain::BrushFalloff(2.f, 4.f, -1.f), .5f));  // legacy cone
    Require(Terrain::BrushFalloff(3.5f, 4.f, 0.f) > 0.f &&
        Terrain::BrushFalloff(3.5f, 4.f, 0.f) < Terrain::BrushFalloff(1.f, 4.f, 0.f));
}

void TestFlatten()
{
    Engine::Scene::Scene scene;
    auto* terrain = MakeTerrain(scene, 1);
    Require(terrain->ApplyBrush(8.f, 8.f, 5.f, 3.f, Terrain::Brush::Raise));
    Require(terrain->heights[8 * 17 + 8] > .99f); // one dab raises at most 1 m
    Terrain::BrushOptions options;
    options.hardness = 1.f;
    options.flattenHeight = .5f;
    Require(terrain->ApplyBrush(8.f, 8.f, 6.f, 1.f, Terrain::Brush::Flatten, 0, false, options));
    for (int row = 4; row <= 12; ++row)
        for (int col = 4; col <= 12; ++col)
            Require(Near(terrain->heights[row * 17 + col], .5f));
    // Outside the radius nothing moved.
    Require(Near(terrain->heights[0], 0.f));
}

void TestPaintErase()
{
    Engine::Scene::Scene scene;
    auto* terrain = MakeTerrain(scene, 3);
    const size_t center = 8 * 17 + 8;
    Terrain::BrushOptions options;
    options.hardness = 1.f;
    Require(terrain->ApplyBrush(8.f, 8.f, 2.f, 1.f, Terrain::Brush::Paint, 2, false, options));
    Require(Near(terrain->weights[center * 3 + 2], 1.f));
    // Partial layer 1, then erase layer 2: its weight goes to layer 1 (the
    // only other layer with weight), and the sample stays normalized.
    options.erase = false;
    Require(terrain->ApplyBrush(8.f, 8.f, 2.f, .5f, Terrain::Brush::Paint, 1, false, options));
    const float layer1 = terrain->weights[center * 3 + 1];
    options.erase = true;
    Require(terrain->ApplyBrush(8.f, 8.f, 2.f, 1.f, Terrain::Brush::Paint, 2, false, options));
    Require(Near(terrain->weights[center * 3 + 2], 0.f));
    Require(terrain->weights[center * 3 + 1] > layer1);
    float sum = 0.f;
    for (int l = 0; l < 3; ++l) sum += terrain->weights[center * 3 + l];
    Require(Near(sum, 1.f));
    // A layer erased where nothing else is painted falls back to another
    // layer instead of leaving the sample empty.
    Require(terrain->ApplyBrush(2.f, 2.f, 1.5f, 1.f, Terrain::Brush::Paint, 0, false, options));
    Require(terrain->weights[(2 * 17 + 2) * 3 + 1] > .99f);
}

void TestManagerFlattenSeam()
{
    Engine::Scene::Scene scene;
    auto* object = scene.AddObject("Manager");
    auto* manager = object->AddComponent<TerrainManager>();
    manager->cellsX = manager->cellsY = 8;
    manager->cellWidth = manager->cellDepth = 1.f;
    manager->loadedRadius = 1;
    manager->farRadius = 0;
    manager->generateColliders = false;
    Require(manager->AddCell(0, 0));
    Require(manager->AddCell(1, 0));
    Require(manager->ApplyBrush(8.0, 4.0, 4.f, 2.f, Terrain::Brush::Raise));
    Terrain::BrushOptions options;
    options.hardness = .3f;
    options.flattenHeight = 1.f;
    Require(manager->ApplyBrush(8.0, 4.0, 3.f, .6f, Terrain::Brush::Flatten, 0, false, options));
    auto* left = manager->GetLoadedCell(0, 0);
    auto* right = manager->GetLoadedCell(1, 0);
    Require(left && right);
    if (!left || !right) return;
    for (int row = 0; row <= 8; ++row)
        Require(Near(left->heights[row * 9 + 8], right->heights[row * 9 + 0]));
}

void TestScatterAndErase()
{
    Engine::Scene::Scene scene;
    auto* terrain = MakeTerrain(scene, 1);
    Require(terrain->ApplyBrush(8.f, 8.f, 6.f, 2.f, Terrain::Brush::Raise));
    // A ground-conforming layer (generated grass) must survive a no-prefab erase.
    auto* grassObject = scene.AddObject("Grass");
    scene.MoveObject(grassObject, terrain->Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
    auto* grass = grassObject->AddComponent<TerrainObjectLayer>();
    grass->layerName = "Meadow grass";
    grass->conformToGround = true;
    grass->placements.push_back({0, "Assets/grass.prefab", {8.f, 0.f, 8.f}});

    Engine::Editor::SceneTerrainTools::Target target;
    target.terrain = terrain;
    target.root = terrain->Owner;
    Engine::Editor::SceneTerrainTools::ScatterSettings settings;
    settings.prefab = "Assets/rock.prefab";
    settings.count = 12;
    settings.minScale = .8f;
    settings.maxScale = 1.2f;
    uint32_t random = 12345u;
    Require(Engine::Editor::SceneTerrainTools::Scatter(scene, target, {8.0, 0.0, 8.0}, 3.f,
        settings, random) == 12);
    TerrainObjectLayer* objects = nullptr;
    for (auto* child : terrain->Owner->Children)
        if (auto* layer = child->GetComponent<TerrainObjectLayer>(); layer && layer != grass)
            objects = layer;
    Require(objects && objects->placements.size() == 12);
    if (!objects) return;
    for (const auto& placement : objects->placements)
    {
        const float dx = placement.position.x - 8.f, dz = placement.position.z - 8.f;
        Require(dx * dx + dz * dz <= 9.0001f);
        float height = 0.f;
        Engine::Components::SampleTerrainSurface(terrain->heights, 16, 16, 1.f, 1.f,
            placement.position.x, placement.position.z, height);
        Require(Near(placement.position.y, height, 1e-3f));
        Require(placement.scale >= .8f && placement.scale <= 1.2f);
        Require(placement.yawDegrees >= -180.f && placement.yawDegrees <= 180.f);
    }
    // A single-object dab lands exactly at the brush center.
    settings.count = 1;
    Require(Engine::Editor::SceneTerrainTools::Scatter(scene, target, {4.0, 0.0, 4.0}, 3.f,
        settings, random) == 1);
    Require(Near(objects->placements.back().position.x, 4.f) &&
        Near(objects->placements.back().position.z, 4.f));
    // Prefab-filtered erase removes the rocks under the brush only.
    Require(Engine::Editor::SceneTerrainTools::Erase(target, {8.0, 0.0, 8.0}, 3.5f,
        "Assets/other.prefab") == 0);
    Require(Engine::Editor::SceneTerrainTools::Erase(target, {8.0, 0.0, 8.0}, 3.5f,
        settings.prefab) == 12);
    Require(objects->placements.size() == 1);
    // Without a prefab: the remaining rock goes, grass stays.
    Require(Engine::Editor::SceneTerrainTools::Erase(target, {6.0, 0.0, 6.0}, 6.f, "") == 1);
    Require(grass->placements.size() == 1);
}
}

void TestPaintMap()
{
    Engine::Scene::Scene scene;
    auto* terrain = MakeTerrain(scene, 2);
    terrain->SetPaintResolution(4);
    Require(terrain->HasPaintMap());
    Require(terrain->PaintWidth() == 65 && terrain->PaintHeight() == 65);
    // Seeded from the vertex weights: everything layer 0.
    Require(Near(terrain->paintWeights[(32 * 65 + 32) * 2], 1.f));
    // A dab between vertices (radius 0.4 m on a 1 m grid) paints map samples
    // but cannot reach a vertex: paint is finer than the mesh.
    Terrain::BrushOptions options;
    options.hardness = 1.f;
    Require(terrain->ApplyBrush(8.5f, 8.5f, .4f, 1.f, Terrain::Brush::Paint, 1, false, options));
    Require(Near(terrain->paintWeights[(34 * 65 + 34) * 2 + 1], 1.f)); // sample at (8.5, 8.5)
    for (size_t vertex = 0; vertex < terrain->heights.size(); ++vertex)
        Require(Near(terrain->weights[vertex * 2], 1.f));
    // A dab on a vertex updates that vertex from the map.
    Require(terrain->ApplyBrush(4.f, 4.f, .3f, 1.f, Terrain::Brush::Paint, 1, false, options));
    Require(Near(terrain->weights[(4 * 17 + 4) * 2 + 1], 1.f));

    // Serialization round trip at 8 bits.
    const auto saved = terrain->Serialize();
    Require(saved.Has("paintMap") && saved["paintResolution"].AsInt() == 4);
    const size_t encoded = saved["paintMap"].AsString().size();
    Require(encoded > 0 && encoded < 4096); // mostly flat weights compress well
    auto* copyObject = scene.AddObject("Copy");
    auto* copy = copyObject->AddComponent<Terrain>();
    copy->Deserialize(saved);
    Require(copy->HasPaintMap() && copy->paintWeights.size() == terrain->paintWeights.size());
    for (size_t i = 0; i < copy->paintWeights.size(); ++i)
        Require(std::abs(copy->paintWeights[i] - terrain->paintWeights[i]) <= 1.f / 255.f + 1e-4f);
    Require(Near(copy->weights[(4 * 17 + 4) * 2 + 1], terrain->weights[(4 * 17 + 4) * 2 + 1], 1.f / 255.f));

    // Resolution change resamples; turning it off keeps the vertex weights.
    copy->SetPaintResolution(8);
    Require(copy->PaintWidth() == 129);
    Require(copy->paintWeights[(68 * 129 + 68) * 2 + 1] > .9f); // (8.5, 8.5)
    copy->SetPaintResolution(0);
    Require(!copy->HasPaintMap() && !copy->Serialize().Has("paintMap"));
    Require(copy->weights[(4 * 17 + 4) * 2 + 1] > .99f);
}

void TestManagerPaintSeam()
{
    Engine::Scene::Scene scene;
    auto* object = scene.AddObject("Manager");
    auto* manager = object->AddComponent<TerrainManager>();
    manager->cellsX = manager->cellsY = 8;
    manager->cellWidth = manager->cellDepth = 1.f;
    manager->loadedRadius = 1;
    manager->farRadius = 0;
    manager->generateColliders = false;
    manager->paintResolution = 4;
    Require(manager->AddCell(0, 0));
    Require(manager->AddCell(1, 0));
    for (auto cell : {std::pair{0, 0}, std::pair{1, 0}})
        if (auto* terrain = manager->GetLoadedCell(cell.first, cell.second))
        {
            terrain->layers.assign(2, Terrain::Layer{});
            terrain->weights.assign(terrain->heights.size() * 2, 0.f);
            for (size_t i = 0; i < terrain->heights.size(); ++i) terrain->weights[i * 2] = 1.f;
            terrain->paintWeights.clear();
            terrain->Rebuild();
        }
    Terrain::BrushOptions options;
    options.hardness = .2f;
    Require(manager->ApplyBrush(8.0, 4.3, 1.3f, .6f, Terrain::Brush::Paint, 1, false, options));
    auto* left = manager->GetLoadedCell(0, 0);
    auto* right = manager->GetLoadedCell(1, 0);
    Require(left && right && left->paintResolution == 4 && right->paintResolution == 4);
    if (!left || !right || !left->HasPaintMap() || !right->HasPaintMap()) return;
    bool painted = false;
    for (int row = 0; row < 33; ++row)
    {
        const float a = left->paintWeights[(size_t(row) * 33 + 32) * 2 + 1];
        const float b = right->paintWeights[(size_t(row) * 33 + 0) * 2 + 1];
        Require(Near(a, b));
        painted |= a > .1f;
    }
    Require(painted);
}

// Stroke cost on a large cell (printed; asserted loosely). One dab is what
// the editor applies per frame while the button is held.
void TimeLargeCellStroke()
{
    Engine::Scene::Scene scene;
    auto* object = scene.AddObject("Large terrain");
    auto* terrain = object->AddComponent<Terrain>();
    terrain->layers.assign(4, Terrain::Layer{});
    terrain->Resize(256, 256, 1.f, 1.f);
    Terrain::BrushOptions options;
    options.hardness = .5f;
    const auto time = [&](Terrain::Brush brush) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i)
            terrain->ApplyBrush(128.f + i * .5f, 128.f, 4.f, .05f, brush, 1, false, options);
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count() / 20.0;
    };
    const double raise = time(Terrain::Brush::Raise);
    const double paint = time(Terrain::Brush::Paint);
    Require(raise < 10.0 && paint < 10.0);
    // Strokes rebuild only the rows they touch; the result must equal a
    // full rebuild of the same data.
    terrain->ApplyBrush(40.f, 200.f, 6.f, .3f, Terrain::Brush::Blend, 0, false, options);
    terrain->ApplyBrush(60.f, 30.f, 3.f, .5f, Terrain::Brush::Paint, 2, false, options);
    auto* mesh = object->GetComponent<Engine::Components::Mesh>();
    const auto incremental = mesh->GetVertices();
    terrain->Rebuild();
    const auto& full = mesh->GetVertices();
    Require(incremental.size() == full.size());
    // Equal up to float rounding: a full rebuild renormalizes every sample
    // again, which may move already-normalized weights by an ulp.
    bool identical = incremental.size() == full.size();
    for (size_t i = 0; identical && i < full.size(); ++i)
    {
        const auto* a = reinterpret_cast<const float*>(&incremental[i]);
        const auto* b = reinterpret_cast<const float*>(&full[i]);
        for (size_t f = 0; identical && f < sizeof(full[i]) / sizeof(float); ++f)
            identical = std::abs(a[f] - b[f]) <= 1e-5f;
    }
    Require(identical);
    std::cout << "256x256 cell, radius 4 m dab: raise " << raise << " ms, paint " << paint << " ms\n";
}

int main()
{
    try
    {
        TestFalloff();
        TestFlatten();
        TestPaintErase();
        TestManagerFlattenSeam();
        TestScatterAndErase();
        TestPaintMap();
        TestManagerPaintSeam();
        TimeLargeCellStroke();
    }
    catch (const std::exception& error)
    {
        std::cerr << "TerrainEditingTools exception: " << error.what() << '\n';
        return 1;
    }
    if (g_failures) return 1;
    std::cout << "TerrainEditingTools passed\n";
    return 0;
}
