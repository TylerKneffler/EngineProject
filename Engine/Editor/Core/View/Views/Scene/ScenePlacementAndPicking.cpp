#include "ScenePlacementAndPicking.h"
#include "SurfacePlacement.h"
#include "Core/Compoonents/Terrain/Terrain.h"

#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Mesh/Mesh.h"
#include "Core/Compoonents/Sprite/Sprite.h"
#include "Core/Scene/Scene.h"
#include "Core/Rendering/RenderSpatialIndex.h"
#include "Core/Threading/JobScheduler.h"
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace Engine::Editor
{
namespace
{
struct TriangleIndex
{
    struct Triangle { uint32_t ids[3]; glm::vec3 minimum, maximum, center; };
    struct Node { glm::vec3 minimum, maximum; size_t begin = 0, end = 0; int left = -1, right = -1; };
    std::vector<glm::vec3> vertices;
    std::vector<Triangle> triangles;
    std::vector<uint32_t> order;
    std::vector<Node> nodes;

    static bool RayBox(const glm::vec3& origin, const glm::vec3& direction,
        const glm::vec3& minimum, const glm::vec3& maximum, float limit)
    {
        float nearDistance = 0.f, farDistance = limit;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::abs(direction[axis]) < 1e-8f)
            {
                if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
                    return false;
                continue;
            }
            float a = (minimum[axis] - origin[axis]) / direction[axis];
            float b = (maximum[axis] - origin[axis]) / direction[axis];
            if (a > b) std::swap(a, b);
            nearDistance = std::max(nearDistance, a);
            farDistance = std::min(farDistance, b);
            if (nearDistance > farDistance) return false;
        }
        return farDistance > 0.f;
    }

    int BuildNode(size_t begin, size_t end)
    {
        const int id = static_cast<int>(nodes.size());
        nodes.emplace_back();
        glm::vec3 minimum(FLT_MAX), maximum(-FLT_MAX);
        for (size_t i = begin; i < end; ++i)
        {
            const Triangle& triangle = triangles[order[i]];
            minimum = glm::min(minimum, triangle.minimum);
            maximum = glm::max(maximum, triangle.maximum);
        }
        nodes[id].minimum = minimum;
        nodes[id].maximum = maximum;
        nodes[id].begin = begin;
        nodes[id].end = end;
        if (end - begin <= 8) return id;
        const glm::vec3 extent = maximum - minimum;
        const int axis = extent.x > extent.y && extent.x > extent.z ? 0 :
            extent.y > extent.z ? 1 : 2;
        const size_t middle = begin + (end - begin) / 2;
        std::nth_element(order.begin() + begin, order.begin() + middle,
            order.begin() + end, [&](uint32_t a, uint32_t b) {
                return triangles[a].center[axis] < triangles[b].center[axis];
            });
        const int left = BuildNode(begin, middle);
        const int right = BuildNode(middle, end);
        nodes[id].left = left;
        nodes[id].right = right;
        return id;
    }

    static TriangleIndex Prepare(Engine::Components::Mesh::PoseSnapshot snapshot,
        std::vector<glm::mat4> palette)
    {
        TriangleIndex result;
        const auto posed = Engine::Components::Mesh::BuildPosedVertices(
            snapshot, &palette);
        result.vertices.reserve(posed.size());
        for (const auto& vertex : posed)
            result.vertices.emplace_back(vertex.pos[0], vertex.pos[1], vertex.pos[2]);
        const size_t count = snapshot.indices.empty() ? posed.size() :
            snapshot.indices.size();
        result.triangles.reserve(count / 3);
        for (size_t i = 0; i + 2 < count; i += 3)
        {
            const uint32_t a = snapshot.indices.empty() ? static_cast<uint32_t>(i) : snapshot.indices[i];
            const uint32_t b = snapshot.indices.empty() ? static_cast<uint32_t>(i + 1) : snapshot.indices[i + 1];
            const uint32_t c = snapshot.indices.empty() ? static_cast<uint32_t>(i + 2) : snapshot.indices[i + 2];
            if (a >= result.vertices.size() || b >= result.vertices.size() ||
                c >= result.vertices.size()) continue;
            const glm::vec3 minimum = glm::min(result.vertices[a],
                glm::min(result.vertices[b], result.vertices[c]));
            const glm::vec3 maximum = glm::max(result.vertices[a],
                glm::max(result.vertices[b], result.vertices[c]));
            result.triangles.push_back({ { a, b, c }, minimum, maximum,
                (minimum + maximum) * .5f });
        }
        result.order.resize(result.triangles.size());
        std::iota(result.order.begin(), result.order.end(), 0u);
        if (!result.order.empty()) result.BuildNode(0, result.order.size());
        return result;
    }

    bool Intersect(const glm::vec3& origin, const glm::vec3& direction,
        float& distance, size_t* triangleIndex = nullptr,
        float* baryU = nullptr, float* baryV = nullptr,
        size_t firstVertex = 0, size_t vertexCount = SIZE_MAX) const
    {
        if (nodes.empty()) return false;
        float nearest = FLT_MAX, nearestU = 0.f, nearestV = 0.f;
        size_t nearestTriangle = 0;
        std::vector<int> stack{ 0 };
        while (!stack.empty())
        {
            const int id = stack.back(); stack.pop_back();
            const Node& node = nodes[id];
            if (!RayBox(origin, direction, node.minimum, node.maximum, nearest))
                continue;
            if (node.left >= 0)
            { stack.push_back(node.left); stack.push_back(node.right); continue; }
            for (size_t i = node.begin; i < node.end; ++i)
            {
                const size_t triangle = order[i];
                const auto& ids = triangles[triangle].ids;
                if (ids[0] < firstVertex || ids[0] - firstVertex >= vertexCount ||
                    ids[1] < firstVertex || ids[1] - firstVertex >= vertexCount ||
                    ids[2] < firstVertex || ids[2] - firstVertex >= vertexCount) continue;
                const glm::vec3& a = vertices[ids[0]];
                const glm::vec3 e1 = vertices[ids[1]] - a;
                const glm::vec3 e2 = vertices[ids[2]] - a;
                const glm::vec3 p = glm::cross(direction, e2);
                const float det = glm::dot(e1, p);
                if (std::abs(det) < 1e-7f) continue;
                const float inverse = 1.f / det;
                const glm::vec3 offset = origin - a;
                const float u = glm::dot(offset, p) * inverse;
                if (u < 0.f || u > 1.f) continue;
                const glm::vec3 q = glm::cross(offset, e1);
                const float v = glm::dot(direction, q) * inverse;
                if (v < 0.f || u + v > 1.f) continue;
                const float hit = glm::dot(e2, q) * inverse;
                if (hit <= 0.f || hit >= nearest) continue;
                nearest = hit; nearestTriangle = triangle;
                nearestU = u; nearestV = v;
            }
        }
        if (nearest == FLT_MAX) return false;
        distance = nearest;
        if (triangleIndex) *triangleIndex = nearestTriangle;
        if (baryU) *baryU = nearestU;
        if (baryV) *baryV = nearestV;
        return true;
    }
};

struct SkinnedEntry
{
    uint64_t geometryRevision = 0, morphRevision = 0;
    uint64_t configurationRevision = 0, surfaceRevision = 0;
    std::vector<glm::mat4> palette;
    std::future<TriangleIndex> pending;
    std::shared_ptr<const TriangleIndex> ready;
};
std::unordered_map<const Engine::Components::Mesh*, SkinnedEntry> skinnedEntries;
struct StaticEntry
{
    uint64_t geometry = 0, morph = 0, surface = 0;
    std::shared_ptr<const TriangleIndex> triangles;
};
std::unordered_map<const Engine::Components::Mesh*, StaticEntry> staticEntries;

std::shared_ptr<const TriangleIndex> StaticIndex(const Engine::Components::Mesh& mesh)
{
    auto& entry = staticEntries[&mesh];
    if (!entry.triangles || entry.geometry != mesh.GetAuthoredGeometryRevision() ||
        entry.morph != mesh.GetMorphWeightsRevision() ||
        entry.surface != mesh.GetSurfaceOffsetsRevision())
    {
        auto snapshot = mesh.CapturePoseSnapshot();
        for (const auto& terrainVertex : mesh.GetTerrainVertices())
        {
            Engine::Components::Mesh::Vertex vertex{};
            for (int axis = 0; axis < 3; ++axis) vertex.pos[axis] = terrainVertex.pos[axis];
            snapshot.vertices.push_back(vertex);
        }
        entry.triangles = std::make_shared<TriangleIndex>(
            TriangleIndex::Prepare(std::move(snapshot), {}));
        entry.geometry = mesh.GetAuthoredGeometryRevision();
        entry.morph = mesh.GetMorphWeightsRevision();
        entry.surface = mesh.GetSurfaceOffsetsRevision();
    }
    return entry.triangles;
}
const Engine::Scene::Scene* cachedScene = nullptr;
uint64_t cachedStructureRevision = 0;
struct SpatialCache
{
    const Engine::Scene::Scene* scene = nullptr;
    uint64_t structureRevision = 0;
    std::vector<Engine::Core::Object*> objects;
    Engine::Rendering::RenderSpatialIndex index;
};
SpatialCache spatialCache;
struct HoverCandidate
{
    Engine::Core::Object* object = nullptr;
    glm::vec3 origin{}, direction{}, minimum{}, maximum{};
    std::shared_ptr<const TriangleIndex> triangles;
    float travelled = 0.f, maximumDistance = FLT_MAX;
    size_t firstVertex = 0, vertexCount = SIZE_MAX;
    bool sphere = false;

    float Intersect() const
    {
        float distance = FLT_MAX;
        if (triangles)
        {
            if (!triangles->Intersect(origin, direction, distance, nullptr, nullptr, nullptr,
                    firstVertex, vertexCount)) return FLT_MAX;
        }
        else if (sphere)
        {
            const glm::vec3 offset = origin - minimum;
            const float a = glm::dot(direction, direction);
            const float b = 2.f * glm::dot(offset, direction);
            const float c = glm::dot(offset, offset) - maximum.x * maximum.x;
            const float discriminant = b * b - 4.f * a * c;
            if (discriminant < 0.f || a <= 0.f) return FLT_MAX;
            const float root = std::sqrt(discriminant);
            const float first = (-b - root) / (2.f * a);
            const float second = (-b + root) / (2.f * a);
            distance = first > 0.f ? first : second;
        }
        else
        {
            float nearDistance = 0.f, farDistance = maximumDistance;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::abs(direction[axis]) < 1e-8f)
                {
                    if (origin[axis] < minimum[axis] ||
                        origin[axis] > maximum[axis]) return FLT_MAX;
                    continue;
                }
                float first = (minimum[axis] - origin[axis]) / direction[axis];
                float second = (maximum[axis] - origin[axis]) / direction[axis];
                if (first > second) std::swap(first, second);
                nearDistance = std::max(nearDistance, first);
                farDistance = std::min(farDistance, second);
                if (nearDistance > farDistance) return FLT_MAX;
            }
            distance = nearDistance > 0.f ? nearDistance : farDistance;
        }
        return distance > 0.f && distance < maximumDistance
            ? travelled + distance : FLT_MAX;
    }
};
// Preserve the world ray parameter when transforming into mesh/instance space.
std::vector<HoverCandidate> StaticCandidates(const Engine::Core::Object& object,
    const Engine::Components::Mesh& mesh, const glm::vec3& origin,
    const glm::vec3& direction)
{
    std::vector<HoverCandidate> result;
    const auto append = [&](const glm::mat4& world, size_t first, size_t count,
                            const Engine::Components::Mesh& source) {
        if (std::abs(glm::determinant(world)) < 1e-8f) return;
        const auto inverse = glm::inverse(world);
        HoverCandidate candidate;
        candidate.origin = glm::vec3(inverse * glm::vec4(origin, 1.f));
        candidate.direction = glm::vec3(inverse * glm::vec4(direction, 0.f));
        if (source.HasBounds() && !TriangleIndex::RayBox(candidate.origin,
                candidate.direction, source.GetBoundsMin(), source.GetBoundsMax(), FLT_MAX)) return;
        candidate.triangles = StaticIndex(source);
        candidate.firstVertex = first;
        candidate.vertexCount = count;
        result.push_back(std::move(candidate));
    };
    const auto world = object.transform.GetWorldMatrix();
    if (!mesh.IsInstanced()) append(world, 0, SIZE_MAX, mesh);
    else if (const auto& data = mesh.GetInstanceData())
        for (const auto& range : mesh.GetInstanceRanges())
            for (size_t i = range.firstInstance;
                 i < size_t(range.firstInstance) + range.instanceCount && i < data->transforms.size(); ++i)
            {
                glm::mat4 instance(1.f);
                for (int row = 0; row < 3; ++row)
                    for (int column = 0; column < 4; ++column)
                        instance[column][row] = data->transforms[i].rows[row][column];
                append(world * instance, range.firstVertex, range.vertexCount,
                    *mesh.GetInstanceSource());
            }
    return result;
}

bool IntersectStaticMesh(const Engine::Core::Object& object,
    const Engine::Components::Mesh& mesh, const glm::vec3& origin,
    const glm::vec3& direction, float& distance)
{
    distance = FLT_MAX;
    for (const auto& candidate : StaticCandidates(object, mesh, origin, direction))
        distance = std::min(distance, candidate.Intersect());
    return distance < FLT_MAX;
}

struct HoverState
{
    uint64_t signature = 0;
    std::future<Engine::Core::Object*> pending;
    Engine::Core::Object* result = nullptr;
    bool completed = false;
};
HoverState hoverState;

uint64_t HashBytes(uint64_t hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i)
    { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}

uint64_t HoverSignature(const Engine::Scene::Scene& scene,
    const EditorUiVec2& mouse, const EditorUiVec2& viewport)
{
    uint64_t hash = 14695981039346656037ull;
    const auto* sceneAddress = &scene;
    hash = HashBytes(hash, &sceneAddress, sizeof(sceneAddress));
    const uint64_t structure = scene.GetStructureRevision();
    hash = HashBytes(hash, &structure, sizeof(structure));
    const uint64_t physics = scene.GetPhysicsActivityRevision();
    hash = HashBytes(hash, &physics, sizeof(physics));
    hash = HashBytes(hash, &mouse, sizeof(mouse));
    hash = HashBytes(hash, &viewport, sizeof(viewport));
    const auto* camera = scene.editorCamera.GetComponent<Engine::Components::Camera>();
    if (camera && viewport.y > 0.f)
    {
        const glm::mat4 view = camera->GetViewMatrix();
        const glm::mat4 projection = camera->GetProjectionMatrix(viewport.x / viewport.y);
        hash = HashBytes(hash, &view, sizeof(view));
        hash = HashBytes(hash, &projection, sizeof(projection));
    }
    for (const auto& entry : scene.GetObjects())
    {
        auto* object = entry.get();
        hash = HashBytes(hash, &object, sizeof(object));
        if (!object) continue;
        const uint64_t world = object->transform.GetWorldRevision();
        hash = HashBytes(hash, &world, sizeof(world));
        const bool enabled = object->IsEnabledInHierarchy();
        hash = HashBytes(hash, &enabled, sizeof(enabled));
        for (auto* component : object->Components)
        {
            hash = HashBytes(hash, &component, sizeof(component));
            if (component)
            {
                const uint64_t configuration = component->GetConfigurationRevision();
                hash = HashBytes(hash, &configuration, sizeof(configuration));
            }
        }
        auto* mesh = object->GetComponent<Engine::Components::Mesh>();
        if (!mesh) continue;
        const bool visible = mesh->IsEditorVisible();
        hash = HashBytes(hash, &visible, sizeof(visible));
        if (const auto& instances = mesh->GetInstanceData())
            hash = HashBytes(hash, &instances->revision, sizeof(instances->revision));
        const uint64_t revisions[] = { mesh->GetAuthoredGeometryRevision(),
            mesh->GetConfigurationRevision(), mesh->GetMorphWeightsRevision(),
            mesh->GetSurfaceOffsetsRevision() };
        hash = HashBytes(hash, revisions, sizeof(revisions));
        if (auto* skin = object->GetComponent<Engine::Components::SkinnedMesh>())
            for (const auto& matrix : skin->BuildPalette())
                hash = HashBytes(hash, &matrix, sizeof(matrix));
    }
    return hash;
}

void UpdateSpatialCache(const Engine::Scene::Scene& scene)
{
    const auto& sceneObjects = scene.GetObjects();
    if (spatialCache.scene != &scene ||
        spatialCache.structureRevision != scene.GetStructureRevision() ||
        spatialCache.objects.size() != sceneObjects.size())
    {
        spatialCache.scene = &scene;
        spatialCache.structureRevision = scene.GetStructureRevision();
        spatialCache.objects.clear();
        spatialCache.objects.reserve(sceneObjects.size());
        for (const auto& object : sceneObjects)
            spatialCache.objects.push_back(object.get());
    }
    std::vector<Engine::Rendering::RenderBounds> bounds;
    bounds.reserve(spatialCache.objects.size());
    for (auto* object : spatialCache.objects)
    {
        Engine::Rendering::RenderBounds box;
        if (!object || !object->IsEnabledInHierarchy())
        { bounds.push_back({ glm::vec3(0.f), glm::vec3(0.f), true }); continue; }
        auto* mesh = object->GetComponent<Engine::Components::Mesh>();
        if (mesh && mesh->HasBounds() &&
            !object->GetComponent<Engine::Components::SkinnedMesh>())
            box = Engine::Rendering::TransformBounds(mesh->GetBoundsMin(),
                mesh->GetBoundsMax(), object->transform.GetWorldMatrix());
        else if (mesh && mesh->HasBounds())
            box.bounded = false; // Animated vertices can leave bind bounds.
        else if (object->GetComponent<Engine::Components::Sprite>() || mesh)
        {
            const glm::vec3 center = object->transform.GetWorldPosition();
            const float radius = glm::max(glm::max(object->transform.scale.x,
                object->transform.scale.y), object->transform.scale.z) * .75f;
            box = { center - glm::vec3(radius), center + glm::vec3(radius), true };
        }
        else
            box = { glm::vec3(0.f), glm::vec3(0.f), true };
        bounds.push_back(box);
    }
    spatialCache.index.Update(std::move(bounds));
}

const TriangleIndex* SkinnedIndex(const Engine::Components::Mesh& mesh,
    const Engine::Components::SkinnedMesh& skin, bool wait)
{
    const std::vector<glm::mat4> palette = skin.BuildPalette();
    const uint64_t geometryRevision = mesh.GetAuthoredGeometryRevision();
    const uint64_t morphRevision = mesh.GetMorphWeightsRevision();
    const uint64_t configurationRevision = mesh.GetConfigurationRevision();
    const uint64_t surfaceRevision = mesh.GetSurfaceOffsetsRevision();
    auto& entry = skinnedEntries[&mesh];
    const bool samePose = entry.palette.size() == palette.size() &&
        std::equal(entry.palette.begin(), entry.palette.end(), palette.begin(),
            [](const glm::mat4& a, const glm::mat4& b) {
                return std::memcmp(&a, &b, sizeof(glm::mat4)) == 0;
            });
    if (entry.geometryRevision != geometryRevision ||
        entry.morphRevision != morphRevision ||
        entry.configurationRevision != configurationRevision ||
        entry.surfaceRevision != surfaceRevision || !samePose ||
        (!entry.ready && !entry.pending.valid()))
    {
        entry.geometryRevision = geometryRevision;
        entry.morphRevision = morphRevision;
        entry.configurationRevision = configurationRevision;
        entry.surfaceRevision = surfaceRevision;
        entry.palette = palette;
        entry.ready.reset();
        entry.pending = Engine::Threading::JobScheduler::Get().Submit(
            [snapshot = mesh.CapturePoseSnapshot(), palette]() mutable {
                return TriangleIndex::Prepare(std::move(snapshot), std::move(palette));
            });
    }
    if (!entry.ready && entry.pending.valid() && (wait ||
        entry.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
        entry.ready = std::make_shared<TriangleIndex>(entry.pending.get());
    return entry.ready.get();
}
bool BuildViewportRay(const Engine::Scene::Scene& scene,
    const EditorUiVec2& mousePos, const EditorUiVec2& viewportSize,
    glm::vec3& origin, glm::vec3& direction)
{
    if (viewportSize.x <= 1.f || viewportSize.y <= 1.f)
        return false;
    const Engine::Components::Camera* camera =
        scene.editorCamera.GetComponent<Engine::Components::Camera>();
    if (!camera)
        return false;

    const glm::mat4 inverseViewProjection = glm::inverse(
        camera->GetProjectionMatrix(viewportSize.x / viewportSize.y) *
        camera->GetViewMatrix());
    const float ndcX = 2.f * mousePos.x / viewportSize.x - 1.f;
    const float ndcY = 1.f - 2.f * mousePos.y / viewportSize.y;
    glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, 0.f, 1.f);
    glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, 1.f, 1.f);
    if (std::abs(nearPoint.w) < 0.000001f || std::abs(farPoint.w) < 0.000001f)
        return false;
    origin = glm::vec3(nearPoint) / nearPoint.w;
    const glm::vec3 target = glm::vec3(farPoint) / farPoint.w;
    direction = glm::normalize(target - origin);
    return true;
}

bool IsInObjectHierarchy(const Engine::Core::Object* object,
    const Engine::Core::Object* root)
{
    for (const Engine::Core::Object* current = object; current;
        current = current->Parent)
    {
        if (current == root)
            return true;
    }
    return false;
}

bool IntersectMeshBounds(const Engine::Core::Object& object,
    const Engine::Components::Mesh& mesh, const glm::vec3& rayOrigin,
    const glm::vec3& rayDirection, float& distance)
{
    if (!mesh.HasBounds())
        return false;
    const glm::mat4 inverseWorld = glm::inverse(object.transform.GetWorldMatrix());
    const glm::vec3 localOrigin = glm::vec3(inverseWorld * glm::vec4(rayOrigin, 1.f));
    const glm::vec3 localDirection = glm::vec3(inverseWorld * glm::vec4(rayDirection, 0.f));
    float nearDistance = 0.f;
    float farDistance = FLT_MAX;
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(localDirection[axis]) < 0.000001f)
        {
            if (localOrigin[axis] < mesh.GetBoundsMin()[axis] ||
                localOrigin[axis] > mesh.GetBoundsMax()[axis])
            {
                return false;
            }
            continue;
        }

        float first = (mesh.GetBoundsMin()[axis] - localOrigin[axis]) /
            localDirection[axis];
        float second = (mesh.GetBoundsMax()[axis] - localOrigin[axis]) /
            localDirection[axis];
        if (first > second)
            std::swap(first, second);
        nearDistance = glm::max(nearDistance, first);
        farDistance = glm::min(farDistance, second);
        if (nearDistance > farDistance)
            return false;
    }

    distance = nearDistance > 0.f ? nearDistance : farDistance;
    return distance > 0.f;
}

bool IntersectSkinnedMesh(const Engine::Core::Object& object,
    const Engine::Components::Mesh& mesh,
    const Engine::Components::SkinnedMesh& skinned,
    const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
    float& distance)
{
    const auto* index = SkinnedIndex(mesh, skinned, true);
    const glm::mat4 inverse = glm::inverse(object.transform.GetWorldMatrix());
    return index && index->Intersect(glm::vec3(inverse * glm::vec4(rayOrigin, 1.f)),
        glm::vec3(inverse * glm::vec4(rayDirection, 0.f)), distance);
}
}

bool ScenePlacementAndPicking::FindTerrainPlacement(
    const Engine::Scene::Scene& scene, const Engine::Core::Object* terrainRoot,
    const EditorUiVec2& mousePos, const EditorUiVec2& viewportSize,
    glm::vec3& placement, glm::vec3& normal)
{
    glm::vec3 origin{}, direction{};
    if (!terrainRoot || !BuildViewportRay(scene, mousePos, viewportSize,
            origin, direction)) return false;
    double nearest = std::numeric_limits<double>::max();
    bool found = false;
    for (const auto& entry : scene.GetObjects())
    {
        auto* object = entry.get();
        if (!object || !object->IsEnabledInHierarchy() ||
            !IsInObjectHierarchy(object, terrainRoot) ||
            !object->GetComponent<Engine::Components::Terrain>()) continue;
        auto* mesh = object->GetComponent<Engine::Components::Mesh>();
        float boundsDistance = 0.f;
        if (!mesh || !IntersectMeshBounds(*object, *mesh, origin,
                direction, boundsDistance)) continue;
        const auto& vertices = mesh->GetVertices();
        const auto& indices = mesh->GetIndices();
        const auto world = object->transform.GetWorldMatrixD();
        const auto up = glm::transpose(glm::inverse(glm::dmat3(world))) *
            glm::dvec3(0.0, 1.0, 0.0);
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            glm::dvec3 points[3];
            bool valid = true;
            for (int j = 0; j < 3; ++j)
            {
                if (indices[i + j] >= vertices.size()) { valid = false; break; }
                const auto& v = vertices[indices[i + j]];
                points[j] = glm::dvec3(world *
                    glm::dvec4(v.pos[0], v.pos[1], v.pos[2], 1.0));
            }
            glm::dvec3 hitNormal;
            if (valid && IntersectPlacementTriangle(glm::dvec3(origin),
                    glm::dvec3(direction), points[0], points[1], points[2],
                    nearest, hitNormal))
            {
                if (glm::dot(hitNormal, up) < 0.0) hitNormal = -hitNormal;
                normal = glm::vec3(hitNormal);
                found = true;
            }
        }
    }
    if (found) placement = glm::vec3(glm::dvec3(origin) +
        glm::dvec3(direction) * nearest);
    return found;
}

void ScenePlacementAndPicking::ClearCaches()
{
    skinnedEntries.clear();
    staticEntries.clear();
    spatialCache = {};
    hoverState = {};
    cachedScene = nullptr;
    cachedStructureRevision = 0;
}

void ScenePlacementAndPicking::PrepareSkinnedGeometry(
    const Engine::Scene::Scene& scene)
{
    if (cachedScene != &scene ||
        cachedStructureRevision != scene.GetStructureRevision())
    {
        ClearCaches();
        cachedScene = &scene;
        cachedStructureRevision = scene.GetStructureRevision();
    }
    for (const auto& entry : scene.GetObjects())
    {
        auto* object = entry.get();
        if (!object || !object->IsEnabledInHierarchy()) continue;
        auto* mesh = object->GetComponent<Engine::Components::Mesh>();
        auto* skin = object->GetComponent<Engine::Components::SkinnedMesh>();
        if (mesh && skin && mesh->IsEditorVisible())
            SkinnedIndex(*mesh, *skin, false);
    }
}

Engine::Core::Object* ScenePlacementAndPicking::PickObjectHoverAsync(
    const Engine::Scene::Scene& scene, const EditorUiVec2& mousePos,
    const EditorUiVec2& viewportSize)
{
    if (scene.IsEditorMode2D())
        return PickObjectInViewport(scene, mousePos, viewportSize, true);
    glm::vec3 rayOrigin{}, rayDirection{};
    if (!BuildViewportRay(scene, mousePos, viewportSize,
        rayOrigin, rayDirection)) return nullptr;
    const uint64_t signature = HoverSignature(scene, mousePos, viewportSize);
    if (hoverState.signature == signature)
    {
        if (!hoverState.completed && hoverState.pending.valid() &&
            hoverState.pending.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready)
        {
            hoverState.result = hoverState.pending.get();
            hoverState.completed = true;
        }
        return hoverState.completed ? hoverState.result : nullptr;
    }

    UpdateSpatialCache(scene);
    std::vector<HoverCandidate> candidates;
    float travelled = 0.f;
    bool geometryPending = false;
    const auto segments = scene.TracePortalRay({ rayOrigin, rayDirection }, 10000.f);
    for (const auto& segment : segments)
    {
        for (uint32_t item : spatialCache.index.QueryRay(segment.ray.origin,
            segment.ray.direction, segment.maxDistance))
        {
            auto* object = spatialCache.objects[item];
            if (!object || !object->IsEnabledInHierarchy() ||
                !Engine::Scene::Scene::IsObjectInSpatialRegion(
                    object, segment.contentScopeRoot)) continue;
            auto* mesh = object->GetComponent<Engine::Components::Mesh>();
            auto* sprite = object->GetComponent<Engine::Components::Sprite>();
            if ((!mesh && !sprite) || (mesh && !mesh->IsEditorVisible())) continue;
            HoverCandidate candidate;
            candidate.object = object;
            candidate.travelled = travelled;
            candidate.maximumDistance = segment.maxDistance;
            if (mesh && mesh->HasBounds())
            {
                const glm::mat4 inverse = glm::inverse(
                    object->transform.GetWorldMatrix());
                candidate.origin = glm::vec3(inverse *
                    glm::vec4(segment.ray.origin, 1.f));
                candidate.direction = glm::vec3(inverse *
                    glm::vec4(segment.ray.direction, 0.f));
                candidate.minimum = mesh->GetBoundsMin();
                candidate.maximum = mesh->GetBoundsMax();
                if (!object->GetComponent<Engine::Components::SkinnedMesh>())
                {
                    for (auto geometry : StaticCandidates(*object, *mesh,
                            segment.ray.origin, segment.ray.direction))
                    {
                        geometry.object = candidate.object;
                        geometry.travelled = candidate.travelled;
                        geometry.maximumDistance = candidate.maximumDistance;
                        candidates.push_back(std::move(geometry));
                    }
                    continue;
                }
                if (auto* skin = object->GetComponent<Engine::Components::SkinnedMesh>())
                {
                    SkinnedIndex(*mesh, *skin, false);
                    const auto found = skinnedEntries.find(mesh);
                    candidate.triangles = found->second.ready;
                    if (!candidate.triangles) geometryPending = true;
                }
            }
            else
            {
                candidate.sphere = true;
                candidate.origin = segment.ray.origin;
                candidate.direction = segment.ray.direction;
                candidate.minimum = object->transform.GetWorldPosition();
                const float radius = glm::max(glm::max(object->transform.scale.x,
                    object->transform.scale.y), object->transform.scale.z) * .75f;
                candidate.maximum.x = radius;
            }
            candidates.push_back(std::move(candidate));
        }
        travelled += segment.maxDistance;
    }
    hoverState = {};
    if (geometryPending) return nullptr;
    hoverState.signature = signature;
    hoverState.pending = Engine::Threading::JobScheduler::Get().Submit(
        [candidates = std::move(candidates)]() {
            Engine::Core::Object* best = nullptr;
            float nearest = FLT_MAX;
            for (const auto& candidate : candidates)
            {
                const float distance = candidate.Intersect();
                if (distance < nearest)
                { nearest = distance; best = candidate.object; }
            }
            return best;
        });
    return nullptr;
}

bool ScenePlacementAndPicking::FindPrefabPlacement(
    const Engine::Scene::Scene& scene, const Engine::Core::Object* prefabPreview,
    const EditorUiVec2& mousePos, const EditorUiVec2& viewportSize,
    glm::vec3& placement)
{
    glm::vec3 rayOrigin{};
    glm::vec3 rayDirection{};
    if (!BuildViewportRay(scene, mousePos, viewportSize, rayOrigin, rayDirection))
        return false;

    UpdateSpatialCache(scene);
    float bestDistance = FLT_MAX;
    bool found = false;
    for (uint32_t candidate : spatialCache.index.QueryRay(
        rayOrigin, rayDirection, FLT_MAX))
    {
        Engine::Core::Object* object = spatialCache.objects[candidate];
        if (!object || !object->IsEnabledInHierarchy() ||
            !object->GetComponent<Engine::Components::Mesh>() ||
            IsInObjectHierarchy(object, prefabPreview))
        {
            continue;
        }

        Engine::Components::Mesh* mesh =
            object->GetComponent<Engine::Components::Mesh>();
        float distance = 0.f;
        if (mesh && IntersectMeshBounds(
            *object, *mesh, rayOrigin, rayDirection, distance) &&
            distance < bestDistance)
        {
            bestDistance = distance;
            placement = rayOrigin + rayDirection * distance;
            found = true;
        }
    }

    if (scene.IsEditorMode2D())
    {
        if (std::abs(rayDirection.z) <= 0.000001f)
            return found;
        const float canvasDistance = -rayOrigin.z / rayDirection.z;
        if (canvasDistance > 0.f && canvasDistance < bestDistance)
        {
            placement = rayOrigin + rayDirection * canvasDistance;
            placement.z = 0.f;
            return true;
        }
        return found;
    }

    if (std::abs(rayDirection.y) > 0.000001f)
    {
        const float gridDistance = -rayOrigin.y / rayDirection.y;
        if (gridDistance > 0.f && gridDistance < bestDistance)
        {
            placement = rayOrigin + rayDirection * gridDistance;
            placement.y = 0.f;
            found = true;
        }
    }
    return found;
}

Engine::Core::Object* ScenePlacementAndPicking::PickBoneInViewport(
    const Engine::Scene::Scene& scene, const EditorUiVec2& mousePos,
    const EditorUiVec2& viewportSize)
{
    if (viewportSize.x <= 1.f || viewportSize.y <= 1.f) return nullptr;
    const auto* camera = scene.editorCamera.GetComponent<
        Engine::Components::Camera>();
    if (!camera) return nullptr;
    const glm::mat4 viewProjection = camera->GetProjectionMatrix(
        viewportSize.x / viewportSize.y) * camera->GetViewMatrix();
    Engine::Core::Object* closest = nullptr;
    float closestDepth = FLT_MAX;
    constexpr float radiusSquared = 14.f * 14.f;
    for (const auto& entry : scene.GetObjects())
    {
        auto* object = entry.get();
        if (!object || !object->IsEnabledInHierarchy() ||
            !object->GetComponent<Engine::Components::AnimationBone>())
            continue;
        const glm::vec4 clip = viewProjection * glm::vec4(
            object->transform.GetWorldPosition(), 1.f);
        if (clip.w <= 0.f) continue;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.z < 0.f || ndc.z > 1.f) continue;
        const float x = (ndc.x * .5f + .5f) * viewportSize.x;
        const float y = (.5f - ndc.y * .5f) * viewportSize.y;
        const float dx = x - mousePos.x, dy = y - mousePos.y;
        if (dx * dx + dy * dy <= radiusSquared && ndc.z < closestDepth)
        {
            closest = object;
            closestDepth = ndc.z;
        }
    }
    return closest;
}

Engine::Core::Object* ScenePlacementAndPicking::PickSkinnedBoneInViewport(
    const Engine::Scene::Scene& scene, const EditorUiVec2& mousePos,
    const EditorUiVec2& viewportSize)
{
    glm::vec3 rayOrigin{}, rayDirection{};
    if (!BuildViewportRay(scene, mousePos, viewportSize,
            rayOrigin, rayDirection)) return nullptr;
    Engine::Core::Object* picked = nullptr;
    float closest = FLT_MAX;
    for (const auto& entry : scene.GetObjects())
    {
        auto* object = entry.get();
        if (!object || !object->IsEnabledInHierarchy()) continue;
        auto* mesh = object->GetComponent<Engine::Components::Mesh>();
        auto* skin = object->GetComponent<Engine::Components::SkinnedMesh>();
        if (!mesh || !skin || !mesh->IsEditorVisible()) continue;
        auto* skeleton = skin->ResolveSkeleton();
        if (!skeleton) continue;
        const auto& joints = skeleton->ResolveJoints();
        const auto& source = mesh->GetVertices();
        if (source.empty()) continue;
        const auto* index = SkinnedIndex(*mesh, *skin, true);
        if (!index) continue;
        const glm::mat4 world = object->transform.GetWorldMatrix();
        if (std::abs(glm::determinant(world)) < 1e-8f) continue;
        const glm::mat4 inverseWorld = glm::inverse(world);
        const glm::vec3 origin = glm::vec3(inverseWorld *
            glm::vec4(rayOrigin, 1.f));
        const glm::vec3 direction = glm::vec3(inverseWorld *
            glm::vec4(rayDirection, 0.f));
        size_t triangle = 0;
        float t = 0.f, u = 0.f, v = 0.f;
        if (index->Intersect(origin, direction, t, &triangle, &u, &v))
        {
            const auto& ids = index->triangles[triangle].ids;
            const float worldDistance = glm::length(glm::vec3(world *
                glm::vec4(origin + direction * t, 1.f)) - rayOrigin);
            if (worldDistance >= closest) continue;
            float bestWeight = 0.f;
            unsigned bestJoint = 0;
            const float barycentric[3] = { 1.f - u - v, u, v };
            for (unsigned joint = 0; joint < joints.size(); ++joint)
            {
                if (!joints[joint]) continue;
                float weight = 0.f;
                for (int corner = 0; corner < 3; ++corner)
                {
                    const size_t vertex = ids[corner];
                    for (int slot = 0; slot < 4; ++slot)
                    {
                        if (source[vertex].weights0[slot] > 0.f &&
                            static_cast<unsigned>(source[vertex].joints0[slot]) == joint)
                            weight += barycentric[corner] * source[vertex].weights0[slot];
                        if (source[vertex].weights1[slot] > 0.f &&
                            static_cast<unsigned>(source[vertex].joints1[slot]) == joint)
                            weight += barycentric[corner] * source[vertex].weights1[slot];
                        if (source[vertex].weights0[slot] == 0.f &&
                            vertex < skin->joints.size() &&
                            vertex < skin->weights.size() &&
                            skin->joints[vertex][slot] == joint)
                            weight += barycentric[corner] * skin->weights[vertex][slot];
                    }
                }
                if (weight > bestWeight)
                { bestWeight = weight; bestJoint = joint; }
            }
            if (bestWeight > 0.f)
            { picked = joints[bestJoint]; closest = worldDistance; }
        }
    }
    return picked;
}

Engine::Core::Object* ScenePlacementAndPicking::PickObjectInViewport(
    const Engine::Scene::Scene& scene, const EditorUiVec2& mousePos,
    const EditorUiVec2& viewportSize, bool preserveMeshChild)
{
    if (viewportSize.x <= 1.f || viewportSize.y <= 1.f)
        return nullptr;

    const Engine::Components::Camera* cam =
        scene.editorCamera.GetComponent<Engine::Components::Camera>();
    if (!cam)
        return nullptr;

    const glm::mat4 view = cam->GetViewMatrix();
    const glm::mat4 proj = cam->GetProjectionMatrix(viewportSize.x / viewportSize.y);
    const glm::mat4 invVP = glm::inverse(proj * view);

    const float ndcX = (2.f * mousePos.x / viewportSize.x) - 1.f;
    const float ndcY = 1.f - (2.f * mousePos.y / viewportSize.y);

    glm::vec4 nearH = invVP * glm::vec4(ndcX, ndcY, 0.f, 1.f);
    glm::vec4 farH = invVP * glm::vec4(ndcX, ndcY, 1.f, 1.f);
    if (nearH.w == 0.f || farH.w == 0.f)
        return nullptr;

    const glm::vec3 rayOrigin = glm::vec3(nearH) / nearH.w;
    const glm::vec3 rayTarget = glm::vec3(farH) / farH.w;
    const glm::vec3 rayDir = glm::normalize(rayTarget - rayOrigin);
    UpdateSpatialCache(scene);

    if (scene.IsEditorMode2D())
    {
        Engine::Core::Object* frontmost = nullptr;
        int frontLayer = std::numeric_limits<int>::min();
        float frontZ = -FLT_MAX;
        for (const auto& objPtr : scene.GetObjects())
        {
            Engine::Core::Object* obj = objPtr.get();
            Engine::Components::Sprite* sprite = obj && obj->IsEnabledInHierarchy()
                ? obj->GetComponent<Engine::Components::Sprite>() : nullptr;
            if (!sprite || std::abs(rayDir.z) < 0.000001f)
                continue;

            glm::mat4 world = obj->transform.GetWorldMatrix();
            world[3].z = 0.f;
            const glm::vec2 spriteSize = sprite->GetWorldSize();
            world = world * glm::scale(glm::mat4(1.f), glm::vec3(spriteSize, 1.f));
            const glm::mat4 inverseWorld = glm::inverse(world);
            const glm::vec3 localOrigin = glm::vec3(
                inverseWorld * glm::vec4(rayOrigin, 1.f));
            const glm::vec3 localDirection = glm::vec3(
                inverseWorld * glm::vec4(rayDir, 0.f));
            if (std::abs(localDirection.z) < 0.000001f)
                continue;

            const float distance = -localOrigin.z / localDirection.z;
            if (distance < 0.f)
                continue;

            const glm::vec3 hit = localOrigin + localDirection * distance;
            if (std::abs(hit.x) > 0.5f || std::abs(hit.y) > 0.5f)
                continue;

            const float z = obj->transform.GetWorldPosition().z;
            if (!frontmost || sprite->sortingLayer > frontLayer ||
                (sprite->sortingLayer == frontLayer && z >= frontZ))
            {
                frontmost = obj;
                frontLayer = sprite->sortingLayer;
                frontZ = z;
            }
        }

        if (frontmost)
            return frontmost;

        Engine::Core::Object* closestMesh = nullptr;
        float closestDistance = FLT_MAX;
        for (uint32_t candidate : spatialCache.index.QueryRay(
            rayOrigin, rayDir, FLT_MAX))
        {
            Engine::Core::Object* obj = spatialCache.objects[candidate];
            Engine::Components::Mesh* mesh = obj && obj->IsEnabledInHierarchy()
                ? obj->GetComponent<Engine::Components::Mesh>() : nullptr;
            float distance = 0.f;
            if (mesh && IntersectMeshBounds(*obj, *mesh, rayOrigin,
                rayDir, distance) && distance < closestDistance)
            {
                closestDistance = distance;
                closestMesh = obj;
            }
        }
        return closestMesh;
    }

    Engine::Core::Object* best = nullptr;
    float bestDistance = FLT_MAX;
    float travelledDistance = 0.f;
    const std::vector<Engine::Scene::Scene::PortalRaySegment> segments =
        scene.TracePortalRay({ rayOrigin, rayDir }, 10000.f);
    for (const Engine::Scene::Scene::PortalRaySegment& segment : segments)
    {
        const auto candidates = spatialCache.index.QueryRay(segment.ray.origin,
            segment.ray.direction, segment.maxDistance);
        for (uint32_t candidate : candidates)
        {
            Engine::Core::Object* obj = spatialCache.objects[candidate];
            Engine::Components::Mesh* mesh = obj && obj->IsEnabledInHierarchy()
                ? obj->GetComponent<Engine::Components::Mesh>() : nullptr;
            Engine::Components::Sprite* sprite = obj && obj->IsEnabledInHierarchy()
                ? obj->GetComponent<Engine::Components::Sprite>() : nullptr;
            if (!obj || !obj->IsEnabledInHierarchy() ||
                (!mesh && !sprite) || (mesh && !mesh->IsEditorVisible()))
            {
                continue;
            }
            if (!Engine::Scene::Scene::IsObjectInSpatialRegion(
                obj, segment.contentScopeRoot))
            {
                continue;
            }

            float distance = 0.f;
            if (mesh && mesh->HasBounds())
            {
                const auto* skinned =
                    obj->GetComponent<Engine::Components::SkinnedMesh>();
                const bool hit = skinned
                    ? IntersectSkinnedMesh(*obj, *mesh, *skinned,
                        segment.ray.origin, segment.ray.direction, distance)
                    : IntersectStaticMesh(*obj, *mesh, segment.ray.origin,
                        segment.ray.direction, distance);
                if (hit && distance < segment.maxDistance &&
                    travelledDistance + distance < bestDistance)
                {
                    bestDistance = travelledDistance + distance;
                    best = !preserveMeshChild && skinned && obj->GetPrefabInstanceRoot()
                        ? obj->GetPrefabInstanceRoot() : obj;
                }
                continue;
            }

            // Sprites and meshes without CPU bounds retain the coarse sphere
            // fallback, but test it in the current portal chart segment.
            const glm::vec3 center = obj->transform.GetWorldPosition();
            const float radius = glm::max(
                glm::max(obj->transform.scale.x, obj->transform.scale.y),
                obj->transform.scale.z) * 0.75f;
            const glm::vec3 oc = segment.ray.origin - center;
            const float a = glm::dot(segment.ray.direction,
                segment.ray.direction);
            const float b = 2.f * glm::dot(oc, segment.ray.direction);
            const float c = glm::dot(oc, oc) - (radius * radius);
            const float discriminant = b * b - 4.f * a * c;
            if (discriminant < 0.f)
                continue;

            const float sqrtDiscriminant = std::sqrt(discriminant);
            const float t0 = (-b - sqrtDiscriminant) / (2.f * a);
            const float t1 = (-b + sqrtDiscriminant) / (2.f * a);
            distance = t0 > 0.f ? t0 : t1;
            if (distance > 0.f && distance < segment.maxDistance &&
                travelledDistance + distance < bestDistance)
            {
                bestDistance = travelledDistance + distance;
                best = obj;
            }
        }
        travelledDistance += segment.maxDistance;
    }

    return best;
}
}
