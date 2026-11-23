#include "Core/Components/Terrain/VolumeTerrain.h"

#include "Core/Components/Camera/Camera.h"
#include "Core/Components/Materials/Material.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Components/Physics/MeshObjectCollider.h"
#include "Core/Components/Physics/RigidBody.h"
#include "Core/Components/Terrain/TerrainBatchAppearance.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace Engine::Components
{
namespace
{
constexpr int LevelCount = 4;

// Layer 0 (flat) and 1 (steep) weights in the terrain shader's vertex
// channels, by slope; uv for the paint map's place (unused without one).
void ApplySlopeWeights(std::vector<Mesh::Vertex>& vertices, float start, float end, float extent)
{
    start = std::min(start, end - 1.e-3f);
    for (auto& vertex : vertices)
    {
        const float t = std::clamp((1.f - vertex.normal[1] - start) / (end - start), 0.f, 1.f);
        const float steep = t * t * (3.f - 2.f * t);
        vertex.uvChannels[2][0] = 1.f - steep;
        vertex.uvChannels[2][1] = steep;
        vertex.uvChannels[2][2] = vertex.uvChannels[2][3] = 0.f;
        vertex.uvChannels[3][0] = vertex.uvChannels[3][1] = 0.f;
        vertex.uv[0] = std::clamp(vertex.pos[0] / std::max(extent, 1.f), 0.f, 1.f);
        vertex.uv[1] = std::clamp(vertex.pos[2] / std::max(extent, 1.f), 0.f, 1.f);
        // The terrain shader adds textured layers onto the vertex colour,
        // which carries untextured layers' colours (Terrain::BuildVertex):
        // none here.
        vertex.color[0] = vertex.color[1] = vertex.color[2] = 0.f;
        vertex.color[3] = 1.f;
    }
}

float DistanceToBox(const glm::vec3& point, const glm::vec3& minimum, const glm::vec3& maximum)
{
    const glm::vec3 outside = glm::max(glm::max(minimum - point, point - maximum), glm::vec3(0.f));
    return glm::length(outside);
}
}

VolumeTerrain::VolumeTerrain()
{
    SetTypeName(COMPONENT_TYPE_NAME(VolumeTerrain));
    singlecomponent = true;
    RegisterField("volumeFile", volumeFile, "Volume");
    RegisterField("flatMaterial", flatMaterial, "Shading");
    RegisterField("steepMaterial", steepMaterial, "Shading");
    RegisterField("steepStart", steepStart, "Shading");
    RegisterField("steepEnd", steepEnd, "Shading");
    RegisterField("textureTiling", textureTiling, "Shading");
    RegisterField("worldExtent", worldExtent, "Shading");
    RegisterField("chunkCells", chunkCells, "Detail");
    RegisterField("lod1Distance", lod1Distance, "Detail");
    RegisterField("lod2Distance", lod2Distance, "Detail");
    RegisterField("lod3Distance", lod3Distance, "Detail");
    RegisterField("drawDistance", drawDistance, "Detail");
    RegisterField("collisionDistance", collisionDistance, "Detail");
    RegisterField("lodFadeSeconds", lodFadeSeconds, "Detail");
}

VolumeTerrain::~VolumeTerrain()
{
    // Jobs hold the volume, not this component; just let them finish.
    for (auto& chunk : m_chunks)
        if (chunk.job.valid()) chunk.job.wait();
}

void VolumeTerrain::Deserialize(const Engine::Serialization::JsonValue& value)
{
    Component::Deserialize(value);
    Rebuild();
}

void VolumeTerrain::OnAfterDeserialize(Engine::Graphics::IGraphicsProvider*)
{
    Rebuild();
}

void VolumeTerrain::OnDestroy()
{
    ReleaseChunks();
}

void VolumeTerrain::ReleaseChunks()
{
    for (auto& chunk : m_chunks)
    {
        if (chunk.job.valid()) chunk.job.wait();
        DropChunk(chunk);
    }
    m_chunks.clear();
}

void VolumeTerrain::DropChunk(Chunk& chunk)
{
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    if (scene)
    {
        if (chunk.object) scene->RequestRemoveObject(chunk.object);
        if (chunk.collision) scene->RequestRemoveObject(chunk.collision);
    }
    chunk.object = nullptr;
    chunk.collision = nullptr;
    chunk.mesh = nullptr;
    chunk.shown = -1;
    std::fill(std::begin(chunk.resident), std::end(chunk.resident), false);
    std::fill(std::begin(chunk.empty), std::end(chunk.empty), false);
    std::fill(std::begin(chunk.triangles), std::end(chunk.triangles), size_t(0));
}

void VolumeTerrain::Rebuild()
{
    if (!Owner) return;
    ReleaseChunks();
    m_volume = volumeFile.empty() ? nullptr : TerrainVolume::Load(Mesh::ResolveFilePath(volumeFile));
    if (!m_volume) return;
    // Layer materials, shared by every chunk (one set of layer arrays).
    m_layerMaterials.clear();
    if (!flatMaterial.empty() || !steepMaterial.empty())
        for (const std::string* path : {&flatMaterial, &steepMaterial})
        {
            auto material = std::make_shared<Material>();
            m_layerMaterials.push_back(
                !path->empty() && material->LoadFromFile(Mesh::ResolveFilePath(*path))
                    ? std::move(material) : nullptr);
        }
    const int cells = std::clamp(chunkCells, 4, 256);
    const glm::ivec3 grid((m_volume->sizeX - 1 + cells - 1) / cells, (m_volume->sizeY - 1 + cells - 1) / cells,
        (m_volume->sizeZ - 1 + cells - 1) / cells);
    m_chunks.resize(static_cast<size_t>(grid.x) * grid.y * grid.z);
    size_t next = 0;
    for (int y = 0; y < grid.y; ++y)
        for (int z = 0; z < grid.z; ++z)
            for (int x = 0; x < grid.x; ++x)
            {
                auto& chunk = m_chunks[next++];
                chunk.index = {x, y, z};
                chunk.minimum = m_volume->origin + glm::vec3(chunk.index * cells) * m_volume->spacing;
                chunk.maximum = chunk.minimum + glm::vec3(static_cast<float>(cells) * m_volume->spacing);
            }
}

void VolumeTerrain::Install(Chunk& chunk, Built built)
{
    auto* scene = Owner->GetScene();
    auto* provider = scene ? scene->GetGraphicsProvider() : nullptr;
    const int level = built.level;
    chunk.resident[level] = true;
    chunk.empty[level] = built.indices.empty();
    chunk.triangles[level] = built.indices.size() / 3;
    if (chunk.empty[level] || !scene) return;
    // Hidden level-0 copy for collision, near the camera only.
    std::vector<Mesh::Vertex> collisionVertices;
    std::vector<uint32_t> collisionIndices;
    if (level == 0 && !chunk.collision)
    {
        collisionVertices = built.vertices;
        collisionIndices = built.indices;
    }
    if (!chunk.object)
    {
        const auto& i = chunk.index;
        chunk.object = scene->AddObject("Volume chunk " + std::to_string(i.x) + "," +
            std::to_string(i.y) + "," + std::to_string(i.z));
        chunk.object->runtimeOnly = true;
        scene->MoveObject(chunk.object, Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
        if (!m_layerMaterials.empty())
        {
            auto* appearance = chunk.object->AddComponent<TerrainBatchAppearance>();
            appearance->materials = m_layerMaterials;
            appearance->textureTiling = textureTiling;
            appearance->worldExtent = worldExtent;
        }
        chunk.mesh = chunk.object->AddComponent<Mesh>();
        if (provider) chunk.mesh->PrepareGraphicsResources(provider);
    }
    auto geometry = std::make_unique<Mesh>();
    geometry->SetIndexedGeometry(std::move(built.vertices), std::move(built.indices));
    if (provider) geometry->PrepareGraphicsResources(provider);
    chunk.mesh->StoreRuntimeLod(level, std::move(geometry));
    if (chunk.shown < 0)
    {
        chunk.mesh->SelectRuntimeLod(level);
        chunk.shown = level;
    }
    if (!collisionIndices.empty())
    {
        const auto& i = chunk.index;
        chunk.collision = scene->AddObject("Volume collision " + std::to_string(i.x) + "," +
            std::to_string(i.y) + "," + std::to_string(i.z));
        chunk.collision->runtimeOnly = true;
        scene->MoveObject(chunk.collision, Owner, Engine::Scene::Scene::ObjectPlacement::AsChild);
        auto* mesh = chunk.collision->AddComponent<Mesh>();
        mesh->SetIndexedGeometry(std::move(collisionVertices), std::move(collisionIndices));
        mesh->SetAnimationVisible(false);   // collision only, never drawn
        auto* collider = chunk.collision->AddComponent<MeshObjectCollider>();
        collider->convex = false;
        auto* body = chunk.collision->AddComponent<RigidBody>();
        body->bodyType = "Static";
        body->useGravity = false;
        body->friction = .9f;
        body->restitution = 0.f;
    }
}

void VolumeTerrain::Update()
{
    auto* scene = Owner ? Owner->GetScene() : nullptr;
    if (!scene || !m_volume || m_chunks.empty()) return;
    // The camera, in the volume's space.
    glm::vec3 eye = scene->editorCamera.transform.GetWorldPosition();
    if (scene->HasStarted())
        if (auto* camera = scene->FindGameCamera(); camera && camera->Owner)
            eye = camera->Owner->transform.GetWorldPosition();
    eye = glm::vec3(glm::inverse(Owner->transform.GetWorldMatrix()) * glm::vec4(eye, 1.f));
    const float delta = std::clamp(scene->GetDeltaTime(), 0.f, .1f);

    // Finished meshing jobs.
    for (auto& chunk : m_chunks)
        if (chunk.meshing && chunk.job.valid() &&
            chunk.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            chunk.meshing = false;
            Install(chunk, chunk.job.get());
        }

    const float bounds[LevelCount] = {lod1Distance, lod2Distance, lod3Distance, drawDistance};
    const auto wantedLevel = [&](float distance, int shown)
    {
        // Hysteresis: a shown level holds 10 % past its band.
        for (int level = 0; level < LevelCount; ++level)
        {
            const float limit = bounds[level] * (level >= shown && shown >= 0 ? 1.1f : 1.f);
            if (distance <= limit) return level;
        }
        return -1;
    };
    const int cells = std::clamp(chunkCells, 4, 256);
    const size_t maxJobs = std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 1, 4);
    size_t running = 0;
    for (const auto& chunk : m_chunks) running += chunk.meshing;
    // Chunks wanting a level they lack, nearest first.
    std::vector<std::pair<float, size_t>> wanting;
    m_stats = {};
    for (size_t i = 0; i < m_chunks.size(); ++i)
    {
        auto& chunk = m_chunks[i];
        const float distance = DistanceToBox(eye, chunk.minimum, chunk.maximum);
        const int level = wantedLevel(distance, chunk.shown);
        if (level < 0)
        {
            if (!chunk.meshing && (chunk.object || chunk.resident[0] || chunk.resident[1] ||
                chunk.resident[2] || chunk.resident[3]))
                DropChunk(chunk);   // streamed out
            continue;
        }
        if (!chunk.resident[level])
        {
            if (!chunk.meshing) wanting.emplace_back(distance, i);
        }
        else if (chunk.shown != level && chunk.mesh && !chunk.empty[level])
        {
            if (chunk.mesh->BeginRuntimeLodTransition(level, lodFadeSeconds)) chunk.shown = level;
        }
        if (chunk.object)
            chunk.object->enabled = chunk.shown >= 0 && !chunk.empty[chunk.shown];
        if (chunk.mesh) chunk.mesh->AdvanceRuntimeLodTransition(delta);
        // Collision follows the camera.
        if (chunk.collision && distance > collisionDistance * 1.5f)
        {
            scene->RequestRemoveObject(chunk.collision);
            chunk.collision = nullptr;
        }
        else if (!chunk.collision && distance <= collisionDistance && level == 0 && chunk.resident[0] &&
            !chunk.empty[0] && !chunk.meshing)
            chunk.resident[0] = false;   // remesh level 0 to rebuild its collider copy
        if (chunk.object && chunk.object->enabled)
        {
            ++m_stats.chunksShown;
            m_stats.triangles += chunk.triangles[chunk.shown];
            ++m_stats.chunksAtLevel[chunk.shown];
        }
        m_stats.colliders += chunk.collision != nullptr;
    }
    std::sort(wanting.begin(), wanting.end());
    for (const auto& [distance, i] : wanting)
    {
        if (running >= maxJobs) break;
        auto& chunk = m_chunks[i];
        const int level = wantedLevel(distance, chunk.shown);
        const int stride = 1 << level;
        const glm::ivec3 first = chunk.index * cells;
        const glm::ivec3 span(cells / stride);
        // Skirts on every level: the gap to a neighbour at another level
        // opens on either side of the seam.
        const float skirt = m_volume->spacing * static_cast<float>(stride) * 1.5f;
        const float start = steepStart, end = steepEnd, extent = worldExtent;
        const bool weights = !m_layerMaterials.empty();
        chunk.job = std::async(std::launch::async,
            [volume = m_volume, first, span, stride, skirt, level, start, end, extent, weights]
            {
                Built built;
                built.level = level;
                MeshSurfaceNets(*volume, first, span, stride, skirt, built.vertices, built.indices);
                if (weights) ApplySlopeWeights(built.vertices, start, end, extent);
                return built;
            });
        chunk.meshing = true;
        ++running;
    }
    m_stats.chunksMeshing = running;
}
}
