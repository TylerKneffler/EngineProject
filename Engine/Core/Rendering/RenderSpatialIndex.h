#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <array>
#include <vector>
#include <cstdint>
#include <utility>

namespace Engine::Rendering
{
struct RenderBounds
{
    glm::vec3 minimum{0}, maximum{0};
    bool bounded = false;
    bool operator==(const RenderBounds& other) const
    { return bounded == other.bounded && minimum == other.minimum && maximum == other.maximum; }
};
inline RenderBounds TransformBounds(glm::vec3 minimum, glm::vec3 maximum, const glm::mat4& world)
{
    const glm::vec3 center = glm::vec3(world * glm::vec4((minimum + maximum) * .5f, 1));
    const glm::vec3 extent = (maximum - minimum) * .5f;
    const glm::vec3 radius = glm::abs(glm::vec3(world[0])) * extent.x +
        glm::abs(glm::vec3(world[1])) * extent.y + glm::abs(glm::vec3(world[2])) * extent.z;
    return {center - radius, center + radius, true};
}
struct RenderSpatialQueryStatistics
{
    uint64_t nodesVisited = 0;
    uint64_t boundsTested = 0;
    uint64_t visibleSlots = 0;
};
class RenderSpatialIndex
{
    static constexpr uint32_t NoNode=UINT32_MAX;
    struct Node
    {
        RenderBounds bounds;
        uint32_t first=0,count=0,left=0,right=0,parent=NoNode,depth=0;
        double area=0;
    };
    std::vector<RenderBounds> m_bounds;
    std::vector<uint32_t> m_order, m_unbounded;
    std::vector<Node> m_nodes;
    std::vector<uint32_t> m_leafForItem,m_changed;
    std::vector<uint64_t> m_marks;
    std::vector<double> m_costDeltas;
    std::vector<std::vector<uint32_t>> m_dirtyLevels;
    uint64_t m_generation=0,m_refitPasses=0;
    double m_treeCost=0,m_baselineCost=0;
    bool m_incremental=true,m_ready=true;
    static RenderBounds Merge(const RenderBounds& a,const RenderBounds& b);
    static double Area(const RenderBounds& bounds);
    struct ViewCache
    {
        glm::mat4 clip{1};
        std::vector<uint8_t> visible;
        std::vector<uint32_t> visibleItems;
        RenderSpatialQueryStatistics visibleItemsStatistics;
        bool valid=false, visibleValid=false, visibleItemsValid=false;
    };
    mutable std::array<ViewCache,8> m_views;
    mutable size_t m_nextView=0;
    uint32_t BuildNode(uint32_t first,uint32_t count,uint32_t parent=NoNode,uint32_t depth=0);
    void Rebuild();
public:
    struct VisibleSlotsQuery
    {
        std::vector<uint32_t> slots;
        RenderSpatialQueryStatistics statistics;
    };
    void SetIncrementalUpdatesEnabled(bool enabled) { m_incremental=enabled; }
    struct UpdateStatistics
    {
        uint64_t changedBounds=0,reusedBounds=0,dirtyLeaves=0,refitNodes=0,rebuilds=0,qualityRebuilds=0;
        double totalMs=0,compareMs=0,refitMs=0,buildMs=0;
    };
    // Single-owner update/publication. Worker jobs operate only on owned bounds
    // and disjoint tree nodes; queries resume after every job has joined.
    UpdateStatistics Update(std::vector<RenderBounds> bounds);
    std::vector<uint32_t> QueryPointXZ(glm::vec3 point) const
    {
        if(!m_ready)
        {
            std::vector<uint32_t> conservative(m_bounds.size());
            std::iota(conservative.begin(),conservative.end(),0); return conservative;
        }
        std::vector<uint32_t> result=m_unbounded;
        const auto contains=[&](const RenderBounds& b) {
            return point.x>=b.minimum.x && point.x<=b.maximum.x && point.z>=b.minimum.z && point.z<=b.maximum.z;
        };
        const auto query=[&](auto&& self,uint32_t index)->void {
            const auto& node=m_nodes[index]; if(!contains(node.bounds)) return;
            if(node.count) for(uint32_t j=0;j<node.count;++j)
            { const auto item=m_order[node.first+j]; if(contains(m_bounds[item])) result.push_back(item); }
            else { self(self,node.left); self(self,node.right); }
        };
        if(!m_nodes.empty()) query(query,0);
        return result;
    }
    std::vector<uint32_t> QueryRay(glm::vec3 origin, glm::vec3 direction,
        float limit) const
    {
        if (!m_ready)
        {
            std::vector<uint32_t> conservative(m_bounds.size());
            std::iota(conservative.begin(), conservative.end(), 0);
            return conservative;
        }
        std::vector<uint32_t> result = m_unbounded;
        const auto intersects = [&](const RenderBounds& bounds) {
            float nearDistance = 0.f, farDistance = limit;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (std::abs(direction[axis]) < 1e-8f)
                {
                    if (origin[axis] < bounds.minimum[axis] ||
                        origin[axis] > bounds.maximum[axis]) return false;
                    continue;
                }
                float a = (bounds.minimum[axis] - origin[axis]) / direction[axis];
                float b = (bounds.maximum[axis] - origin[axis]) / direction[axis];
                if (a > b) std::swap(a, b);
                nearDistance = std::max(nearDistance, a);
                farDistance = std::min(farDistance, b);
                if (nearDistance > farDistance) return false;
            }
            return farDistance > 0.f;
        };
        const auto query = [&](auto&& self, uint32_t nodeIndex) -> void {
            const auto& node = m_nodes[nodeIndex];
            if (!intersects(node.bounds)) return;
            if (node.count)
                for (uint32_t i = 0; i < node.count; ++i)
                {
                    const uint32_t item = m_order[node.first + i];
                    if (intersects(m_bounds[item])) result.push_back(item);
                }
            else { self(self, node.left); self(self, node.right); }
        };
        if (!m_nodes.empty()) query(query, 0);
        std::sort(result.begin(), result.end());
        return result;
    }
    // Query without touching the view cache so independent views can prepare
    // visibility concurrently against the same published tree.
    std::vector<uint8_t> QueryUncached(const glm::mat4& clip) const
    {
        std::vector<uint8_t> visible(m_bounds.size(),m_ready ? 0:1);
        if(!m_ready) return visible;
        for (auto i:m_unbounded) visible[i]=1;
        glm::vec4 row[4];
        for (int i=0;i<4;++i) row[i]={clip[0][i],clip[1][i],clip[2][i],clip[3][i]};
        const glm::vec4 planes[6]={row[3]+row[0],row[3]-row[0],row[3]+row[1],row[3]-row[1],row[2],row[3]-row[2]};
        const auto outside=[&](const RenderBounds& b) {
            const auto center=(b.minimum+b.maximum)*.5f, extent=(b.maximum-b.minimum)*.5f;
            for (const auto& p:planes)
                if (glm::dot(glm::vec3(p),center)+p.w+glm::dot(glm::abs(glm::vec3(p)),extent)<-1.e-4f) return true;
            return false;
        };
        const auto query=[&](auto&& self,uint32_t i)->void {
            const auto& node=m_nodes[i]; if (outside(node.bounds)) return;
            if (node.count) for(uint32_t j=0;j<node.count;++j)
            { const auto item=m_order[node.first+j]; if(!outside(m_bounds[item])) visible[item]=1; }
            else { self(self,node.left); self(self,node.right); }
        };
        if (!m_nodes.empty()) query(query,0);
        return visible;
    }
    // Return only visible slots for consumers that can expand slots to their
    // associated render items. This avoids scanning the full slot array after
    // the BVH has already rejected entire branches.
    VisibleSlotsQuery QueryVisibleSlotsUncached(const glm::mat4& clip) const
    {
        VisibleSlotsQuery result;
        if (!m_ready)
        {
            result.slots.resize(m_bounds.size());
            std::iota(result.slots.begin(), result.slots.end(), 0u);
            result.statistics.visibleSlots = result.slots.size();
            return result;
        }
        auto& visible = result.slots;
        visible = m_unbounded;
        glm::vec4 row[4];
        for (int i = 0; i < 4; ++i)
            row[i] = { clip[0][i], clip[1][i], clip[2][i], clip[3][i] };
        const glm::vec4 planes[6] = {
            row[3] + row[0], row[3] - row[0], row[3] + row[1],
            row[3] - row[1], row[2], row[3] - row[2] };
        const auto outside = [&](const RenderBounds& bounds)
        {
            ++result.statistics.boundsTested;
            const auto center = (bounds.minimum + bounds.maximum) * .5f;
            const auto extent = (bounds.maximum - bounds.minimum) * .5f;
            for (const auto& plane : planes)
                if (glm::dot(glm::vec3(plane), center) + plane.w +
                    glm::dot(glm::abs(glm::vec3(plane)), extent) < -1.e-4f)
                    return true;
            return false;
        };
        const auto query = [&](auto&& self, uint32_t index) -> void
        {
            ++result.statistics.nodesVisited;
            const auto& node = m_nodes[index];
            if (outside(node.bounds)) return;
            if (node.count)
            {
                for (uint32_t i = 0; i < node.count; ++i)
                {
                    const uint32_t item = m_order[node.first + i];
                    if (!outside(m_bounds[item])) visible.push_back(item);
                }
            }
            else
            {
                self(self, node.left);
                self(self, node.right);
            }
        };
        if (!m_nodes.empty()) query(query, 0);
        result.statistics.visibleSlots = visible.size();
        return result;
    }
    const std::vector<uint32_t>& QueryVisibleSlots(const glm::mat4& clip,
        RenderSpatialQueryStatistics* statistics = nullptr) const
    {
        for (const auto& view : m_views)
            if (view.valid && view.visibleItemsValid && view.clip == clip)
            {
                if (statistics) *statistics = view.visibleItemsStatistics;
                return view.visibleItems;
            }
        ViewCache* selected = nullptr;
        for (auto& view : m_views)
            if (view.valid && view.clip == clip) { selected = &view; break; }
        if (!selected)
        {
            selected = &m_views[m_nextView++ % m_views.size()];
            selected->visibleValid = false;
            selected->visibleItemsValid = false;
        }
        selected->clip = clip;
        auto result = QueryVisibleSlotsUncached(clip);
        selected->visibleItems = std::move(result.slots);
        selected->visibleItemsStatistics = result.statistics;
        selected->visibleItemsValid = true;
        selected->valid = true;
        if (statistics) *statistics = selected->visibleItemsStatistics;
        return selected->visibleItems;
    }
    const std::vector<uint8_t>& Query(const glm::mat4& clip) const
    {
        for(const auto& view:m_views)
            if(view.valid && view.visibleValid && view.clip==clip) return view.visible;
        ViewCache* selected=nullptr;
        for(auto& view:m_views)
            if(view.valid && view.clip==clip) { selected=&view; break; }
        if(!selected)
        {
            selected=&m_views[m_nextView++ % m_views.size()];
            selected->visibleValid=false;
            selected->visibleItemsValid=false;
        }
        selected->clip=clip;
        selected->visible=QueryUncached(clip);
        selected->visibleValid=true;
        selected->valid=true;
        return selected->visible;
    }
};
}
