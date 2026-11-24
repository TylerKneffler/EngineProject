#pragma once
#include "AutomaticInstancing.h"
#include "PersistentInstanceBatches.h"
#include "PersistentInstancePool.h"
#include <glm/glm.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Engine::Rendering
{
// Visibility monitor (diagnostics, ENGINE_VISIBILITY_MONITOR=<file.jsonl> with
// ENGINE_TRACE_LAYER=<name part>): for each GPU cull batch of the traced layer
// in the camera pass, the CPU works out which placements the cull should keep
// (ReferenceCull, the same tests as InstanceCull.hlsl except occlusion), the
// batch's whole visible list is read back once the frame has run, and the two
// are compared. Lines written:
//   {"type":"batch","serial":S,...}   a batch with placements missing or extra
//   {"type":"frame","serial":S,...}   per frame: expected, kept, missing, extra
// S is the renderer's prepared frame serial (frames.jsonl "render_serial").
// Missing = expected but not kept by the GPU (with occlusion on, includes the
// placements it judged hidden). Extra = kept but not expected.

// Why the reference cull rejected placements (index into rejected[]).
enum class ReferenceReject : uint32_t { TileBand, FinalDistance, Reach, Thinned, Band, Frustum, Count };

struct ReferenceCullResult
{
    std::vector<uint32_t> indices;      // expected pool indices
    std::vector<glm::vec4> info;        // per expected: world centre xyz, camera distance
    std::vector<uint32_t> tileOf;       // per expected: tile record index
    std::array<uint32_t, size_t(ReferenceReject::Count)> rejected{};
};

namespace VisibilityMonitorDetail
{
inline uint32_t ThinHash(int32_t x, int32_t y, uint32_t seed)
{
    uint32_t hash = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ seed;
    hash ^= hash >> 16; hash *= 0x7feb352du; hash ^= hash >> 15; hash *= 0x846ca68bu;
    return hash ^ (hash >> 16);
}
inline bool ThinnedAway(glm::vec4 thin, glm::vec3 root, glm::vec3 camera)
{
    if (thin.z >= 1.f || thin.x == thin.y) return false;
    const float t = glm::clamp((glm::length(root - camera) - thin.x) / (thin.y - thin.x), 0.f, 1.f);
    const float share = glm::mix(1.f, glm::clamp(thin.z, 0.f, 1.f), t * t * (3.f - 2.f * t));
    const float rank = float(ThinHash(int32_t(std::floor(root.x * 16.f)), int32_t(std::floor(root.z * 16.f)),
        0x1b873593u) & 4095u) / 4095.f;
    return share <= rank;
}
}

// InstanceCull.hlsl CSMain, selection.y == 2 (tile records), camera pass,
// without occlusion. Keep in sync with the shader.
inline ReferenceCullResult ReferenceCull(const InstanceCullConstants& constants,
    const std::vector<InstanceTileRecord>& tiles, const PersistentInstancePool& pool)
{
    using namespace VisibilityMonitorDetail;
    ReferenceCullResult result;
    const glm::vec3 camera(constants.cameraPosition);
    const glm::ivec4 selection = constants.selection;
    const bool shadowPass = (selection.w & 1) != 0;
    glm::vec4 rows[4];
    for (int row = 0; row < 4; ++row)
        rows[row] = glm::vec4(constants.viewProjection[0][row], constants.viewProjection[1][row],
            constants.viewProjection[2][row], constants.viewProjection[3][row]);
    const glm::vec4 planes[6] = { rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1],
        rows[3] - rows[1], rows[2], rows[3] - rows[2] };
    const auto reject = [&](ReferenceReject why, uint32_t count = 1) { result.rejected[size_t(why)] += count; };
    for (uint32_t tileIndex = 0; tileIndex < tiles.size(); ++tileIndex)
    {
        const auto& tile = tiles[tileIndex];
        const glm::vec4 lod = tile.lod;
        if (selection.x >= 0 && (selection.w & 2) == 0)
        {
            const float distance = glm::length(glm::vec3(tile.origin) - camera);
            const float width = std::max(.001f, std::min(.5f, (lod.y - lod.x) * .25f));
            if ((selection.x == 0 && distance > lod.x + width) ||
                (selection.x == 1 && (distance < lod.x - width || distance > lod.y + width)) ||
                (selection.x == 2 && distance < lod.y - width))
            { reject(ReferenceReject::TileBand, tile.range.y); continue; }
        }
        const glm::vec3 localCenter = (glm::vec3(tile.minimum) + glm::vec3(tile.maximum)) * .5f;
        const glm::vec3 localExtent = (glm::vec3(tile.maximum) - glm::vec3(tile.minimum)) * .5f;
        const glm::vec4 w0 = tile.world[0], w1 = tile.world[1], w2 = tile.world[2];
        const glm::vec3 worldX(w0), worldY(w1), worldZ(w2);
        const uint32_t reachBits = (uint32_t(selection.w) >> 16) & 0x3ffu;
        glm::vec4 thin(glm::uintBitsToFloat(tile.reserved.x), glm::uintBitsToFloat(tile.reserved.y),
            glm::uintBitsToFloat(tile.reserved.z), glm::uintBitsToFloat(tile.reserved.w));
        if (reachBits) { thin.x *= float(reachBits) / 1024.f; thin.y *= float(reachBits) / 1024.f; }
        for (uint32_t index = 0; index < tile.range.y; ++index)
        {
            const uint32_t placementIndex = tile.range.x + index;
            const auto& placement = pool.Get(placementIndex);
            const glm::vec4 center(localCenter, 1.f);
            const glm::vec3 instanceCenter(glm::dot(placement.rows[0], center), glm::dot(placement.rows[1], center),
                glm::dot(placement.rows[2], center));
            const glm::vec3 instanceExtent(glm::dot(glm::abs(glm::vec3(placement.rows[0])), localExtent),
                glm::dot(glm::abs(glm::vec3(placement.rows[1])), localExtent),
                glm::dot(glm::abs(glm::vec3(placement.rows[2])), localExtent));
            const glm::vec4 ic(instanceCenter, 1.f);
            const glm::vec4 worldCenter(glm::dot(w0, ic), glm::dot(w1, ic), glm::dot(w2, ic), 1.f);
            const glm::vec3 worldExtent(glm::dot(glm::abs(worldX), instanceExtent),
                glm::dot(glm::abs(worldY), instanceExtent), glm::dot(glm::abs(worldZ), instanceExtent));
            const float radius = glm::length(worldExtent);
            const glm::vec3 wc(worldCenter);
            const float nearestDistance = shadowPass
                ? glm::length(glm::max(glm::abs(wc - camera) - worldExtent, glm::vec3(0.f)))
                : std::max(0.f, glm::length(wc - camera) - radius);
            if (lod.w > 0.f && nearestDistance >= lod.w) { reject(ReferenceReject::FinalDistance); continue; }
            const glm::vec4 rootLocal(placement.rows[0].w, placement.rows[1].w, placement.rows[2].w, 1.f);
            const glm::vec3 root(glm::dot(w0, rootLocal), glm::dot(w1, rootLocal), glm::dot(w2, rootLocal));
            if (reachBits && constants.cameraPosition.w > 0.f &&
                glm::length(root - camera) >= constants.cameraPosition.w)
            { reject(ReferenceReject::Reach); continue; }
            if ((selection.w & (1 << 26)) == 0 && ThinnedAway(thin, root, camera))
            { reject(ReferenceReject::Thinned); continue; }
            if (selection.x >= 0 && (selection.w & 2) != 0)
            {
                const glm::vec3 delta = glm::abs(wc - camera);
                const float coverage = (selection.w & 32) != 0
                    ? glm::clamp(glm::length(glm::vec3(placement.rows[0])), .25f, 4.f) : 1.f;
                const bool rootDistance = (selection.w & 64) != 0;
                const float rootRange = glm::length(root - camera);
                const float nearest = rootDistance ? rootRange : (shadowPass
                    ? glm::length(glm::max(delta - worldExtent, glm::vec3(0.f))) : glm::length(delta) - radius) / coverage;
                const float farthest = rootDistance ? rootRange : (shadowPass
                    ? glm::length(delta + worldExtent) : glm::length(delta) + radius) / coverage;
                const float widthX = glm::clamp(lod.x * .04f, .5f, 6.f);
                const float widthY = glm::clamp(lod.y * .04f, .5f, 6.f);
                if ((selection.x == 0 && nearest > lod.x + widthX) ||
                    (selection.x == 1 && (farthest < lod.x - widthX || nearest > lod.y + widthY)) ||
                    (selection.x == 2 && farthest < lod.y - widthY))
                { reject(ReferenceReject::Band); continue; }
            }
            bool visible = true;
            for (const auto& plane : planes)
                if (glm::dot(plane, worldCenter) < -(shadowPass
                    ? glm::dot(glm::abs(glm::vec3(plane)), worldExtent) : radius * glm::length(glm::vec3(plane))))
                    visible = false;
            if (!visible) { reject(ReferenceReject::Frustum); continue; }
            result.indices.push_back(placementIndex);
            result.info.emplace_back(wc, glm::length(wc - camera));
            result.tileOf.push_back(tileIndex);
        }
    }
    return result;
}

class VisibilityMonitor
{
public:
    static VisibilityMonitor& Get() { static VisibilityMonitor monitor; return monitor; }
    bool Enabled() const { return m_enabled; }
    // One batch's comparison, from its readback (two frames after recording).
    void ReportBatch(uint64_t serial, const std::string& label, uint32_t expected, uint32_t kept,
        uint32_t missing, uint32_t extra, const std::string& samples)
    {
        std::lock_guard lock(m_mutex);
        auto& frame = m_frames[serial];
        frame[0] += expected; frame[1] += kept; frame[2] += missing; frame[3] += extra; ++frame[4];
        if (missing || extra)
            m_out << "{\"type\":\"batch\",\"serial\":" << serial << ",\"batch\":\"" << label
                << "\",\"expected\":" << expected << ",\"kept\":" << kept << ",\"missing\":" << missing
                << ",\"extra\":" << extra << ",\"missing_samples\":[" << samples << "]}\n";
    }
    // Frames whose readbacks have all landed (serial older than `completed`).
    void Flush(uint64_t completed)
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_frames.begin(); it != m_frames.end() && it->first < completed;)
        {
            const auto& f = it->second;
            m_out << "{\"type\":\"frame\",\"serial\":" << it->first << ",\"expected\":" << f[0]
                << ",\"kept\":" << f[1] << ",\"missing\":" << f[2] << ",\"extra\":" << f[3]
                << ",\"batches\":" << f[4] << "}\n";
            it = m_frames.erase(it);
        }
        m_out.flush();
    }
private:
    VisibilityMonitor()
    {
        const char* path = std::getenv("ENGINE_VISIBILITY_MONITOR");
        m_enabled = path && *path && std::getenv("ENGINE_TRACE_LAYER");
        if (m_enabled) m_out.open(path, std::ios::trunc);
        m_enabled = m_enabled && m_out.is_open();
    }
    bool m_enabled = false;
    std::ofstream m_out;
    std::mutex m_mutex;
    std::map<uint64_t, std::array<uint64_t, 5>> m_frames;
};
}
