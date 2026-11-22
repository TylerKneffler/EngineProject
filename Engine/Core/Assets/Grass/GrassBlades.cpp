#include "Grass/GrassBlades.h"

#include "Core/Components/Materials/Material.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Rendering/GpuGrassBlades.h"
#include "Core/Scene/Object.h"
#include "Core/Scene/Scene.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace
{
// GrassBlades.hlsl fieldData layout.
constexpr size_t HeaderFloats = 64, CellTableFloats = 256, MaximumCells = 64;
// The blade template: SEGMENTS rows of two vertices (uv.x 0 and 1, uv.y
// root to tip) closing to a single tip vertex.
constexpr int Segments = 4;

std::shared_ptr<Engine::Components::Mesh> BladeTemplate(Engine::Graphics::IGraphicsProvider* provider)
{
    using Vertex = Engine::Components::Mesh::Vertex;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    const auto vertex = [&](float u, float v) {
        Vertex out{};
        out.pos[0] = u - .5f; out.pos[1] = v; out.pos[2] = 0.f;
        out.normal[2] = 1.f;
        out.uv[0] = u; out.uv[1] = v;
        out.tangent[0] = 1.f; out.tangent[3] = 1.f;
        vertices.push_back(out);
    };
    for (int row = 0; row < Segments; ++row)
    {
        const float v = float(row) / Segments;
        vertex(0.f, v); vertex(1.f, v);
    }
    vertex(.5f, 1.f);
    for (int row = 0; row + 1 < Segments; ++row)
    {
        const uint32_t a = row * 2, b = a + 1, c = a + 3, d = a + 2;
        indices.insert(indices.end(), {a, b, c, a, c, d});
    }
    const uint32_t last = (Segments - 1) * 2, tip = Segments * 2;
    indices.insert(indices.end(), {last, last + 1, tip});
    auto mesh = std::make_shared<Engine::Components::Mesh>();
    mesh->SetIndexedGeometry(std::move(vertices), std::move(indices));
    mesh->CreateBuffer(provider->GetBufferFactory());
    return mesh;
}
}

bool GrassBlades::Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("ENGINE_GPU_GRASS");
        return value && value[0] == '1';
    }();
    return enabled;
}

std::shared_ptr<GrassBlades> GrassBlades::For(Engine::Scene::Scene& scene)
{
    auto registry = std::static_pointer_cast<GrassBlades>(scene.grassBlades);
    if (!registry)
    {
        registry = std::make_shared<GrassBlades>();
        scene.grassBlades = registry;
    }
    return registry;
}

void GrassBlades::SetCell(const void* key, glm::vec3 origin, int samples, float spacing,
    std::vector<float> heights, std::vector<float> paint)
{
    auto& cell = m_cells[key];
    cell.origin = origin; cell.samples = samples; cell.spacing = spacing;
    cell.heights = std::move(heights); cell.paint = std::move(paint);
    m_dirty = true;
}

void GrassBlades::RemoveCell(const void* key)
{
    if (m_cells.erase(key)) m_dirty = true;
}

void GrassBlades::SetSettings(const Settings& settings)
{
    if (m_hasSettings) return; // the first meadow field's settings hold
    m_settings = settings;
    m_hasSettings = true;
    m_dirty = true;
}

void GrassBlades::Update(Engine::Scene::Scene& scene, Engine::Graphics::IGraphicsProvider* provider)
{
    if (!provider) return;
    if (!m_field) m_field = std::make_shared<Engine::Rendering::GpuBladeField>();
    if (m_dirty)
    {
        m_dirty = false;
        const auto& s = m_settings;
        std::vector<float> data(HeaderFloats + CellTableFloats, 0.f);
        const size_t cellCount = std::min(m_cells.size(), MaximumCells);
        const int samples = m_cells.empty() ? 33 : m_cells.begin()->second.samples;
        const float spacing = m_cells.empty() ? 4.f : m_cells.begin()->second.spacing;
        const float patch = 2.f;
        const float header[] = {float(cellCount), float(samples), spacing, patch,
            s.shortDensity, s.tallDensity, s.fieldSize, s.fieldCoverage, s.shortUnderTall, float(s.seed),
            s.keepDistance, s.reach - s.fadeBand, s.clumpSize,
            std::ceil((s.shortDensity + s.tallDensity) * patch * patch),
            s.shortHeight[0], s.shortHeight[1], s.tallHeight[0], s.tallHeight[1],
            s.shortWidth, s.tallWidth, s.dryShare, s.paint[0], s.paint[1]};
        std::copy(std::begin(header), std::end(header), data.begin());
        size_t index = 0;
        for (const auto& [key, cell] : m_cells)
        {
            if (index >= cellCount) break;
            if (cell.samples != samples || cell.heights.size() != size_t(samples * samples) ||
                cell.paint.size() != cell.heights.size()) continue;
            const size_t entry = HeaderFloats + index * 4;
            data[entry] = cell.origin.x; data[entry + 1] = cell.origin.z;
            data[entry + 2] = cell.origin.y; data[entry + 3] = float(data.size());
            data.insert(data.end(), cell.heights.begin(), cell.heights.end());
            data.insert(data.end(), cell.paint.begin(), cell.paint.end());
            ++index;
        }
        data[0] = float(index);
        if (m_field->data) m_retired.push_back(m_field->data);
        while (m_retired.size() > 4) m_retired.pop_front();
        std::shared_ptr<Engine::Graphics::IGraphicsBuffer> buffer = provider->GetBufferFactory()->CreateBuffer(
            Engine::Graphics::IGraphicsBuffer::Usage::ShaderResource,
            Engine::Graphics::IGraphicsBuffer::AccessMode::Upload,
            data.size() * sizeof(float), data.data(), sizeof(float));
        m_field->data = std::move(buffer);
        m_field->capacity = s.capacity;
        m_field->patchSize = patch;
        m_field->reach = s.reach;
    }
    auto* carrier = m_carrierId ? scene.FindObjectById(m_carrierId) : nullptr;
    if (!carrier)
    {
        carrier = scene.AddObject("GPU grass blades");
        carrier->runtimeOnly = true;
        carrier->layer = m_settings.projectLayer;
        auto* mesh = carrier->AddComponent<Engine::Components::Mesh>();
        auto instances = std::make_shared<Engine::Components::Mesh::InstanceData>();
        Engine::Components::Mesh::InstanceTransform identity{};
        identity.rows[0] = {1, 0, 0, 0}; identity.rows[1] = {0, 1, 0, 0}; identity.rows[2] = {0, 0, 1, 0};
        instances->transforms.push_back(identity);
        instances->placementIds.push_back(1);
        auto source = BladeTemplate(provider);
        const uint32_t indexCount = source->GetIndexCount();
        // Bounds everywhere: blades are culled per patch on the GPU.
        mesh->SetInstancedGeometry(source, instances, {{0, indexCount, 0, 1}},
            glm::vec3(-1.e6f), glm::vec3(1.e6f));
        mesh->gpuBladeField = m_field;
        mesh->foliageWindStrength = std::clamp(m_settings.windStrength, 0.f, 1.f);
        mesh->usesGrassShadowBudget = true;
        auto* material = carrier->AddComponent<Engine::Components::Material>();
        material->diffuseColor = {1, 1, 1};
        material->useVertexColors = true;
        material->shader = "Assets/Shaders/GrassBlades.hlsl";
        material->roughnessFactor = 1;
        material->metallicFactor = 0;
        material->doubleSided = true;
        material->castsShadows = false;
        m_carrierId = scene.GetObjectId(carrier);
    }
    carrier->enabled = m_field->data && !m_cells.empty();
}
