#include "Core/Components/Terrain/TerrainField.h"
#include "Core/Components/Terrain/TerrainSurfaceSample.h"

#include <algorithm>
#include <cmath>

namespace Engine::Components
{
namespace
{
// Same blends as TerrainVolumeBuild.cpp, so the field and the meshes agree.
float SmoothMax(float a, float b, float k)
{
    if (k <= 0.f) return std::max(a, b);
    const float h = std::clamp(.5f + .5f * (a - b) / k, 0.f, 1.f);
    return b + (a - b) * h + k * h * (1.f - h);
}
float SmoothMin(float a, float b, float k) { return -SmoothMax(-a, -b, k); }

bool Contains(const glm::vec3& low, const glm::vec3& high, const glm::vec3& p)
{
    return p.x >= low.x && p.y >= low.y && p.z >= low.z &&
        p.x <= high.x && p.y <= high.y && p.z <= high.z;
}
bool ContainsColumn(const glm::vec3& low, const glm::vec3& high, float x, float z)
{
    return x >= low.x && z >= low.z && x <= high.x && z <= high.z;
}
// Below this a vertical march would skip features; edits are rounded shapes
// a metre or more across.
constexpr float MarchStep = .25f;
constexpr int BisectSteps = 14;
}

TerrainField::TerrainField(TerrainFieldLayout layout, HeightSeed seed, EditSource edits,
    size_t cachedCells)
    : m_layout(layout), m_seed(std::move(seed)), m_editSource(std::move(edits)),
      m_capacity(std::max<size_t>(16, cachedCells))
{
}

TerrainField::Cell TerrainField::CellAt(float x, float z) const
{
    return {static_cast<int>(std::floor(x / m_layout.SpanX())),
        static_cast<int>(std::floor(z / m_layout.SpanZ()))};
}

std::shared_ptr<const std::vector<TerrainVolumeEdit>> TerrainField::OwnEdits(Cell cell) const
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (const auto found = m_ownEdits.find(cell); found != m_ownEdits.end())
            return found->second;
    }
    std::shared_ptr<const std::vector<TerrainVolumeEdit>> loaded;
    if (m_editSource)
    {
        std::vector<TerrainVolumeEdit> edits;
        if (m_editSource(cell, edits) && !edits.empty())
            loaded = std::make_shared<const std::vector<TerrainVolumeEdit>>(std::move(edits));
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ownEdits.try_emplace(cell, loaded).first->second;
}

std::shared_ptr<const TerrainField::CellData> TerrainField::Get(Cell cell) const
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (const auto found = m_cells.find(cell); found != m_cells.end())
        {
            m_order.splice(m_order.begin(), m_order, found->second.position);
            return found->second.data;
        }
    }
    // Built outside the lock: the seed may read files.
    auto data = std::make_shared<CellData>();
    std::vector<float> heights;
    const size_t expected = static_cast<size_t>(m_layout.cellsX + 1) * (m_layout.cellsY + 1);
    if (m_seed && m_seed(cell, heights) && heights.size() == expected)
    {
        data->valid = true;
        const auto [low, high] = std::minmax_element(heights.begin(), heights.end());
        data->lowest = *low;
        data->highest = *high;
        data->heights = std::make_shared<const std::vector<float>>(std::move(heights));
        const float spanX = m_layout.SpanX(), spanZ = m_layout.SpanZ();
        // Own edits first, then the neighbours' (FeedVolumeNeighbours order:
        // blends do not commute).
        const auto append = [&](int dx, int dz) {
            const auto edits = OwnEdits({cell.first + dx, cell.second + dz});
            if (!edits) return;
            const glm::vec3 offset(dx * spanX, 0.f, dz * spanZ);
            for (const auto& edit : *edits)
            {
                const auto shifted = (dx || dz) ? edit.Shifted(offset) : edit;
                const glm::vec3 low = shifted.Minimum(), high = shifted.Maximum();
                if (high.x < 0.f || high.z < 0.f || low.x > spanX || low.z > spanZ) continue;
                data->edits.push_back(shifted);
                data->editLow = glm::min(data->editLow, low);
                data->editHigh = glm::max(data->editHigh, high);
            }
        };
        append(0, 0);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
                if (dx || dz) append(dx, dz);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (const auto found = m_cells.find(cell); found != m_cells.end())
    {
        m_order.splice(m_order.begin(), m_order, found->second.position);
        return found->second.data;
    }
    m_order.push_front(cell);
    m_cells.emplace(cell, Entry{data, m_order.begin()});
    while (m_cells.size() > m_capacity)
    {
        m_cells.erase(m_order.back());
        m_order.pop_back();
    }
    return data;
}

const TerrainField::CellData* TerrainField::Fetch(Cursor& cursor, Cell cell) const
{
    if (!cursor.data || cursor.cell != cell)
    {
        cursor.data = Get(cell);
        cursor.cell = cell;
    }
    return cursor.data.get();
}

float TerrainField::DensityAt(Cursor& cursor, const glm::vec3& position) const
{
    const Cell cell = CellAt(position.x, position.z);
    const CellData* data = Fetch(cursor, cell);
    if (!data || !data->valid) return -1.f;
    const glm::vec3 local(position.x - cell.first * m_layout.SpanX(), position.y,
        position.z - cell.second * m_layout.SpanZ());
    float height = 0.f;
    glm::vec3 normal(0.f, 1.f, 0.f);
    SampleTerrainSurface(*data->heights, m_layout.cellsX, m_layout.cellsY,
        m_layout.cellWidth, m_layout.cellDepth, local.x, local.z, height, &normal);
    float density = (height - local.y) * std::max(normal.y, .05f);
    if (!data->edits.empty() && Contains(data->editLow, data->editHigh, local))
        for (const auto& edit : data->edits)
        {
            if (!Contains(edit.Minimum(), edit.Maximum(), local)) continue;
            const float shape = edit.Distance(local);
            density = edit.subtract ? SmoothMin(density, shape, edit.blend)
                                    : SmoothMax(density, -shape, edit.blend);
        }
    return density;
}

glm::vec3 TerrainField::NormalAt(Cursor& cursor, const glm::vec3& position) const
{
    const Cell cell = CellAt(position.x, position.z);
    const CellData* data = Fetch(cursor, cell);
    if (!data || !data->valid) return {0.f, 1.f, 0.f};
    const glm::vec3 local(position.x - cell.first * m_layout.SpanX(), position.y,
        position.z - cell.second * m_layout.SpanZ());
    // Plain ground: the height grid's own triangle normal.
    if (data->edits.empty() || !Contains(data->editLow, data->editHigh, local))
    {
        float height = 0.f;
        glm::vec3 normal(0.f, 1.f, 0.f);
        SampleTerrainSurface(*data->heights, m_layout.cellsX, m_layout.cellsY,
            m_layout.cellWidth, m_layout.cellDepth, local.x, local.z, height, &normal);
        return normal;
    }
    constexpr float e = .05f;
    const glm::vec3 gradient(
        DensityAt(cursor, position + glm::vec3(e, 0.f, 0.f)) - DensityAt(cursor, position - glm::vec3(e, 0.f, 0.f)),
        DensityAt(cursor, position + glm::vec3(0.f, e, 0.f)) - DensityAt(cursor, position - glm::vec3(0.f, e, 0.f)),
        DensityAt(cursor, position + glm::vec3(0.f, 0.f, e)) - DensityAt(cursor, position - glm::vec3(0.f, 0.f, e)));
    const float length = glm::length(gradient);
    // Density grows into the ground; the surface faces the other way.
    return length > 1e-6f ? -gradient / length : glm::vec3(0.f, 1.f, 0.f);
}

float TerrainField::Density(const glm::vec3& position) const
{
    Cursor cursor;
    return DensityAt(cursor, position);
}

glm::vec3 TerrainField::Normal(const glm::vec3& position) const
{
    Cursor cursor;
    return NormalAt(cursor, position);
}

std::optional<TerrainField::Surface> TerrainField::SurfaceAt(float x, float z, float from) const
{
    Cursor cursor;
    const Cell cell = CellAt(x, z);
    const CellData* data = Fetch(cursor, cell);
    if (!data || !data->valid) return std::nullopt;
    const float localX = x - cell.first * m_layout.SpanX();
    const float localZ = z - cell.second * m_layout.SpanZ();
    float height = 0.f;
    glm::vec3 normal(0.f, 1.f, 0.f);
    SampleTerrainSurface(*data->heights, m_layout.cellsX, m_layout.cellsY,
        m_layout.cellWidth, m_layout.cellDepth, localX, localZ, height, &normal);
    // The edits on this vertical line, if any.
    float editLow = std::numeric_limits<float>::max(), editHigh = std::numeric_limits<float>::lowest();
    for (const auto& edit : data->edits)
    {
        const glm::vec3 low = edit.Minimum(), high = edit.Maximum();
        if (!ContainsColumn(low, high, localX, localZ)) continue;
        editLow = std::min(editLow, low.y);
        editHigh = std::max(editHigh, high.y);
    }
    if (editLow > editHigh)
    {
        // Plain height field: one surface, solid below it.
        if (from < height) return std::nullopt;
        return Surface{height, normal};
    }
    // March down from the top of everything on the line (or `from`), past
    // the lowest edit and the seed ground; below that it is solid ground.
    const float top = std::min(from, std::max(height, editHigh) + MarchStep);
    const float bottom = std::min(height, editLow) - MarchStep;
    float yAbove = top;
    float dAbove = DensityAt(cursor, {x, yAbove, z});
    for (float y = top - MarchStep; y >= bottom - MarchStep; y -= MarchStep)
    {
        const float d = DensityAt(cursor, {x, y, z});
        if (dAbove < 0.f && d >= 0.f)
        {
            float air = yAbove, ground = y;
            for (int i = 0; i < BisectSteps; ++i)
            {
                const float middle = .5f * (air + ground);
                (DensityAt(cursor, {x, middle, z}) < 0.f ? air : ground) = middle;
            }
            const float surface = .5f * (air + ground);
            return Surface{surface, NormalAt(cursor, {x, surface, z})};
        }
        yAbove = y;
        dAbove = d;
    }
    return std::nullopt;
}

std::optional<TerrainField::Hit> TerrainField::Raycast(const glm::vec3& origin,
    const glm::vec3& direction, float maxDistance) const
{
    const float length = glm::length(direction);
    if (length <= 1e-8f || maxDistance <= 0.f) return std::nullopt;
    const glm::vec3 unit = direction / length;
    Cursor cursor;
    float t = 0.f;
    float d = DensityAt(cursor, origin);
    if (d >= 0.f) return Hit{0.f, origin, NormalAt(cursor, origin)};
    // Density is about the distance to the surface (exact for plain ground,
    // a bound for rounded edits), so steps follow it, kept short enough not
    // to cross a thin edit.
    for (int i = 0; i < 8192 && t < maxDistance; ++i)
    {
        const float step = std::clamp(-d * .8f, .05f, 4.f);
        const float next = std::min(t + step, maxDistance);
        const float dNext = DensityAt(cursor, origin + unit * next);
        if (dNext >= 0.f)
        {
            float air = t, ground = next;
            for (int k = 0; k < BisectSteps; ++k)
            {
                const float middle = .5f * (air + ground);
                (DensityAt(cursor, origin + unit * middle) < 0.f ? air : ground) = middle;
            }
            const float hit = .5f * (air + ground);
            const glm::vec3 position = origin + unit * hit;
            return Hit{hit, position, NormalAt(cursor, position)};
        }
        t = next;
        d = dNext;
        if (next >= maxDistance) break;
    }
    return std::nullopt;
}

bool TerrainField::HasEdits(Cell cell) const
{
    const auto data = Get(cell);
    return data && !data->edits.empty();
}

std::shared_ptr<const std::vector<float>> TerrainField::SeedHeights(Cell cell) const
{
    const auto data = Get(cell);
    return data && data->valid ? data->heights : nullptr;
}

void TerrainField::Invalidate(Cell cell)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ownEdits.erase(cell);
    // A cell's edits reach its neighbours.
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
            if (const auto found = m_cells.find({cell.first + dx, cell.second + dz});
                found != m_cells.end())
            {
                m_order.erase(found->second.position);
                m_cells.erase(found);
            }
}

void TerrainField::InvalidateAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cells.clear();
    m_order.clear();
    m_ownEdits.clear();
}
}
