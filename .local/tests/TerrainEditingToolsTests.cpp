// Terrain editing tool regressions: brush falloff, flatten, layer erase,
// seam-consistent flatten across manager cells, and object scatter / erase
// through the scene view's SceneTerrainTools.
#include "Core/Scene/Scene.h"
#include "Core/Scene/Object.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Terrain/Terrain.h"
#include "Core/Components/Terrain/TerrainManager.h"
#include "Core/Components/Terrain/TerrainObjectLayer.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"
#include "Core/Components/Terrain/TerrainHeightArchive.h"
#include "Core/Components/Terrain/TerrainRegionEdit.h"
#include "Core/Components/Terrain/TerrainBrushShape.h"
#include "Core/Brushes/BrushPreset.h"
#include "Core/Brushes/BrushRules.h"
#include "Core/Components/Terrain/TerrainPlacementBrush.h"
#include "Core/Components/Terrain/TerrainObjectArchive.h"
#include "Core/Components/Terrain/TerrainFoliage.h"
#include "Core/Components/Terrain/TerrainFoliageBrush.h"
#include "Grass/GrassField.h"
#include "Editor/Core/View/Views/Scene/SceneTerrainTools.h"

#include <chrono>
#include <thread>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <tuple>
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

// A relief brush: a square pyramid 4 m wide and 2 m tall at radius 2 (the
// shape's half width), seen from above, imprints its heights into the height
// data. (Built directly; brush assets load the same grids from images.)
std::shared_ptr<const Engine::Components::TerrainBrushShape> PyramidShape()
{
    auto shape = std::make_shared<Engine::Components::TerrainBrushShape>();
    const int n = 65;
    shape->resolution = n;
    shape->halfExtent = 2.f;
    shape->heights.resize(size_t(n) * n);
    shape->mask.resize(size_t(n) * n);
    for (int row = 0; row < n; ++row)
        for (int col = 0; col < n; ++col)
        {
            const float u = col * 2.f / (n - 1) - 1.f, v = row * 2.f / (n - 1) - 1.f;
            const float edge = std::max(std::abs(u), std::abs(v));
            shape->heights[size_t(row) * n + col] = 2.f * (1.f - edge);
            shape->mask[size_t(row) * n + col] = std::clamp((1.f - edge) / .05f, 0.f, 1.f);
        }
    shape->maxHeight = 2.f;
    shape->BuildOutline();
    return shape;
}

void TestStampBrush()
{
    const auto shape = PyramidShape();
    Require(shape != nullptr);
    if (!shape) return;
    Require(Near(shape->maxHeight, 2.f, .1f));
    Require(shape->SampleMask({0.f, 0.f}) > .99f);
    Require(shape->SampleMask({.999f, .999f}) < .5f);
    // Shape space and back.
    const glm::vec2 offset = Engine::Components::TerrainBrushShape::FromShape({.5f, -.25f}, 3.f, .7f);
    const glm::vec2 back = Engine::Components::TerrainBrushShape::ToShape(offset.x, offset.y, 3.f, .7f);
    Require(Near(back.x, .5f) && Near(back.y, -.25f));

    // A stamp is a raise brush scaled by its height map: each dab raises the
    // apex by the strength, a point half way down a face by half of it, and
    // nothing outside the outline. Dabs add up, building the shape.
    Engine::Scene::Scene scene;
    auto* terrain = MakeTerrain(scene, 1);
    Terrain::BrushOptions options;
    options.shape = shape;
    const float radius = shape->halfExtent;
    for (int dab = 0; dab < 4; ++dab)
        Require(terrain->ApplyBrush(8.f, 8.f, radius, .5f, Terrain::Brush::Raise, 0, false, options));
    Require(Near(terrain->heights[8 * 17 + 8], 2.f, .05f));
    Require(Near(terrain->heights[8 * 17 + 9], 1.f, .15f));
    Require(Near(terrain->heights[8 * 17 + 11], 0.f));   // outside the pyramid's 2 m footprint
    Require(Near(terrain->heights[0], 0.f));
    // Lower takes the same shape back out.
    for (int dab = 0; dab < 4; ++dab)
        terrain->ApplyBrush(8.f, 8.f, radius, .5f, Terrain::Brush::Lower, 0, false, options);
    Require(Near(terrain->heights[8 * 17 + 8], 0.f, .05f));
    Require(Near(terrain->heights[8 * 17 + 9], 0.f, .05f));

    // Stop at the stamp: held far past it, a stroke ends at the stamp's own
    // surface (base + its heights at this radius), and a second stroke over
    // the first adds nothing.
    {
        auto* grown = MakeTerrain(scene, 1);
        Terrain::BrushOptions stop;
        stop.shape = shape;
        stop.stopAtStamp = true;
        stop.stampBase = 0.f;
        for (int stroke = 0; stroke < 2; ++stroke)
            for (int dab = 0; dab < 40; ++dab)
                grown->ApplyBrush(8.f, 8.f, radius, .5f, Terrain::Brush::Raise, 0, false, stop);
        Require(Near(grown->heights[8 * 17 + 8], 2.f, .1f));   // the apex, not 40 x .5
        Require(Near(grown->heights[8 * 17 + 9], 1.f, .15f));  // half way down a face
        Require(Near(grown->heights[0], 0.f));
        // Twice the radius, twice as tall: the stamp keeps its proportions.
        auto* big = MakeTerrain(scene, 1);
        for (int dab = 0; dab < 40; ++dab)
            big->ApplyBrush(8.f, 8.f, radius * 2.f, .5f, Terrain::Brush::Raise, 0, false, stop);
        Require(Near(big->heights[8 * 17 + 8], 4.f, .2f));
    }

    // Backed: the stamp builds only against terrain behind it. A 5 m wall
    // from x = 10 on; the stamp faces -x (its back toward the wall), its
    // square reaching over the wall's top. Held long enough to overshoot.
    {
        auto* walled = MakeTerrain(scene, 1);
        for (int row = 0; row <= 16; ++row)
            for (int col = 10; col <= 16; ++col) walled->heights[row * 17 + col] = 5.f;
        walled->Rebuild();
        Terrain::BrushOptions backed;
        backed.shape = shape;
        backed.backed = true;
        backed.shapeYaw = -1.5707963f;   // FromShape(facing (0, 1)) = (-1, 0)
        const glm::vec2 face = Engine::Components::TerrainBrushShape::FromShape(shape->facing, 1.f, backed.shapeYaw);
        Require(Near(face.x, -1.f) && Near(face.y, 0.f));
        bool changed = false;
        for (int dab = 0; dab < 40; ++dab)
            changed |= walled->ApplyBrush(8.5f, 8.f, 3.f, .5f, Terrain::Brush::Raise, 0, false, backed);
        Require(changed);
        float highest = 0.f;
        for (float h : walled->heights) highest = std::max(highest, h);
        Require(highest <= 5.f + 1e-4f);                         // never above the wall
        for (int row = 0; row <= 16; ++row)
            for (int col = 10; col <= 16; ++col)
                Require(Near(walled->heights[row * 17 + col], 5.f)); // its top untouched
        Require(walled->heights[8 * 17 + 9] > 4.f);              // rock built at the foot
        // The same stamp on open flat ground has nothing to build against.
        auto* open = MakeTerrain(scene, 1);
        Require(!open->ApplyBrush(8.5f, 8.f, 3.f, .5f, Terrain::Brush::Raise, 0, false, backed));
    }

    // Across a cell border, turned, the shared samples agree.
    auto* object = scene.AddObject("Manager");
    auto* manager = object->AddComponent<TerrainManager>();
    manager->cellsX = manager->cellsY = 8;
    manager->cellWidth = manager->cellDepth = 1.f;
    manager->loadedRadius = 1;
    manager->farRadius = 0;
    manager->generateColliders = false;
    Require(manager->AddCell(0, 0));
    Require(manager->AddCell(1, 0));
    options.shapeYaw = .6f;
    Require(manager->ApplyBrush(8.0, 4.0, radius * 1.5f, 1.f, Terrain::Brush::Raise, 0, false, options));
    auto* left = manager->GetLoadedCell(0, 0);
    auto* right = manager->GetLoadedCell(1, 0);
    Require(left && right);
    if (!left || !right) return;
    Require(left->heights[4 * 9 + 8] > .9f);
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
    std::cout << "256x256 cell, radius 4 m dab: raise " << raise << " ms, paint " << paint << " ms\n";
}

// ---- Region editing (TerrainRegionEdit.h) ----

void TestRegionMasks()
{
    using Engine::Components::TerrainRegionMask;
    const auto circle = TerrainRegionMask::Circle({0.0, 0.0}, 10.f, .5f);
    Require(Near(circle.Weight(0.0, 0.0), 1.f));
    Require(Near(circle.Weight(4.9, 0.0), 1.f));       // inside the hard core
    Require(circle.Weight(7.5, 0.0) > 0.f && circle.Weight(7.5, 0.0) < 1.f);
    Require(Near(circle.Weight(10.5, 0.0), 0.f));
    const auto box = TerrainRegionMask::Box({-5.0, -2.0}, {5.0, 2.0}, false, 0.f);
    Require(Near(box.Weight(4.9, 1.9), 1.f) && Near(box.Weight(5.1, 0.0), 0.f));
    const auto ellipse = TerrainRegionMask::Box({-5.0, -2.0}, {5.0, 2.0}, true, 0.f);
    Require(Near(ellipse.Weight(4.5, 0.0), 1.f) && Near(ellipse.Weight(4.5, 1.9), 0.f));
    // An L-shaped lasso: inside the arms, outside the notch.
    const auto lasso = TerrainRegionMask::Polygon({{0, 0}, {10, 0}, {10, 4}, {4, 4}, {4, 10}, {0, 10}}, 1.f);
    Require(Near(lasso.Weight(2.0, 8.0), 1.f));
    Require(Near(lasso.Weight(8.0, 8.0), 0.f));
    Require(lasso.Weight(9.6, 2.0) > 0.f && lasso.Weight(9.6, 2.0) < 1.f);
    glm::dvec2 low, high;
    Require(lasso.Bounds(low, high) && low == glm::dvec2(0.0) && high == glm::dvec2(10.0));
    Require(TerrainRegionMask{}.Empty());
}

TerrainManager* MakeHillManager(Engine::Scene::Scene& scene)
{
    auto* object = scene.AddObject("Manager");
    auto* manager = object->AddComponent<TerrainManager>();
    manager->cellsX = manager->cellsY = 32;
    manager->cellWidth = manager->cellDepth = 1.f;
    manager->loadedRadius = 1;
    manager->farRadius = 0;
    manager->generateColliders = false;
    Require(manager->AddCell(0, 0));
    Require(manager->AddCell(1, 0));
    // A ridged hill straddling the border between the cells at x = 32.
    for (auto cell : {std::pair{0, 0}, std::pair{1, 0}})
        if (auto* terrain = manager->GetLoadedCell(cell.first, cell.second))
        {
            for (int row = 0; row <= 32; ++row)
                for (int col = 0; col <= 32; ++col)
                {
                    const double x = cell.first * 32.0 + col, z = row;
                    const double d = std::hypot(x - 32.0, z - 16.0);
                    terrain->heights[size_t(row) * 33 + col] = float(
                        12.0 * std::exp(-d * d / 150.0) + 1.5 * std::sin(x * .7) * std::cos(z * .5));
                }
            terrain->Rebuild();
        }
    return manager;
}

void RequireSeam(TerrainManager* manager)
{
    auto* left = manager->GetLoadedCell(0, 0);
    auto* right = manager->GetLoadedCell(1, 0);
    Require(left && right);
    if (!left || !right) return;
    for (int row = 0; row <= 32; ++row)
        Require(left->heights[size_t(row) * 33 + 32] == right->heights[size_t(row) * 33]);
}

void TestRegionErosionSeam()
{
    using Engine::Components::TerrainRegionMask;
    using Engine::Components::TerrainRegionOperation;
    Engine::Scene::Scene scene;
    auto* manager = MakeHillManager(scene);
    const auto before = manager->GetLoadedCell(0, 0)->heights;
    TerrainRegionOperation op;
    op.kind = TerrainRegionOperation::Kind::Hydraulic;
    op.dropletDensity = 4.f;
    op.erosionUnit = 4.f;
    const auto mask = TerrainRegionMask::Box({16.0, 2.0}, {48.0, 30.0}, false, 3.f);
    const auto start = std::chrono::steady_clock::now();
    Require(manager->EditRegion(mask, op));
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "Region hydraulic erosion, 32x28 m, ~3600 droplets: " << ms << " ms\n";
    RequireSeam(manager);
    // Changed inside, untouched far outside the mask (corner sample).
    double change = 0.0;
    const auto& after = manager->GetLoadedCell(0, 0)->heights;
    for (size_t i = 0; i < after.size(); ++i) change += std::abs(after[i] - before[i]);
    Require(change > 1.0);
    Require(after[0] == before[0]);
    // Erosion moves material, so it should not create or destroy much.
    for (auto kind : {TerrainRegionOperation::Kind::Thermal, TerrainRegionOperation::Kind::Smooth,
                      TerrainRegionOperation::Kind::Noise, TerrainRegionOperation::Kind::Terrace})
    {
        TerrainRegionOperation other;
        other.kind = kind;
        other.talusDegrees = 20.f;
        other.iterations = 8;
        Require(manager->EditRegion(TerrainRegionMask::Circle({32.0, 16.0}, 10.f, .4f), other));
        RequireSeam(manager);
    }
    // Flatten with blend 1 and no feather reaches the height exactly.
    TerrainRegionOperation flatten;
    flatten.kind = TerrainRegionOperation::Kind::Flatten;
    flatten.height = 3.f;
    Require(manager->EditRegion(TerrainRegionMask::Box({30.0, 10.0}, {34.0, 14.0}, false, 0.f), flatten));
    Require(Near(manager->GetLoadedCell(0, 0)->heights[12 * 33 + 32], 3.f));
    Require(Near(manager->GetLoadedCell(1, 0)->heights[12 * 33 + 1], 3.f));
    RequireSeam(manager);
}

void TestRegionPaint()
{
    using Engine::Components::TerrainRegionMask;
    Engine::Scene::Scene scene;
    auto* terrain = MakeTerrain(scene, 2);
    const auto lasso = TerrainRegionMask::Polygon({{2, 2}, {10, 2}, {10, 10}, {2, 10}}, 0.f);
    Require(terrain->PaintRegion(lasso, 1.f, 1, false));
    Require(Near(terrain->weights[(6 * 17 + 6) * 2 + 1], 1.f));
    Require(Near(terrain->weights[(14 * 17 + 14) * 2 + 1], 0.f));
    Require(terrain->PaintRegion(lasso, 1.f, 1, true));
    Require(Near(terrain->weights[(6 * 17 + 6) * 2 + 1], 0.f));
}

// ---- Brushes (.brush) on terrain ----

// The format: version 2 carries relief, rules and placement and reads back
// the same; version 1 files with sections are refused.
void TestBrushPresetTerrain()
{
    namespace B = Engine::Brushes;
    const auto path = std::filesystem::temp_directory_path() / "terrain_brush_test.brush";
    B::BrushPreset preset;
    preset.name = "Meadow grass";
    preset.relief.mapPath = "Assets/relief.png";
    preset.relief.heightRatio = .8f;
    preset.relief.backed = true;
    preset.terrain.tool = B::BrushTool::Place;
    preset.terrain.rules.maxHeight = 120.f;
    preset.terrain.rules.maxSlope = 30.f;
    preset.terrain.rules.onLayers = {"Assets/Materials/Terrain/PBR/grass.material", "forest soil"};
    preset.terrain.rules.avoidLayers = {"rock"};
    preset.terrain.placement.density = .4f;
    preset.terrain.placement.minSpacing = 1.5f;
    preset.terrain.placement.assets.push_back({"Assets/a.prefab", 2.f, .8f, 1.2f});
    preset.terrain.placement.assets.push_back({"Assets/b.prefab", 1.f, 1.f, 1.f});
    std::string error;
    Require(B::SaveBrushPreset(path, preset, error));
    B::BrushPreset loaded;
    Require(B::LoadBrushPreset(path, loaded, error));
    Require(loaded.relief.mapPath == "Assets/relief.png" && Near(loaded.relief.heightRatio, .8f) && loaded.relief.backed);
    Require(loaded.terrain.tool == B::BrushTool::Place);
    Require(Near(loaded.terrain.rules.maxHeight, 120.f) && Near(loaded.terrain.rules.maxSlope, 30.f));
    Require(loaded.terrain.rules.minHeight < -1e30f);
    Require(loaded.terrain.rules.onLayers == std::vector<std::string>(
            {"Assets/Materials/Terrain/PBR/grass.material", "forest soil"}) &&
        loaded.terrain.rules.avoidLayers == std::vector<std::string>({"rock"}));
    Require(loaded.terrain.placement.assets.size() == 2 && loaded.terrain.placement.assets[0].asset == "Assets/a.prefab" &&
        Near(loaded.terrain.placement.assets[0].weight, 2.f) && Near(loaded.terrain.placement.assets[0].maxScale, 1.2f));
    Require(Near(loaded.terrain.placement.minSpacing, 1.5f));
    // A plain brush stays version 1, as the painting tools wrote it.
    B::BrushPreset plain;
    Require(B::SaveBrushPreset(path, plain, error));
    {
        std::ifstream file(path);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        Require(text.find(R"(version="1")") != std::string::npos && text.find("<Terrain") == std::string::npos);
    }
    // Version 1 with a section is refused.
    {
        std::ofstream file(path, std::ios::trunc);
        file << R"(<BrushPreset version="1" name="x" tip="" shape="0" falloff="2" size="40" hardness="0.5" )"
                R"(strength="0.35" opacity="1" flow="1" spacing="0.08" rotation="0" scatter="0" )"
                R"(pressureSize="0" pressureStrength="0"><Terrain tool="place"/></BrushPreset>)";
    }
    Require(!B::LoadBrushPreset(path, loaded, error));
    std::filesystem::remove(path);
    // Rules: bands fade, layers gate.
    B::BrushRules rules;
    rules.maxHeight = 10.f; rules.heightFade = 2.f;
    rules.maxSlope = 30.f; rules.slopeFade = 10.f;
    rules.onLayers = {"grass"};
    // Layers named by material, whatever their order on the terrain.
    const auto set = B::ResolveBrushLayers(rules, {"Assets/M/soil.material", "Assets/M/Grass.material"});
    Require(set.on == std::vector<int>({1}) && set.needsOn);
    const float grass[2] = {0.f, 1.f}, soil[2] = {1.f, 0.f};
    Require(Near(B::BrushRulesWeight(rules, 5.f, 10.f, &set, grass, 2), 1.f));
    Require(Near(B::BrushRulesWeight(rules, 11.f, 10.f, &set, grass, 2), .5f));
    Require(Near(B::BrushRulesWeight(rules, 5.f, 35.f, &set, grass, 2), .5f));
    Require(Near(B::BrushRulesWeight(rules, 5.f, 10.f, &set, soil, 2), 0.f));
    // A terrain without the named layer: the brush acts nowhere on it.
    const auto none = B::ResolveBrushLayers(rules, {"Assets/M/sand.material"});
    const float sand[1] = {1.f};
    Require(Near(B::BrushRulesWeight(rules, 5.f, 10.f, &none, sand, 1), 0.f));
    Require(B::RuleNamesMaterial("grass.material", "Assets\\M\\GRASS.material") &&
        !B::RuleNamesMaterial("Assets/Other/grass.material", "Assets/M/grass.material"));
}

// The stamps, converted to brushes (tools/stamp_brush.py): their tip and
// relief load into a shape that raises the terrain like the stamps did.
void TestReliefBrushAsset()
{
    const auto shape = Engine::Components::TerrainBrushShape::Load("Assets/TerrainData/Brushes/RockFace_01.brush");
    Require(shape != nullptr);
    if (!shape) return;
    Require(shape->resolution == 128 && Near(shape->halfExtent, 1.f));
    Require(Near(shape->maxHeight, 1.385f, .01f));   // the brush's height ratio
    Require(shape->SampleMask({0.f, 0.f}) > .5f && shape->SampleMask({.999f, -.999f}) < .5f);
    Require(shape->facing.y > .9f && shape->outline.size() == 64);
    // The same shape again from the cache.
    Require(Engine::Components::TerrainBrushShape::Load("Assets/TerrainData/Brushes/RockFace_01.brush") == shape);
    // A square analytic tip with a hard falloff: full to the corners.
    Engine::Brushes::BrushPreset square;
    square.shape = 1; square.falloff = 0; square.hardness = .9f;
    const auto box = Engine::Components::TerrainBrushShape::FromPreset(square, 32);
    Require(box && box->SampleMask({.8f, .8f}) > .99f && box->SampleMask({.98f, 0.f}) < .01f);
    Engine::Brushes::BrushPreset round;
    Require(!Engine::Components::TerrainBrushShape::FromPreset(round));   // the terrain's own round brush
    // The bundled example brushes load, each for its tool.
    for (const auto& [file, tool] : {std::pair{"Meadow grass (paint).brush", Engine::Brushes::BrushTool::Paint},
             std::pair{"Meadow bushes (place).brush", Engine::Brushes::BrushTool::Place},
             std::pair{"Oak stand (place).brush", Engine::Brushes::BrushTool::Place}})
    {
        Engine::Brushes::BrushPreset example;
        std::string error;
        Require(Engine::Brushes::LoadBrushPreset(
            std::filesystem::path("Engine/Core/Assets/TerrainData/Brushes") / file, example, error));
        Require(example.terrain.tool == tool && example.terrain.rules.Any());
        if (tool == Engine::Brushes::BrushTool::Place)
            for (const auto& asset : example.terrain.placement.assets)
                Require(std::filesystem::exists("Engine/Core/" + asset.asset));
    }
}

// Rules on sculpt and paint strokes: a Raise brush limited to flat ground
// leaves a slope alone; a Paint brush limited to a layer paints only there.
void TestBrushRulesOnStrokes()
{
    Engine::Scene::Scene scene;
    auto* terrain = MakeTerrain(scene, 2);
    terrain->layers[0].material = "Assets/Materials/Terrain/PBR/forest_soil.material";
    terrain->layers[1].material = "Assets/Materials/Terrain/PBR/grass.material";
    // A 45 degree ramp from x = 8 on, flat before it.
    for (int row = 0; row <= 16; ++row)
        for (int col = 0; col <= 16; ++col)
            terrain->heights[size_t(row) * 17 + col] = col > 8 ? float(col - 8) : 0.f;
    terrain->Rebuild();
    Engine::Brushes::BrushRules flat;
    flat.maxSlope = 20.f; flat.slopeFade = 0.f;
    Terrain::BrushOptions options;
    options.rules = &flat;
    options.hardness = 1.f;
    Require(terrain->ApplyBrush(8.f, 8.f, 6.f, 1.f, Terrain::Brush::Raise, 0, false, options));
    Require(Near(terrain->heights[8 * 17 + 4], 1.f, .05f));     // flat: raised
    Require(Near(terrain->heights[8 * 17 + 12], 4.f));          // on the ramp: untouched
    // Paint grass (layer 1) only where there is grass already: a strip at
    // col 2, 60 % grass.
    for (int row = 0; row <= 16; ++row)
    {
        terrain->weights[(size_t(row) * 17 + 2) * 2] = .4f;
        terrain->weights[(size_t(row) * 17 + 2) * 2 + 1] = .6f;
    }
    Engine::Brushes::BrushRules onGrass;
    onGrass.onLayers = {"grass"};
    Terrain::BrushOptions paint;
    paint.rules = &onGrass;
    paint.hardness = 1.f;
    terrain->ApplyBrush(3.f, 8.f, 3.f, .5f, Terrain::Brush::Paint, 1, false, paint);
    Require(Near(terrain->weights[(size_t(8) * 17 + 4) * 2 + 1], 0.f));   // soil beside the strip: unpainted
    Require(terrain->weights[(size_t(8) * 17 + 2) * 2 + 1] > .7f);         // the strip: painted
}

// A Place brush: assets by weight at a density, kept apart, on the ground
// its rules allow, into its own layer; erasing thins them.
void TestPlacementBrush()
{
    namespace B = Engine::Brushes;
    Engine::Scene::Scene scene;
    auto* terrain = MakeTerrain(scene, 1);
    B::BrushPreset brush;
    brush.terrain.tool = B::BrushTool::Place;
    auto& place = brush.terrain.placement;
    place.assets = {{"Assets/tuft_a.prefab", 3.f, 1.f, 1.f}, {"Assets/tuft_b.prefab", 1.f, .5f, .5f}};
    place.density = 1.f;
    place.minSpacing = 1.f;
    Engine::Components::TerrainPlacementDab dab;
    dab.center = {8.0, 0.0, 8.0};
    dab.radius = 5.f;
    dab.hardness = 1.f;
    dab.layer = "Painted tufts";   // the tool's choice, not the brush's
    uint32_t random = 12345u;
    int placed = 0;
    for (int n = 0; n < 6; ++n)
        placed += Engine::Components::ApplyPlacementBrush(scene, nullptr, terrain, brush, dab, random);
    Engine::Components::TerrainObjectLayer* layer = nullptr;
    for (auto* child : terrain->Owner->Children)
        if (auto* found = child->GetComponent<Engine::Components::TerrainObjectLayer>())
            if (found->layerName == "Painted tufts") layer = found;
    Require(layer && placed > 20 && layer->placements.size() == size_t(placed));
    if (!layer) return;
    int heavy = 0;
    for (size_t i = 0; i < layer->placements.size(); ++i)
    {
        const auto& a = layer->placements[i];
        Require(glm::length(glm::vec2(a.position.x - 8.f, a.position.z - 8.f)) <= 5.01f);
        heavy += a.asset == "Assets/tuft_a.prefab";
        if (a.asset == "Assets/tuft_b.prefab") Require(Near(a.scale, .5f));
        for (size_t j = i + 1; j < layer->placements.size(); ++j)
        {
            const auto& b = layer->placements[j];
            Require(glm::length(glm::vec2(a.position.x - b.position.x, a.position.z - b.position.z)) >= 1.f - 1e-4f);
        }
    }
    Require(heavy > placed / 2);   // 3:1 by weight
    // Rules that rule the ground out place nothing.
    brush.terrain.rules.minHeight = 10.f;
    brush.terrain.rules.heightFade = 0.f;
    Require(Engine::Components::ApplyPlacementBrush(scene, nullptr, terrain, brush, dab, random) == 0);
    brush.terrain.rules = {};
    // Erasing at full strength over the whole patch removes them all.
    dab.erase = true;
    dab.radius = 8.f;
    int removed = 0;
    for (int n = 0; n < 3; ++n)
        removed += Engine::Components::ApplyPlacementBrush(scene, nullptr, terrain, brush, dab, random);
    Require(removed == placed && layer->placements.empty());
}

// Painted objects of procedural cells live in per-cell files: none for an
// untouched (generated) cell, one once painted, read back on load, and undo
// (a scene snapshot from before) takes the file back to what it was.
// Painting into a new layer takes the layer name's settings.
void TestObjectFilesAndLayerSettings()
{
    namespace B = Engine::Brushes;
    const auto folder = std::filesystem::temp_directory_path() / "engine_object_files_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder, error);
    const auto builder = [](TerrainManager::Cell, Terrain& terrain)
    {
        terrain.layers.assign(1, Terrain::Layer{});
        terrain.weights.assign(terrain.heights.size(), 1.f);
        std::fill(terrain.heights.begin(), terrain.heights.end(), 2.f);
    };
    // A generated layer: three trees per cell.
    const auto generator = [](const TerrainManager::ObjectLayerContext&, const std::string& name,
        Engine::Components::TerrainObjectLayer& layer)
    {
        if (name != "Forest") return;
        for (int i = 0; i < 3; ++i)
            layer.placements.push_back({0, "Assets/tree.prefab", {4.f + 8.f * i, 2.f, 4.f}});
    };
    const auto makeManager = [&](Engine::Scene::Scene& scene) {
        auto* manager = scene.AddObject("Manager")->AddComponent<TerrainManager>();
        manager->cellsX = manager->cellsY = 32;
        manager->cellWidth = manager->cellDepth = 1.f;
        manager->loadedRadius = 0;
        manager->farRadius = 0;
        manager->generateColliders = false;
        manager->cellDataDirectory = folder.string();
        manager->SetProceduralBuilder(builder);
        manager->SetObjectLayerGenerator(generator);
        return manager;
    };
    const auto layerOf = [](Terrain* cell, const std::string& name) -> Engine::Components::TerrainObjectLayer* {
        if (!cell || !cell->Owner) return nullptr;
        for (auto* child : cell->Owner->Children)
            if (auto* layer = child ? child->GetComponent<Engine::Components::TerrainObjectLayer>() : nullptr)
                if (layer->layerName == name) return layer;
        return nullptr;
    };
    Engine::Scene::Scene scene;
    auto* manager = makeManager(scene);
    Require(manager->AddProceduralCell(0, 0));
    Require(manager->AddObjectLayer("Forest"));
    manager->SyncLoadedCells(0, 0);
    auto* cell = manager->GetLoadedCell(0, 0);
    Require(cell && layerOf(cell, "Forest") && layerOf(cell, "Forest")->placements.size() == 3);
    if (!cell) return;
    const auto file = folder / "0_0.objects";
    // Untouched: no file, and the scene holds no placements.
    const auto untouched = manager->Serialize();
    Require(!std::filesystem::exists(file, error));
    Require(untouched["objectLayerCells"].ArraySize() == 0);
    // The layer name's settings, then paint into it.
    Engine::Components::TerrainObjectLayer::Settings settings;
    settings.viewDistance = 400.f; settings.lod1Distance = 60.f; settings.lod2Distance = 150.f;
    settings.shadowDistance = 120.f; settings.farShadowDistance = 300.f;
    manager->SetObjectLayerSettings("Painted", settings);
    B::BrushPreset brush;
    brush.terrain.placement.assets = {{"Assets/bush.prefab", 1.f, 1.f, 1.f}};
    brush.terrain.placement.density = .5f;
    brush.terrain.placement.minSpacing = 1.f;
    Engine::Components::TerrainPlacementDab dab;
    dab.center = {16.0, 0.0, 16.0};
    dab.radius = 6.f;
    dab.hardness = 1.f;
    dab.layer = "Painted";
    uint32_t random = 99u;
    const int placed = Engine::Components::ApplyPlacementBrush(scene, manager, nullptr, brush, dab, random);
    auto* painted = layerOf(cell, "Painted");
    Require(placed > 5 && painted && painted->placements.size() == size_t(placed));
    if (painted)
        Require(Near(painted->viewDistance, 400.f) && Near(painted->lod1Distance, 60.f) &&
            Near(painted->shadowDistance, 120.f) && Near(painted->farShadowDistance, 300.f));
    // Settings changed later reach the loaded layer too.
    settings.viewDistance = 500.f;
    manager->SetObjectLayerSettings("Painted", settings);
    if (painted) Require(Near(painted->viewDistance, 500.f));
    // Saved: the cell's file, listed by revision; the scene holds no placements.
    const auto edited = manager->Serialize();
    Require(std::filesystem::exists(file, error));
    Require(edited["objectLayerCells"].ArraySize() == 0);
    Require(edited["objectFiles"].ArraySize() == 1 && edited["objectLayerSettings"].ArraySize() == 1);
    // Loaded fresh: the painted layer comes back from the file, with its settings.
    {
        Engine::Scene::Scene loadScene;
        auto* loaded = makeManager(loadScene);
        loaded->Deserialize(edited);
        loaded->SetProceduralBuilder(builder);
        loaded->SetObjectLayerGenerator(generator);
        loaded->SyncLoadedCells(0, 0);
        auto* again = loaded->GetLoadedCell(0, 0);
        auto* layer = layerOf(again, "Painted");
        Require(layer && layer->placements.size() == size_t(placed) && Near(layer->viewDistance, 500.f));
        Require(layerOf(again, "Forest") && layerOf(again, "Forest")->placements.size() == 3);
    }
    // Undo to before the painting: the file goes; redo brings it back.
    {
        Engine::Scene::Scene undoScene;
        auto* undo = makeManager(undoScene);
        undo->Deserialize(untouched);
        Require(!std::filesystem::exists(file, error));
        undo->Deserialize(edited);
        Require(std::filesystem::exists(file, error));
        undo->SetProceduralBuilder(builder);
        undo->SetObjectLayerGenerator(generator);
        undo->SyncLoadedCells(0, 0);
        auto* layer = layerOf(undo->GetLoadedCell(0, 0), "Painted");
        Require(layer && layer->placements.size() == size_t(placed));
    }
    std::filesystem::remove_all(folder, error);
}

// The edge band, trail clearance and per-asset height bands.
void TestBrushEdgeTrailAndAltitudeRules()
{
    namespace B = Engine::Brushes;
    B::BrushRules edge;
    edge.onLayers = {"forest_soil"};
    edge.layerThreshold = .1f;
    edge.maxLayerShare = .6f;
    const auto set = B::ResolveBrushLayers(edge, {"Assets/M/grass.material", "Assets/M/forest_soil.material"});
    const float meadow[2] = {1.f, 0.f}, border[2] = {.6f, .4f}, forest[2] = {0.f, 1.f};
    Require(Near(B::BrushRulesWeight(edge, 0.f, 0.f, &set, meadow, 2), 0.f));
    Require(Near(B::BrushRulesWeight(edge, 0.f, 0.f, &set, border, 2), 1.f));
    Require(Near(B::BrushRulesWeight(edge, 0.f, 0.f, &set, forest, 2), 0.f));
    // Trails: none within clearance, full past clearance + fade.
    B::BrushRules trail;
    trail.trailClearance = 4.f;
    trail.trailFade = 2.f;
    Require(Near(B::BrushRulesWeight(trail, 0.f, 0.f, nullptr, nullptr, 0, 2.f), 0.f));
    Require(Near(B::BrushRulesWeight(trail, 0.f, 0.f, nullptr, nullptr, 0, 5.f), .5f));
    Require(Near(B::BrushRulesWeight(trail, 0.f, 0.f, nullptr, nullptr, 0, 10.f), 1.f));
    Require(Near(B::BrushRulesWeight(trail, 0.f, 0.f), 1.f));   // no trails
    // The manager's trails: distance and share off a published polyline,
    // infinite and zero without one.
    Engine::Scene::Scene scene;
    auto* manager = scene.AddObject("Manager")->AddComponent<TerrainManager>();
    Require(std::isinf(manager->TrailDistance(10.f, 10.f)));
    Require(manager->TrailShare(10.f, 10.f) == 0.f);
    Engine::Components::TerrainTrail path;
    path.points = {{0.f, 50.f}, {600.f, 50.f}, {600.f, 300.f}};
    manager->SetTrail(7u, path);
    Require(Near(manager->TrailDistance(100.f, 50.f), 0.f));
    Require(Near(manager->TrailDistance(100.f, 53.f), 3.f));
    Require(Near(manager->TrailDistance(610.f, 200.f), 10.f));
    Require(Near(manager->TrailShare(100.f, 51.f), .9f));          // inside core: full strength
    Require(manager->TrailShare(100.f, 54.5f) == 0.f);              // past the edge
    Require(std::isinf(manager->TrailDistance(100.f, 400.f)));      // beyond every bin's reach
    // Carving: a flat cell under the trail sinks by depth on the centre line,
    // only over crossable paint.
    {
        const auto network = manager->GetTrails();
        std::vector<float> heights(9 * 9, 0.f), weights(9 * 9 * 2, 0.f);
        for (size_t i = 0; i < heights.size(); ++i) weights[i * 2 + 1] = 1.f;  // all layer 1
        Require(network->Carve(96.f, 34.f, 8, 8, 4.f, 4.f, weights, 2, heights));
        Require(Near(heights[4 * 9 + 2], -.12f));                  // (104, 50): on the line
        Require(heights[0] == 0.f);                                  // (96, 34): 16 m off
        std::vector<float> rock(9 * 9, 0.f), rockWeights(9 * 9 * 2, 0.f);
        for (size_t i = 0; i < rock.size(); ++i) rockWeights[i * 2] = 1.f;  // all layer 0
        path.crossableLayers = 1u << 1;
        manager->SetTrail(7u, path);
        Require(!manager->GetTrails()->Carve(96.f, 34.f, 8, 8, 4.f, 4.f, rockWeights, 2, rock));
    }
    // The GPU layout lists the segments near a bin and packs their shape.
    {
        const auto layout = manager->GetTrails()->PackGpu(nullptr, 0);
        Require(layout.width == 2048 && layout.height >= 1 && layout.keys.w == 2048);
        Require(layout.keys.z > layout.keys.y);
        const float* segment = &layout.texels[size_t(layout.keys.z) * 4];
        Require(segment[0] == 0.f && segment[1] == 50.f && segment[2] == 600.f && segment[3] == 50.f);
        Require(segment[7] == float(1 * 64 + 2));                   // layer 1, crossable layer 1
    }
    manager->RemoveTrail(7u);
    Require(std::isinf(manager->TrailDistance(100.f, 50.f)));
    // Procedural cells carve the trails as they build: at once when the trail
    // comes first, and by reloading the cell once a later change settles.
    {
        Engine::Scene::Scene carveScene;
        auto* carving = carveScene.AddObject("Carving")->AddComponent<TerrainManager>();
        carving->cellsX = carving->cellsY = 32;
        carving->cellWidth = carving->cellDepth = 1.f;
        carving->loadedRadius = carving->farRadius = 0;
        carving->generateColliders = false;
        carving->SetProceduralBuilder([](TerrainManager::Cell, Terrain& terrain) {
            terrain.layers.assign(1, Terrain::Layer{});
            terrain.weights.assign(terrain.heights.size(), 1.f);
            std::fill(terrain.heights.begin(), terrain.heights.end(), 2.f);
            terrain.Rebuild();
        });
        Require(carving->AddProceduralCell(0, 0));
        Engine::Components::TerrainTrail across;
        across.points = {{-10.f, 16.f}, {50.f, 16.f}};
        carving->SetTrail(1u, across);
        carving->SyncLoadedCells(0, 0);
        auto* cell = carving->GetLoadedCell(0, 0);
        Require(cell && Near(cell->heights[16 * 33 + 16], 2.f - .12f));
        Require(cell && cell->heights[0] == 2.f);
        carving->RemoveTrail(1u);
        carving->SyncLoadedCells(0, 0);   // not settled yet: still carved
        cell = carving->GetLoadedCell(0, 0);
        Require(cell && Near(cell->heights[16 * 33 + 16], 2.f - .12f));
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
        carving->SyncLoadedCells(0, 0);
        cell = carving->GetLoadedCell(0, 0);
        Require(cell && cell->heights[16 * 33 + 16] == 2.f);
    }
    // Editor frames call the trail watchers (scripts get no Update there).
    {
        int calls = 0;
        manager->SetTrailWatcher(3u, [&calls] { ++calls; });
        manager->UpdateEditorStreaming(glm::dvec3(0.0), nullptr, 720, 1.f);
        Require(calls == 1);
        manager->RemoveTrailWatcher(3u);
        manager->UpdateEditorStreaming(glm::dvec3(0.0), nullptr, 720, 1.f);
        Require(calls == 1);
    }
    // Species by altitude: an asset's weight fades past its height band.
    B::BrushPlacementAsset spruce{"Assets/spruce.prefab", 2.f, 1.f, 1.f};
    spruce.minHeight = 300.f; spruce.heightFade = 50.f;
    Require(Near(spruce.WeightAt(400.f), 2.f) && Near(spruce.WeightAt(275.f), 1.f) && Near(spruce.WeightAt(200.f), 0.f));
}

// Region placement: the brush over a Select-tool region, inside it only.
void TestPlacementRegion()
{
    namespace B = Engine::Brushes;
    Engine::Scene::Scene scene;
    auto* terrain = MakeTerrain(scene, 1);
    B::BrushPreset brush;
    brush.terrain.placement.assets = {{"Assets/tuft.prefab", 1.f, 1.f, 1.f}};
    brush.terrain.placement.density = 1.f;
    brush.terrain.placement.minSpacing = .8f;
    const auto box = Engine::Components::TerrainRegionMask::Box({2.0, 2.0}, {8.0, 12.0}, false, 0.f);
    uint32_t random = 7u;
    const int placed = Engine::Components::ApplyPlacementRegion(scene, nullptr, terrain, brush, box, 1.f,
        "Region tufts", false, random);
    Require(placed > 20);
    Engine::Components::TerrainObjectLayer* layer = nullptr;
    for (auto* child : terrain->Owner->Children)
        if (auto* found = child->GetComponent<Engine::Components::TerrainObjectLayer>())
            if (found->layerName == "Region tufts") layer = found;
    Require(layer && layer->placements.size() == size_t(placed));
    if (layer)
        for (const auto& p : layer->placements)
            Require(p.position.x >= 2.f && p.position.x <= 8.f && p.position.z >= 2.f && p.position.z <= 12.f);
    // Thinning the whole region at full strength removes them.
    Require(Engine::Components::ApplyPlacementRegion(scene, nullptr, terrain, brush, box, 1.f,
        "Region tufts", true, random) == placed);
}

// Stitching matches loaded neighbours' borders without turning that into an
// edit: an untouched neighbour of an edited cell gets no .terrainedit, and
// the edited cell's file stays as the user saved it.
void TestStitchWritesNoEdits()
{
    const auto folder = std::filesystem::temp_directory_path() / "engine_stitch_edit_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder, error);
    const auto builder = [](TerrainManager::Cell, Terrain& terrain)
    {
        terrain.layers.assign(1, Terrain::Layer{});
        terrain.weights.assign(terrain.heights.size(), 1.f);
        std::fill(terrain.heights.begin(), terrain.heights.end(), 0.f);
    };
    const auto makeManager = [&](Engine::Scene::Scene& scene, int radius) {
        auto* manager = scene.AddObject("Manager")->AddComponent<TerrainManager>();
        manager->cellsX = manager->cellsY = 32;
        manager->cellWidth = manager->cellDepth = 1.f;
        manager->loadedRadius = radius;
        manager->farRadius = 0;
        manager->generateColliders = false;
        manager->cellDataDirectory = folder.string();
        manager->SetProceduralBuilder(builder);
        return manager;
    };
    // A saved edit of (0, 0) that raises its east border by 1 m (as made
    // while (1, 0) was not loaded), and a scene that lists it as edited.
    const auto edited = folder / "0_0.terrainedit", neighbour = folder / "1_0.terrainedit";
    {
        std::ofstream file(edited, std::ios::binary);
        const auto put = [&](auto value) { file.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
        put(uint64_t(0x31544944454e5254ull)); put(uint32_t(1)); put(uint32_t(32)); put(uint32_t(32));
        put(uint32_t(1)); put(uint32_t(33)); put(uint32_t(0));
        for (uint32_t row = 0; row <= 32; ++row) { put(row * 33 + 32); put(1.f); }
    }
    Engine::Scene::Scene editScene;
    auto* editor = makeManager(editScene, 0);
    Require(editor->AddProceduralCell(0, 0) && editor->AddProceduralCell(1, 0));
    auto json = editor->Serialize();
    auto editedCells = Engine::Serialization::JsonValue::MakeArray();
    editedCells.Push(Engine::Serialization::JsonValue::MakeObject()
        .Set("x", Engine::Serialization::JsonValue(0)).Set("y", Engine::Serialization::JsonValue(0)));
    json.Set("editedCells", std::move(editedCells));
    Require(!std::filesystem::exists(neighbour, error));
    const auto bytes = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    };
    const std::string saved = bytes(edited);
    // Both loaded: the seam matches, and saving writes no edit for (1, 0).
    Engine::Scene::Scene loadScene;
    auto* loaded = makeManager(loadScene, 1);
    loaded->Deserialize(json);
    loaded->loadedRadius = 1;
    loaded->SetProceduralBuilder(builder);
    loaded->SyncLoadedCells(0, 0);
    auto* left = loaded->GetLoadedCell(0, 0);
    auto* right = loaded->GetLoadedCell(1, 0);
    Require(left && right);
    if (left && right)
    {
        Require(left->heights[16 * 33 + 32] > .5f);
        for (int row = 0; row <= 32; ++row)
            Require(left->heights[size_t(row) * 33 + 32] == right->heights[size_t(row) * 33]);
    }
    loaded->Serialize();
    Require(!std::filesystem::exists(neighbour, error));
    Require(bytes(edited) == saved);
    std::filesystem::remove_all(folder, error);
}

// The object archive: cells' layers written and read back (positions to a few
// millimetres, yaw to under 1.5 degrees, scale to 1/40), templates kept, empty
// cells empty; a manager loads a cell's placements from it, and a cell's own
// .objects file wins over its block.
void TestObjectArchive()
{
    using Engine::Components::TerrainObjectArchive;
    const auto folder = std::filesystem::temp_directory_path() / "engine_object_archive_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder, error);
    const auto path = (folder / "world.objects").string();
    TerrainObjectArchive::Header header;
    header.extent = 1; header.cellsX = header.cellsY = 32; header.cellWidth = header.cellDepth = 1.f;
    Engine::Scene::Scene templateScene;
    auto* forestTemplate = templateScene.AddObject("t")->AddComponent<Engine::Components::TerrainObjectLayer>();
    forestTemplate->layerName = "Forest";
    forestTemplate->viewDistance = 900.f;
    forestTemplate->objectLods["Assets/oak.prefab"].distances = {60.f, 220.f};
    std::vector<std::pair<std::string, Engine::Serialization::JsonValue>> templates{
        {"Forest", forestTemplate->Serialize()}};
    const auto source = [](TerrainObjectArchive::Cell cell) {
        std::vector<TerrainObjectArchive::Layer> layers;
        if (cell != TerrainObjectArchive::Cell{0, 0}) return layers;
        TerrainObjectArchive::Layer forest{"Forest", {}};
        for (int i = 0; i < 50; ++i)
        {
            Engine::Components::TerrainObjectLayer::Placement p;
            p.asset = i % 2 ? "Assets/oak.prefab" : "Assets/pine.prefab";
            p.position = {i * .61f, 3.25f + i, 31.f - i * .5f};
            p.normal = glm::normalize(glm::vec3(.2f, 1.f, -.1f));
            p.yawDegrees = -170.f + i * 7.f;
            p.scale = .8f + i * .01f;
            forest.placements.push_back(p);
        }
        layers.push_back(forest);
        return layers;
    };
    Require(TerrainObjectArchive::Write(path, header, templates, source));
    const auto archive = TerrainObjectArchive::Open(path);
    Require(archive != nullptr);
    if (!archive) return;
    std::vector<TerrainObjectArchive::Layer> layers;
    Require(archive->ReadCell({0, 0}, layers) && layers.size() == 1 && layers[0].placements.size() == 50);
    // Stored sorted by asset, then x; heights to 1/16 m.
    auto expected = source({0, 0})[0].placements;
    const auto order = [](const auto& a, const auto& b) {
        return a.asset != b.asset ? a.asset < b.asset : a.position.x < b.position.x; };
    std::stable_sort(expected.begin(), expected.end(), order);
    if (layers.size() == 1) std::stable_sort(layers[0].placements.begin(), layers[0].placements.end(), order);
    for (size_t i = 0; i < 50 && layers.size() == 1; ++i)
    {
        const auto& a = layers[0].placements[i];
        const auto& b = expected[i];
        Require(a.asset == b.asset && std::abs(a.position.x - b.position.x) < .005f &&
            std::abs(a.position.z - b.position.z) < .005f && std::abs(a.position.y - b.position.y) <= .032f);
        Require(glm::length(a.normal - b.normal) < .02f && std::abs(a.scale - b.scale) <= .0125f);
        float yaw = std::fmod(std::abs(a.yawDegrees - b.yawDegrees), 360.f);
        Require(std::min(yaw, 360.f - yaw) < 1.5f);
    }
    Require(archive->ReadCell({1, -1}, layers) && layers.empty());
    Require(!archive->ReadCell({2, 0}, layers));
    Engine::Serialization::JsonValue saved;
    Require(archive->ReadCellLayers({0, 0}, saved) && saved.ArraySize() == 1 &&
        Near(saved.ArrayAt(0)["viewDistance"].AsFloat(), 900.f));
    // A manager loads (0, 0)'s placements; an edit file then wins.
    const auto builder = [](TerrainManager::Cell, Terrain& terrain) {
        terrain.layers.assign(1, Terrain::Layer{});
        terrain.weights.assign(terrain.heights.size(), 1.f);
    };
    const auto load = [&](Engine::Scene::Scene& scene) {
        auto* manager = scene.AddObject("Manager")->AddComponent<TerrainManager>();
        manager->cellsX = manager->cellsY = 32;
        manager->cellWidth = manager->cellDepth = 1.f;
        manager->loadedRadius = 0;
        manager->farRadius = 0;
        manager->generateColliders = false;
        manager->cellDataDirectory = folder.string();
        manager->objectArchive = path;
        manager->SetProceduralBuilder(builder);
        manager->AddProceduralCell(0, 0);
        manager->SyncLoadedCells(0, 0);
        return manager;
    };
    const auto forestOf = [](TerrainManager* manager) -> Engine::Components::TerrainObjectLayer* {
        auto* cell = manager->GetLoadedCell(0, 0);
        if (!cell || !cell->Owner) return nullptr;
        for (auto* child : cell->Owner->Children)
            if (auto* layer = child ? child->GetComponent<Engine::Components::TerrainObjectLayer>() : nullptr)
                if (layer->layerName == "Forest") return layer;
        return nullptr;
    };
    {
        Engine::Scene::Scene scene;
        auto* manager = load(scene);
        auto* forest = forestOf(manager);
        Require(forest && forest->placements.size() == 50 && Near(forest->viewDistance, 900.f));
        // Each placement keeps its asset through the load (the layer's packed
        // placements once wrote their asset table in name order: pine, met
        // first here, came back as oak and oak as pine).
        if (forest)
        {
            auto unmatched = expected;
            for (const auto& p : forest->placements)
            {
                const auto found = std::find_if(unmatched.begin(), unmatched.end(), [&](const auto& w) {
                    return w.asset == p.asset && std::abs(w.position.x - p.position.x) < .01f &&
                        std::abs(w.position.z - p.position.z) < .01f;
                });
                Require(found != unmatched.end());
                if (found != unmatched.end()) unmatched.erase(found);
            }
            Require(unmatched.empty());
            // And through a layer's own save and load.
            Engine::Core::Object copyObject;
            auto* copy = copyObject.AddComponent<Engine::Components::TerrainObjectLayer>();
            copy->Deserialize(forest->Serialize());
            Require(copy->placements.size() == forest->placements.size());
            for (size_t i = 0; i < copy->placements.size() && i < forest->placements.size(); ++i)
                Require(copy->placements[i].asset == forest->placements[i].asset);
        }
        if (forest) forest->placements.resize(10);   // an edit: thinned
        manager->Serialize();                         // writes 0_0.objects
        Require(std::filesystem::exists(folder / "0_0.objects", error));
    }
    {
        Engine::Scene::Scene scene;
        auto* forest = forestOf(load(scene));
        Require(forest && forest->placements.size() == 10);
    }
    std::filesystem::remove_all(folder, error);
}

// ---- Height archive v3: per-file scale and coarse cells ----

void TestArchiveV3()
{
    namespace A = TerrainHeightArchive;
    const auto path = (std::filesystem::temp_directory_path() / "terrain_archive_v3_test.heights").string();
    const int extent = 1, cells = 8, stride = 4;
    const float scale = 16.f;
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        const uint64_t signature = 77;
        const int fine = 0;
        A::Write(file, A::Magic); A::Write(file, uint32_t(3)); A::Write(file, signature);
        A::Write(file, extent); A::Write(file, cells); A::Write(file, cells); A::Write(file, scale);
        A::Write(file, fine); A::Write(file, stride);
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                const int n = (x == 0 && y == 0) ? cells + 1 : cells / stride + 1;
                for (int i = 0; i < n * n; ++i)
                    A::Write(file, int16_t(i * 37 - 200 + x * 1000 + y * 3));
                for (int i = 0; i < (n * n + 3) / 4; ++i) A::Write(file, uint8_t(0xe4));
            }
    }
    uint64_t signature = 0;
    Require(A::ReadSignature(path, extent, cells, cells, signature) && signature == 77);
    std::vector<float> heights;
    std::vector<uint8_t> codes;
    Require(A::ReadCellCodes(path, 77, extent, cells, cells, 0, 0, heights, codes));
    Require(heights.size() == 81 && Near(heights[1], (37 - 200) / 16.f));
    // Coarse cell: corners are the stored corners exactly; the middle of the
    // first edge is the rounded mean of its two stored neighbours.
    Require(A::ReadCellCodes(path, 77, extent, cells, cells, 1, -1, heights, codes));
    Require(heights.size() == 81);
    const float a = (-200 + 1000 - 3) / 16.f, b = (37 - 200 + 1000 - 3) / 16.f;
    Require(Near(heights[0], a) && Near(heights[4], b));
    Require(Near(heights[2], std::floor((a + b) * 8.f + .5f) / 16.f));
    Require(codes[0] == 0 && codes[3] == 1 && codes[6] == 2);
    int visited = 0;
    Require(A::ForEachCell(path, 77, extent, cells, cells,
        [&](int, int, const std::vector<float>& h, const std::vector<uint8_t>&)
        { visited += h.size() == 81; return true; }));
    Require(visited == 9);
    std::filesystem::remove(path);
}

// A procedural cell's edit file keeps its sculpting and hand paint (only
// the samples that differ from the builder's), and a version 2 file (with
// the erosion entries edits once kept) still reads.
void TestEditRoundTrip()
{
    using Engine::Components::TerrainRegionMask;
    using Engine::Components::TerrainRegionOperation;
    const auto folder = std::filesystem::temp_directory_path() / "engine_edit_round_trip_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder, error);
    const auto builder = [](TerrainManager::Cell, Terrain& terrain)
    {
        terrain.layers.assign(2, Terrain::Layer{});
        terrain.weights.assign(terrain.heights.size() * 2, 0.f);
        for (size_t i = 0; i < terrain.heights.size(); ++i) terrain.weights[i * 2] = 1.f;
        for (int row = 0; row <= 32; ++row)
            for (int col = 0; col <= 32; ++col)
            {
                const double d = std::hypot(col - 16.0, row - 16.0);
                terrain.heights[size_t(row) * 33 + col] = float(10.0 * std::exp(-d * d / 80.0) +
                    std::sin(col * .7) * std::cos(row * .5));
            }
    };
    const auto makeManager = [&](Engine::Scene::Scene& scene) {
        auto* manager = scene.AddObject("Manager")->AddComponent<TerrainManager>();
        manager->cellsX = manager->cellsY = 32;
        manager->cellWidth = manager->cellDepth = 1.f;
        manager->loadedRadius = 0;
        manager->farRadius = 0;
        manager->generateColliders = false;
        manager->cellDataDirectory = folder.string();
        manager->SetProceduralBuilder(builder);
        return manager;
    };
    Engine::Scene::Scene scene;
    auto* manager = makeManager(scene);
    Require(manager->AddProceduralCell(0, 0));
    manager->SyncLoadedCells(0, 0);
    auto* cell = manager->GetLoadedCell(0, 0);
    Require(cell != nullptr);
    if (!cell) return;
    TerrainRegionOperation op;
    op.kind = TerrainRegionOperation::Kind::Hydraulic;
    op.dropletDensity = 4.f;
    op.erosionUnit = 4.f;
    Require(manager->EditRegion(TerrainRegionMask::Circle({16.0, 16.0}, 10.f, .5f), op));
    Require(cell->ApplyBrush(4.f, 4.f, 2.f, 1.5f, Terrain::Brush::Raise, 0));
    Require(cell->ApplyBrush(28.f, 28.f, 1.5f, 1.f, Terrain::Brush::Paint, 1));
    const auto json = manager->Serialize();
    const auto file = folder / "0_0.terrainedit";
    Require(std::filesystem::exists(file, error));
    std::vector<float> heights(33 * 33, 0.f);
    Require(TerrainHeightArchive::ApplyEditHeights(file.string(), 32, 32, heights));
    // Reloaded from the builder plus the edit: the same heights and paint.
    Engine::Scene::Scene reloadScene;
    auto* reload = makeManager(reloadScene);
    reload->Deserialize(json);
    reload->SetProceduralBuilder(builder);
    reload->SyncLoadedCells(0, 0);
    auto* restored = reload->GetLoadedCell(0, 0);
    Require(restored != nullptr);
    if (restored)
    {
        Require(restored->heights == cell->heights);
        Require(restored->weights == cell->weights);
        Require(restored->weights[(28 * 33 + 28) * 2 + 1] > .5f && restored->weights[(4 * 33 + 4) * 2] == 1.f);
    }
    // Version 2: a height entry, no weights, two erosion entries (skipped).
    const auto legacy = folder / "legacy.terrainedit";
    {
        std::ofstream out(legacy, std::ios::binary);
        const auto put = [&](const auto& value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
        put(uint64_t(0x31544944454e5254ull)); put(uint32_t(2)); put(uint32_t(32)); put(uint32_t(32));
        put(uint32_t(2)); put(uint32_t(1)); put(uint32_t(0));
        put(uint32_t(5)); put(7.5f);
        put(uint32_t(2));
        put(uint32_t(1)); put(.5f); put(1.f);
        put(uint32_t(9)); put(.25f); put(2.f);
    }
    std::vector<float> legacyHeights(33 * 33, 0.f);
    Require(TerrainHeightArchive::ApplyEditHeights(legacy.string(), 32, 32, legacyHeights) &&
        legacyHeights[5] == 7.5f);
    TerrainHeightArchive::EditEntries entries;
    Require(TerrainHeightArchive::ReadEditEntries(legacy.string(), 32, 32, entries) &&
        entries.heights.size() == 1 && entries.weights.empty());
    std::filesystem::remove_all(folder, error);
}

// The macro color map follows edits: an edit marks its cell and those around
// it for recomposing (when the source can), and a patch writes new samples
// over the map, clipped to it.
void TestMacroColorPatches()
{
    using Engine::Components::TerrainRegionMask;
    using Engine::Components::TerrainRegionOperation;
    std::vector<glm::vec4> linear(16, glm::vec4(.5f, .5f, .5f, 0.f));
    auto image = TerrainManager::EncodeMacroColorImage(4, 4, 0.f, 0.f, 16.f, linear);
    Require(image.linear && image.linear->size() == 16 && image.mipLevels == 3);
    TerrainManager::MacroColorPatch patch;
    patch.x0 = 3; patch.y0 = 1; patch.width = 2; patch.height = 1;
    patch.colors = {glm::vec4(1.f, 0.f, 0.f, 1.f), glm::vec4(0.f, 1.f, 0.f, 1.f)};
    const auto patched = TerrainManager::PatchMacroColorImage(image, {patch});
    Require(patched.linear && patched.width == 4);
    if (patched.linear)
    {
        Require((*patched.linear)[1 * 4 + 3] == glm::vec4(1.f, 0.f, 0.f, 1.f));
        Require((*patched.linear)[1 * 4 + 2] == glm::vec4(.5f, .5f, .5f, 0.f));
        // sRGB red, full alpha, at mip 0 sample (3, 1).
        const size_t byte = size_t(1 * 4 + 3) * 4;
        Require(patched.mips[byte] == 255 && patched.mips[byte + 1] == 0 && patched.mips[byte + 3] == 255);
    }
    Require(!TerrainManager::PatchMacroColorImage(TerrainManager::MacroColorImage{}, {patch}).linear);

    Engine::Scene::Scene scene;
    auto* manager = MakeHillManager(scene);
    TerrainManager::MacroColorSource source;
    source.revision = [] { return uint64_t(0); };
    source.makeBuild = [] { return std::function<TerrainManager::MacroColorImage()>{}; };
    manager->SetMacroColorSource(source);
    TerrainRegionOperation flatten;
    flatten.kind = TerrainRegionOperation::Kind::Flatten;
    flatten.height = 2.f;
    Require(manager->EditRegion(TerrainRegionMask::Circle({8.0, 8.0}, 3.f, 1.f), flatten));
    Require(manager->GetMacroDirtyCells().empty()); // the source cannot patch
    source.makeCellUpdate = [](const std::vector<TerrainManager::Cell>&, const TerrainManager::MacroColorImage&)
    { return std::function<std::vector<TerrainManager::MacroColorPatch>()>{}; };
    manager->SetMacroColorSource(source);
    Require(manager->EditRegion(TerrainRegionMask::Circle({8.0, 8.0}, 3.f, 1.f), flatten) ||
        manager->ApplyBrush(8.0, 8.0, 3.f, 1.f, Terrain::Brush::Raise, 0));
    const auto dirty = manager->GetMacroDirtyCells();
    Require(dirty.size() == 9 && std::count(dirty.begin(), dirty.end(), TerrainManager::Cell{-1, -1}) == 1 &&
        std::count(dirty.begin(), dirty.end(), TerrainManager::Cell{1, 1}) == 1);
}

// Far terrain configured after the scene loads takes the saved height
// edits of edited cells, not the source's heights.
void TestFarGridTakesEdits()
{
    const auto folder = std::filesystem::temp_directory_path() / "engine_far_edit_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder, error);
    const auto builder = [](TerrainManager::Cell, Terrain& terrain)
    {
        terrain.layers.assign(1, Terrain::Layer{});
        terrain.weights.assign(terrain.heights.size(), 1.f);
        std::fill(terrain.heights.begin(), terrain.heights.end(), 1.f);
    };
    Engine::Scene::Scene scene;
    auto* manager = scene.AddObject("Manager")->AddComponent<TerrainManager>();
    manager->cellsX = manager->cellsY = 32;
    manager->cellWidth = manager->cellDepth = 1.f;
    manager->loadedRadius = 0;
    manager->farRadius = 0;
    manager->generateColliders = false;
    manager->cellDataDirectory = folder.string();
    manager->SetProceduralBuilder(builder);
    Require(manager->AddProceduralCell(0, 0));
    manager->SyncLoadedCells(0, 0);
    auto* cell = manager->GetLoadedCell(0, 0);
    Require(cell != nullptr);
    if (!cell) return;
    // Raise the cell's first corner by 1 m and save the edit.
    Require(manager->ApplyBrush(0.0, 0.0, 1.5f, 1.f, Terrain::Brush::Raise, 0));
    const auto json = manager->Serialize();
    Require(std::filesystem::exists(folder / "0_0.terrainedit", error));
    Engine::Scene::Scene loadScene;
    auto* loaded = loadScene.AddObject("Manager")->AddComponent<TerrainManager>();
    loaded->SetProceduralBuilder(builder);
    loaded->Deserialize(json);
    // A far grid of the source's heights (1 m everywhere), extent 1.
    loaded->ConfigureFarTerrain(1, std::vector<float>(16, 1.f), {}, "", 0, {});
    Require(loaded->HasFarPatch({0, 0}) && !loaded->HasFarPatch({1, 0}));
    Require(Near(loaded->GetFarCornerHeight(0, 0), 2.f) && Near(loaded->GetFarCornerHeight(1, 1), 1.f));
    std::filesystem::remove_all(folder, error);
}

// The generated landscape: fine play-area cells must share their border
// samples exactly with the coarse cells around them.
void TestLandscapeArchiveSeams()
{
    namespace A = TerrainHeightArchive;
    const std::string path = "Engine/Core/Assets/TerrainData/Landscape/world.heights";
    std::ifstream file(path, std::ios::binary);
    A::Layout layout;
    if (!A::ReadLayout(file, layout, 0, 94, 32, 32))
    {
        std::cout << "Landscape archive not found or not the 94-cell map; seam check skipped\n";
        return;
    }
    file.close();
    uint64_t signature = 0;
    Require(A::ReadSignature(path, layout.extent, 32, 32, signature));
    const int f = layout.fineHalfExtent;
    std::vector<float> inner, outer;
    for (int c = -f; c <= f; c += 7)
    {
        // East edge of the play area: fine (f, c) against coarse (f + 1, c).
        Require(A::ReadCell(path, signature, 94, 32, 32, f, c, inner));
        Require(A::ReadCell(path, signature, 94, 32, 32, f + 1, c, outer));
        for (int row = 0; row <= 32; ++row)
            Require(inner[size_t(row) * 33 + 32] == outer[size_t(row) * 33]);
        // South edge: fine (c, -f) against coarse (c, -f - 1).
        Require(A::ReadCell(path, signature, 94, 32, 32, c, -f, inner));
        Require(A::ReadCell(path, signature, 94, 32, 32, c, -f - 1, outer));
        for (int col = 0; col <= 32; ++col)
            Require(inner[col] == outer[size_t(32) * 33 + col]);
    }
    std::vector<float> farGrid;
    Require(A::ReadFarGrid("Engine/Core/Assets/TerrainData/Landscape/world.fargrid", signature, 94, farGrid));
    Require(A::ReadCell(path, signature, 94, 32, 32, 0, 0, inner));
    Require(farGrid.size() == 190u * 190u && farGrid[size_t(94) * 190 + 94] == inner[0]);
}

// Volume sculpting (todo.md "Terrain: volume-first conversion" step 6): a
// carve saves the cell's edit file, drops or removes the objects it opens
// up under, shows in the field, and the scene copy (Serialize volumeEdits)
// restores the file both ways, as undo does by reloading a snapshot.
void TestVolumeSculpt()
{
    using Engine::Components::TerrainVolumeEdit;
    const auto folder = std::filesystem::temp_directory_path() / "engine_volume_sculpt_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder, error);
    Engine::Scene::Scene scene;
    auto* manager = MakeHillManager(scene);
    manager->cellDataDirectory = folder.string();
    auto* cell = manager->GetLoadedCell(0, 0);
    Require(cell != nullptr);
    if (!cell) return;
    // Objects resting on the ground: one over the carve, one well away.
    float groundAt = 0.f, groundAway = 0.f;
    Require(Engine::Components::SampleTerrainSurface(cell->heights, 32, 32, 1.f, 1.f, 10.f, 16.f, groundAt));
    Require(Engine::Components::SampleTerrainSurface(cell->heights, 32, 32, 1.f, 1.f, 3.f, 3.f, groundAway));
    auto* layerObject = scene.AddObject("Objects");
    scene.MoveObject(layerObject, cell->Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
    auto* layer = layerObject->AddComponent<TerrainObjectLayer>();
    layer->placements.push_back({0, "Assets/rock.prefab", {10.f, groundAt, 16.f}});
    layer->placements.push_back({0, "Assets/rock.prefab", {3.f, groundAway, 3.f}});
    const auto field = manager->GetTerrainField();
    Require(field != nullptr);
    const auto top = field ? field->SurfaceAt(10.f, 16.f) : std::nullopt;
    Require(top && std::abs(top->height - groundAt) < .05f);
    const auto before = manager->Serialize();

    // A vertical shaft 3 m across, 6 m deep, at (10, 16).
    TerrainVolumeEdit shaft;
    shaft.shape = TerrainVolumeEdit::Shape::Capsule;
    shaft.subtract = true;
    shaft.a = {10.f, groundAt - 6.f, 16.f};
    shaft.b = {10.f, groundAt + 20.f, 16.f};
    shaft.radius = 3.f;
    shaft.blend = .5f;
    Require(manager->AddVolumeEdit(shaft, true));
    const auto file = folder / "0_0.volumeedit";
    Require(std::filesystem::exists(file, error));
    Require(manager->GetVolumeEdits({0, 0}).size() == 1);
    const auto carved = manager->GetTerrainField()->SurfaceAt(10.f, 16.f);
    Require(carved && carved->height < groundAt - 5.f);
    // The object over the shaft fell to its floor (or went, on a wall); the
    // other stayed.
    bool sawAway = false;
    for (const auto& placement : layer->placements)
    {
        if (placement.position.x == 3.f) { sawAway = true; Require(placement.position.y == groundAway); }
        else Require(placement.position.y < groundAt - 5.f);
    }
    Require(sawAway);
    const auto after = manager->Serialize();
    Require(after.Has("volumeEdits") && after["volumeEdits"].ArraySize() == 1);

    // Undo: a fresh manager loading the snapshot from before removes the
    // file; redo puts it back, the same edit.
    {
        Engine::Scene::Scene undoScene;
        auto* undo = undoScene.AddObject("Manager")->AddComponent<TerrainManager>();
        undo->Deserialize(before);
        undo->cellDataDirectory = folder.string();
        undo->Deserialize(before);
        Require(!std::filesystem::exists(file, error));
        undo->Deserialize(after);
        Require(std::filesystem::exists(file, error));
        const auto& restored = undo->GetVolumeEdits({0, 0});
        Require(restored.size() == 1 && restored[0].subtract &&
            std::abs(restored[0].radius - 3.f) < 1e-4f && std::abs(restored[0].a.y - shaft.a.y) < 1e-3f);
        // Loading the same scene again leaves the file as it is.
        const auto written = std::filesystem::last_write_time(file, error);
        undo->Deserialize(after);
        Require(std::filesystem::last_write_time(file, error) == written);
    }
    std::filesystem::remove_all(folder, error);
}

// Foliage density (TerrainFoliage.h) and the ground cover grown from it
// (GrassField): the same plants for any tile size, painting changes only
// where it reaches, it is saved and restored, and it holds on any surface.
void TestFoliage()
{
    using Engine::Components::TerrainFoliageCell;
    using Engine::Components::TerrainFoliageStroke;
    using Engine::Components::TerrainVolumeEdit;
    const auto folder = std::filesystem::temp_directory_path() / "engine_foliage_test";
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder, error);
    Engine::Scene::Scene scene;
    auto* manager = MakeHillManager(scene);
    manager->cellDataDirectory = folder.string();
    auto* cell = manager->GetLoadedCell(0, 0);
    Require(cell != nullptr);
    if (!cell) return;
    auto* layerObject = scene.AddObject("Meadow");
    scene.MoveObject(layerObject, cell->Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
    auto* layer = layerObject->AddComponent<TerrainObjectLayer>();
    layer->layerName = "Meadow";
    auto* grass = new GrassField();
    grass->shortMesh = "Assets/a.obj";
    grass->tallMesh = "Assets/b.obj";
    grass->shortPatchesPerSquareMeter = 2.f;
    grass->tallPatchesPerSquareMeter = .5f;
    grass->proceduralLayerMask = 0;
    grass->seed = 5;
    grass->allowProceduralGeneration = true;
    layerObject->AddComponent(grass);
    Require(grass->FoliageLayerName() == "Meadow");
    using Plants = std::map<uint64_t, std::pair<glm::vec3, glm::vec3>>;
    const auto collect = [&](float tile) {
        grass->tileSize = tile;
        Plants plants;
        const int tiles = static_cast<int>(32.f / tile);
        for (int z = 0; z < tiles; ++z)
            for (int x = 0; x < tiles; ++x)
                for (const auto& p : grass->GenerateTileForTesting(x, z))
                {
                    Require(!plants.count(p.id));
                    plants[p.id] = {p.position, p.normal};
                }
        return plants;
    };
    const auto same = [](const Plants& a, const Plants& b) {
        if (a.size() != b.size()) return false;
        for (const auto& [id, plant] : a)
        {
            const auto found = b.find(id);
            if (found == b.end() || glm::distance(found->second.first, plant.first) > 1e-4f) return false;
        }
        return true;
    };
    const Plants open = collect(4.f);
    // About 2.5 per square metre over the cell's 32 x 32 m (less its edges).
    Require(open.size() > 2000 && open.size() < 2800);
    Require(same(open, collect(16.f)));   // tile size does not move a plant
    Require(same(open, collect(4.f)));    // nor does running again

    // Erasing a ball of 5 m around (16, 16): none left near its centre, and
    // every plant past its reach is the same plant as before.
    float height = 0.f;
    Require(Engine::Components::SampleTerrainSurface(cell->heights, 32, 32, 1.f, 1.f, 16.f, 16.f, height));
    TerrainFoliageStroke erase;
    erase.mode = TerrainFoliageStroke::Mode::Erase;
    erase.centre = {16.f, height, 16.f};
    erase.radius = 5.f;
    erase.strength = 1.f;
    erase.falloff = 0.f;
    const uint64_t revision = manager->GetFoliageRevision({0, 0});
    Require(manager->PaintFoliage("Meadow", erase));
    Require(manager->GetFoliageRevision({0, 0}) != revision);
    Require(!manager->GetFoliage({0, 0})->Empty());
    const Plants erased = collect(4.f);
    const auto within = [](const Plants& plants, float low, float high) {
        Plants result;
        for (const auto& [id, plant] : plants)
        {
            const float d = std::hypot(plant.first.x - 16.f, plant.first.z - 16.f);
            if (d >= low && d < high) result[id] = plant;
        }
        return result;
    };
    Require(!within(open, 0.f, 3.f).empty() && within(erased, 0.f, 3.f).empty());
    Require(same(within(open, 7.f, 100.f), within(erased, 7.f, 100.f)));

    // Saved with the manager, and a snapshot brings it back (undo).
    const auto snapshot = manager->Serialize();
    const auto file = folder / "0_0.foliage";
    Require(std::filesystem::exists(file, error));
    TerrainFoliageCell loaded;
    Require(TerrainFoliageCell::Load(file.string(), loaded));
    Require(loaded.Signature() == manager->GetFoliage({0, 0})->Signature());
    manager->SetFoliage({0, 0}, nullptr);
    Require(same(open, collect(4.f)));
    manager->Serialize();                      // the file goes with the paint
    Require(!std::filesystem::exists(file, error));
    {
        Engine::Scene::Scene undoScene;
        auto* undo = undoScene.AddObject("Manager")->AddComponent<TerrainManager>();
        undo->cellDataDirectory = folder.string();
        undo->Deserialize(snapshot);
        undo->cellDataDirectory = folder.string();
        Require(undo->GetFoliage({0, 0})->Signature() == loaded.Signature());
        undo->Serialize();                     // and its file comes back
        Require(std::filesystem::exists(file, error));
    }

    // Any orientation: a slab of rock over the ground at (24, 8). Its
    // underside grows nothing until density is painted there; then plants
    // hang from it, facing down.
    float ground = 0.f;
    Require(Engine::Components::SampleTerrainSurface(cell->heights, 32, 32, 1.f, 1.f, 24.f, 8.f, ground));
    TerrainVolumeEdit slab;
    slab.shape = TerrainVolumeEdit::Shape::Box;
    slab.subtract = false;
    slab.a = {24.f, ground + 4.f, 8.f};
    slab.b = {3.5f, .6f, 3.5f};
    slab.radius = .2f;
    slab.blend = .3f;
    Require(manager->AddVolumeEdit(slab, true));
    const auto facingDown = [](const Plants& plants) {
        size_t count = 0;
        for (const auto& [id, plant] : plants) count += plant.second.y < -.5f ? 1 : 0;
        return count;
    };
    Require(facingDown(collect(4.f)) == 0);
    TerrainFoliageStroke ceiling;
    ceiling.mode = TerrainFoliageStroke::Mode::Add;
    ceiling.centre = {24.f, ground + 3.4f, 8.f};
    ceiling.radius = 3.f;
    ceiling.strength = 1.f;
    ceiling.falloff = 0.f;
    Require(manager->PaintFoliage("Meadow", ceiling));
    const Plants hanging = collect(4.f);
    Require(facingDown(hanging) > 5);
    Require(same(hanging, collect(16.f)));   // in 3D too, tile size does not matter

    // A Foliage brush (its section saved and read back) paints the same way.
    Engine::Brushes::BrushPreset brush;
    brush.terrain.tool = Engine::Brushes::BrushTool::Foliage;
    brush.terrain.foliage.mode = Engine::Brushes::BrushFoliage::Mode::Erase;
    brush.terrain.foliage.layers = {{"Meadow", 1.f}};
    const auto brushPath = folder / "Clear.brush";
    std::string brushError;
    Require(Engine::Brushes::SaveBrushPreset(brushPath, brush, brushError));
    Engine::Brushes::BrushPreset read;
    Require(Engine::Brushes::LoadBrushPreset(brushPath, read, brushError));
    Require(read.terrain.tool == Engine::Brushes::BrushTool::Foliage && read.terrain.foliage.layers.size() == 1 &&
        read.terrain.foliage.layers[0].layer == "Meadow" &&
        read.terrain.foliage.mode == Engine::Brushes::BrushFoliage::Mode::Erase);
    Engine::Components::TerrainFoliageDab dab;
    float corner = 0.f;
    Require(Engine::Components::SampleTerrainSurface(cell->heights, 32, 32, 1.f, 1.f, 6.f, 26.f, corner));
    dab.centre = {6.f, corner, 26.f};
    dab.radius = 4.f;   // a ball: plants up the slope are farther in 3D
    dab.strength = 1.f;
    dab.hardness = 1.f;
    Require(Engine::Components::ApplyFoliageBrush(*manager, read, dab));
    const Plants brushed = collect(4.f);
    size_t nearCount = 0;
    for (const auto& [id, plant] : brushed)
        nearCount += std::hypot(plant.first.x - 6.f, plant.first.z - 26.f) < 2.f ? 1 : 0;
    Require(nearCount == 0);
    std::filesystem::remove_all(folder, error);
}

int main()
{
    try
    {
        TestFalloff();
        TestFlatten();
        TestPaintErase();
        TestManagerFlattenSeam();
        TestStampBrush();
        TestBrushPresetTerrain();
        TestReliefBrushAsset();
        TestBrushRulesOnStrokes();
        TestPlacementBrush();
        TestObjectFilesAndLayerSettings();
        TestBrushEdgeTrailAndAltitudeRules();
        TestPlacementRegion();
        TestStitchWritesNoEdits();
        TestObjectArchive();
        TestScatterAndErase();
        TestPaintMap();
        TestManagerPaintSeam();
        TestRegionMasks();
        TestRegionErosionSeam();
        TestRegionPaint();
        TestVolumeSculpt();
        TestFoliage();
        TestArchiveV3();
                TestEditRoundTrip();
        TestMacroColorPatches();
        TestFarGridTakesEdits();
        TestLandscapeArchiveSeams();
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
