#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <cstddef>

namespace Engine::Components
{
class Transform;
// Owner-thread capture/commit around jobs over copied TRS/matrix data. Public
// transform edits and reparenting remain compatible with the lazy accessors.
class TransformHierarchy
{
public:
    struct Statistics
    {
        uint64_t localUpdated=0,worldUpdated=0,worldReused=0,levels=0;
        double totalMs=0,snapshotMs=0,computeMs=0,commitMs=0;
        bool valid=true,topologyRebuilt=false;
    };
    Statistics Prepare(const std::vector<Transform*>& transforms);
    // Read during a stable owner phase after Prepare, with no intervening TRS
    // or hierarchy writes. External/non-batched transforms keep lazy validation.
    glm::mat4 ReadPreparedWorldMatrix(const Transform& transform) const;
    uint64_t ReadPreparedWorldRevision(const Transform& transform) const;
private:
    static constexpr size_t NoParent=size_t(-1);
    struct Node
    {
        glm::dvec3 position{};
        glm::vec3 rotation{},scale{};
        glm::dmat4 localD{1},worldD{1},externalWorld{1};
        glm::mat4 local{1},world{1};
        uint64_t localRevision=0,worldRevision=0,parentRevision=0;
        uint64_t cachedLocalRevision=0,cachedParentRevision=0;
        size_t parent=NoParent;
        bool localDirty=false,worldDirty=false,worldInitialized=false,parentChanged=false;
    };
    std::vector<Transform*> m_sources,m_parents,m_parentScratch;
    std::vector<size_t> m_dirtyScratch;
    std::vector<Node> m_nodes;
    struct Index { size_t position=0; uint64_t generation=0; };
    std::unordered_map<const Transform*,Index> m_indices;
    std::vector<size_t> m_firstChild,m_nextSibling,m_order;
    std::vector<std::pair<size_t,size_t>> m_levels;
    uint64_t m_generation=0;
    bool m_topologyValid=false;
};
}
