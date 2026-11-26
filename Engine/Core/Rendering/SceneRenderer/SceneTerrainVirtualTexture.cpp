#include "Core/Rendering/SceneRenderer/SceneTerrainVirtualTexture.h"

#include "Core/Rendering/SceneRenderer/SceneRendererDrawPreparation.h"
#include "Core/Components/Mesh/Mesh.h"
#include "Core/Graphics/DepthConvention.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include "Core/Graphics/IGraphicsContext.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Graphics/IGraphicsTexture.h"
#include "Core/Rendering/RenderUtilities.h"
#include "Core/Rendering/RenderSpatialIndex.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Engine::Rendering
{
namespace
{
// One fill draw's constants: Object.hlsl DrawConstants (DrawCBData, then
// virtualTextureRect and virtualTextureDepth), in a 256-byte record.
struct FillRecord
{
    DrawCBData draw;
    glm::vec4 rect;
    glm::vec4 depth;
    uint8_t padding[256 - sizeof(DrawCBData) - 2 * sizeof(glm::vec4)];
};
static_assert(sizeof(FillRecord) == 256);

// World texel rectangle [x0, x1) x [z0, z1) of one level.
struct TexelRect { int64_t x0, z0, x1, z1; };

int64_t FloorMod(int64_t value, int64_t modulus)
{
    const int64_t r = value % modulus;
    return r < 0 ? r + modulus : r;
}
}

bool TerrainVirtualTexture::Fills(const PreparedDraw& draw)
{
    // Layered terrain surfaces with the full mesh vertex (cells and volume
    // chunks); distant tiles (compact vertices) keep their own path.
    return draw.terrainCell && !draw.terrain && !draw.blended && draw.mesh &&
        draw.vertexBuffer && draw.mesh->HasBounds() &&
        (static_cast<uint32_t>(draw.objectData.materialParams.w) & 2048u) != 0u;
}

bool TerrainVirtualTexture::Configure(Graphics::IGraphicsProvider* provider, const Settings& requested)
{
    Settings settings = requested;
    settings.levels = std::clamp(settings.levels, 1u, 12u);
    settings.resolution = std::clamp(settings.resolution, 64u, 4096u);
    settings.texelSize = std::clamp(settings.texelSize, 0.0005f, 4.f);
    if (settings == m_settings && (m_active || !settings.enabled)) return m_active;
    m_settings = settings;
    m_active = false;
    m_albedo.reset(); m_normal.reset(); m_mean.reset(); m_depth.reset();
    m_levels.clear();
    if (!settings.enabled || !provider || !provider->GetTextureFactory()) return false;
    auto* textures = provider->GetTextureFactory();
    m_albedo = textures->CreateRenderTargetArray(settings.resolution, settings.resolution, settings.levels, true);
    m_normal = textures->CreateRenderTargetArray(settings.resolution, settings.resolution, settings.levels, false);
    m_mean = textures->CreateRenderTargetArray(settings.resolution, settings.resolution, settings.levels, true);
    m_depth = textures->CreateDepthTexture2D(settings.resolution, settings.resolution);
    if (!m_albedo || !m_normal || !m_mean || !m_depth)
    {
        m_albedo.reset(); m_normal.reset(); m_mean.reset(); m_depth.reset();
        return false;
    }
    m_levels.resize(settings.levels);
    m_active = true;
    return true;
}

glm::vec4 TerrainVirtualTexture::ShaderConstants() const
{
    return m_active ? glm::vec4(m_settings.texelSize, float(m_settings.levels),
        float(m_settings.resolution), 1.f) : glm::vec4(0.f);
}

std::array<const Graphics::IGraphicsTexture*, 3> TerrainVirtualTexture::Textures() const
{
    return {m_albedo.get(), m_normal.get(), m_mean.get()};
}

void TerrainVirtualTexture::Invalidate()
{
    for (auto& level : m_levels) level.valid = false;
    m_previousSources.clear();
}

void TerrainVirtualTexture::Record(Graphics::IGraphicsContext* context, Graphics::IGraphicsProvider* provider,
    const std::vector<PreparedDraw>& draws, const glm::vec3& camera,
    Graphics::IPipelineState* pipeline, uint64_t frameSlot,
    const std::function<void(const PreparedDraw&)>& submit)
{
    m_statistics = {};
    if (!m_active || !context || !provider || !pipeline) return;
    const int64_t texels = m_settings.resolution;
    const int64_t half = texels / 2;
    const int64_t step = std::max<int64_t>(texels / 16, 1);

    // The terrain draws and their world X/Z bounds.
    struct Source { const PreparedDraw* draw; glm::vec2 minimum, maximum; };
    std::vector<Source> sources;
    for (const auto& draw : draws)
    {
        if (!Fills(draw)) continue;
        const auto bounds = TransformBounds(draw.mesh->GetBoundsMin(), draw.mesh->GetBoundsMax(),
            draw.objectData.world);
        sources.push_back({&draw, {bounds.minimum.x, bounds.minimum.z}, {bounds.maximum.x, bounds.maximum.z}});
    }
    if (sources.empty()) return;

    // Per level, the world texel rectangles the window gained.
    // clear: the rectangle held nothing valid (a strip the window gained);
    // refills of terrain that appeared only clear depth and draw over it,
    // so neighbouring ground drawn earlier (now off screen) is kept.
    struct Work { uint32_t level; TexelRect rect; bool clear = true; };
    std::vector<Work> work;
    for (uint32_t level = 0; level < m_levels.size(); ++level)
    {
        const double texel = double(m_settings.texelSize) * std::ldexp(1.0, int(level));
        const glm::ivec2 target(
            int(std::llround(std::floor(double(camera.x) / texel / double(step) + 0.5)) * step),
            int(std::llround(std::floor(double(camera.z) / texel / double(step) + 0.5)) * step));
        auto& state = m_levels[level];
        const TexelRect next{int64_t(target.x) - half, int64_t(target.y) - half,
            int64_t(target.x) + half, int64_t(target.y) + half};
        if (!state.valid || std::abs(int64_t(target.x) - state.center.x) >= texels ||
            std::abs(int64_t(target.y) - state.center.y) >= texels)
        {
            work.push_back({level, next});
            ++m_statistics.fullLevels;
        }
        else if (target != state.center)
        {
            const TexelRect old{int64_t(state.center.x) - half, int64_t(state.center.y) - half,
                int64_t(state.center.x) + half, int64_t(state.center.y) + half};
            // Columns that entered (full height of the new window).
            if (next.x1 > old.x1) work.push_back({level, {std::max(old.x1, next.x0), next.z0, next.x1, next.z1}});
            if (next.x0 < old.x0) work.push_back({level, {next.x0, next.z0, std::min(old.x0, next.x1), next.z1}});
            // Rows that entered, over the columns kept.
            const int64_t keptX0 = std::max(old.x0, next.x0), keptX1 = std::min(old.x1, next.x1);
            if (keptX0 < keptX1)
            {
                if (next.z1 > old.z1) work.push_back({level, {keptX0, std::max(old.z1, next.z0), keptX1, next.z1}});
                if (next.z0 < old.z0) work.push_back({level, {keptX0, next.z0, keptX1, std::min(old.z0, next.z1)}});
            }
        }
        state.center = target;
        state.valid = true;
    }
    // Terrain draws that were not drawn last frame (a chunk streamed in,
    // swapped level, or came into view: the fill only sees the camera's
    // draws) redraw their footprint in every level. Strips filled while
    // their terrain was missing or off screen stayed empty otherwise.
    std::unordered_set<const void*> current;
    current.reserve(sources.size());
    for (const auto& source : sources)
    {
        const void* key = source.draw->vertexBuffer;
        current.insert(key);
        if (m_previousSources.count(key)) continue;
        for (uint32_t level = 0; level < m_levels.size(); ++level)
        {
            const double texel = double(m_settings.texelSize) * std::ldexp(1.0, int(level));
            const auto& state = m_levels[level];
            const TexelRect window{int64_t(state.center.x) - half, int64_t(state.center.y) - half,
                int64_t(state.center.x) + half, int64_t(state.center.y) + half};
            const TexelRect area{
                std::max(window.x0, int64_t(std::floor(double(source.minimum.x) / texel))),
                std::max(window.z0, int64_t(std::floor(double(source.minimum.y) / texel))),
                std::min(window.x1, int64_t(std::ceil(double(source.maximum.x) / texel))),
                std::min(window.z1, int64_t(std::ceil(double(source.maximum.y) / texel)))};
            if (area.x0 < area.x1 && area.z0 < area.z1) work.push_back({level, area, false});
        }
        ++m_statistics.newSources;
    }
    m_previousSources = std::move(current);
    if (work.empty()) return;

    // Split each rectangle at the texture's wrap (toroidal addressing).
    struct Piece { uint32_t level; TexelRect world; Graphics::IGraphicsContext::ScissorRect texture; bool clear; };
    std::vector<Piece> pieces;
    for (const auto& item : work)
    {
        const auto split = [&](int64_t a, int64_t b, std::array<std::pair<int64_t, int64_t>, 2>& out) {
            const int64_t t = FloorMod(a, texels), length = b - a;
            if (t + length <= texels) { out[0] = {a, b}; return 1; }
            out[0] = {a, a + (texels - t)}; out[1] = {a + (texels - t), b}; return 2;
        };
        std::array<std::pair<int64_t, int64_t>, 2> xs{}, zs{};
        const int nx = split(item.rect.x0, item.rect.x1, xs), nz = split(item.rect.z0, item.rect.z1, zs);
        for (int i = 0; i < nx; ++i)
            for (int j = 0; j < nz; ++j)
            {
                const int64_t tx = FloorMod(xs[i].first, texels), tz = FloorMod(zs[j].first, texels);
                pieces.push_back({item.level, {xs[i].first, zs[j].first, xs[i].second, zs[j].second},
                    {int32_t(tx), int32_t(tz), int32_t(tx + (xs[i].second - xs[i].first)),
                     int32_t(tz + (zs[j].second - zs[j].first))}, item.clear});
            }
    }

    // Constants: one record per (piece, overlapping draw).
    struct Call { const PreparedDraw* draw; size_t record; };
    std::vector<std::vector<Call>> calls(pieces.size());
    size_t records = 0;
    for (size_t p = 0; p < pieces.size(); ++p)
    {
        const auto& piece = pieces[p];
        const float texel = m_settings.texelSize * std::ldexp(1.f, int(piece.level));
        const glm::vec2 minimum(float(piece.world.x0) * texel, float(piece.world.z0) * texel);
        const glm::vec2 maximum(float(piece.world.x1) * texel, float(piece.world.z1) * texel);
        for (const auto& source : sources)
            if (source.maximum.x >= minimum.x && source.minimum.x <= maximum.x &&
                source.maximum.y >= minimum.y && source.minimum.y <= maximum.y)
                calls[p].push_back({source.draw, records++});
    }
    if (!records) return;
    if (m_constants.size() <= frameSlot) m_constants.resize(size_t(frameSlot) + 1);
    auto& constants = m_constants[size_t(frameSlot)];
    const size_t bytes = records * sizeof(FillRecord);
    if (!constants.buffer || constants.capacity < bytes)
    {
        size_t capacity = 64 * sizeof(FillRecord);
        while (capacity < bytes) capacity *= 2;
        auto* factory = provider->GetBufferFactory();
        if (!factory) return;
        // Shared: packets that recorded the previous one keep it alive.
        constants.buffer = std::shared_ptr<Graphics::IGraphicsBuffer>(factory->CreateBuffer(
            Graphics::IGraphicsBuffer::Usage::ConstantBuffer, Graphics::IGraphicsBuffer::AccessMode::Upload,
            capacity, nullptr, sizeof(uint32_t) * 4u));
        constants.mapped = constants.buffer ? constants.buffer->Map() : nullptr;
        constants.capacity = constants.mapped ? capacity : 0;
        if (!constants.mapped) return;
    }

    const auto targets = Textures();
    const Graphics::IGraphicsTexture* targetList[3] = {targets[0], targets[1], targets[2]};
    const float clearColor[4] = {0.f, 0.f, 0.f, 0.f};
    for (uint32_t level = 0; level < m_levels.size(); ++level)
    {
        bool begun = false;
        for (size_t p = 0; p < pieces.size(); ++p)
        {
            const auto& piece = pieces[p];
            if (piece.level != level) continue;
            if (!begun)
            {
                if (!context->BeginColorPass(targetList, 3, level, m_depth.get())) return;
                const float size = float(texels);
                context->SetViewport({0.f, 0.f, size, size, 0.f, 1.f});
                begun = true;
            }
            context->SetScissorRect(piece.texture);
            context->ClearColorPassRectangle(piece.texture, piece.clear ? clearColor : nullptr,
                Graphics::Depth::ClearValue);
            ++m_statistics.rectangles;
            m_statistics.texels += uint64_t(piece.texture.right - piece.texture.left) *
                uint64_t(piece.texture.bottom - piece.texture.top);
            const float texel = m_settings.texelSize * std::ldexp(1.f, int(piece.level));
            // World X/Z at this piece's texture texel (0, 0), and 1 / extent.
            const glm::vec4 rect(float(piece.world.x0 - piece.texture.left) * texel,
                float(piece.world.z0 - piece.texture.top) * texel,
                1.f / (float(texels) * texel), 1.f / (float(texels) * texel));
            for (const auto& call : calls[p])
            {
                FillRecord record{};
                record.draw = call.draw->drawData;
                record.rect = rect;
                record.depth = glm::vec4(-2000.f, 1.f / 12000.f, 0.f, 0.f);
                std::memcpy(static_cast<uint8_t*>(constants.mapped) + call.record * sizeof(FillRecord),
                    &record, sizeof(record));
                context->SetPipeline(pipeline);
                context->SetConstantBuffer(0, constants.buffer.get(), call.record * sizeof(FillRecord));
                static const bool noTextures = std::getenv("ENGINE_TERRAIN_VT_NOTEXTURES") != nullptr; // diagnostics
                if (!noTextures)
                    // Slots 16-23 are the view's light cookies and IES profiles
                    // (bound once per view): the fill reads none of them.
                    for (uint32_t slot = 0; slot < call.draw->textureSlotCount && slot < 16; ++slot)
                        context->SetTexture(slot, call.draw->textures[slot]);
                submit(*call.draw);
                ++m_statistics.draws;
            }
        }
        if (begun) context->EndColorPass();
    }
    constants.buffer->FlushMappedWrites(0, records * sizeof(FillRecord));
}
}
