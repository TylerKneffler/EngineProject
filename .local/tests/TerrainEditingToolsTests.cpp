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
#include "Core/Components/Terrain/TerrainStamp.h"
#include "Editor/Core/View/Views/Scene/SceneTerrainTools.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

// A stamp as a brush: a square pyramid 4 m wide and 2 m tall (stamp space,
// Y up), seen from above, imprints its heights into the height data.
std::shared_ptr<const Engine::Components::TerrainBrushShape> PyramidShape()
{
    Engine::Components::TerrainStamp stamp;
    stamp.width = stamp.height = 2;
    stamp.spacing = 1.f;
    const glm::vec3 corners[4] = {{-2.f, 0.f, -2.f}, {2.f, 0.f, -2.f}, {2.f, 0.f, 2.f}, {-2.f, 0.f, 2.f}};
    for (const auto& corner : corners)
    {
        Engine::Components::TerrainStamp::Point point;
        point.position = corner;
        stamp.points.push_back(point);
    }
    Engine::Components::TerrainStamp::Point apex;
    apex.position = {0.f, 2.f, 0.f};
    apex.weight = 1.f;
    stamp.points.push_back(apex);
    stamp.levels.push_back({0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4});
    return Engine::Components::TerrainBrushShape::FromStamp(stamp, 64);
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

int main()
{
    try
    {
        TestFalloff();
        TestFlatten();
        TestPaintErase();
        TestManagerFlattenSeam();
        TestStampBrush();
        TestScatterAndErase();
        TestPaintMap();
        TestManagerPaintSeam();
        TestRegionMasks();
        TestRegionErosionSeam();
        TestRegionPaint();
        TestVolumeSculpt();
        TestArchiveV3();
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
