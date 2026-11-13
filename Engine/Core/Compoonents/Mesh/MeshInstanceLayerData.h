#pragma once

#include "Core/Compoonents/Mesh/MeshInstanceBatchBuilder.h"

#include <cstdint>
#include <memory>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Engine::Components
{
// Reusable placement and instance-buffer state for a group of meshes sharing
// the same transforms. The caller owns placement policy and spatial residency.
class MeshInstanceLayerData
{
public:
    using Batch = MeshInstanceBatchBuilder::Result;

    // Preserve spatial ordering and placement identity, while decoupling CPU
    // render-object count from the authored tile grid. Never mutates sources.
    template<class Key, class Placement>
    static std::vector<std::vector<const Placement*>> PackPlacementBlocks(
        const std::map<Key, std::vector<const Placement*>>& tiles,
        size_t maximumPlacements = 4096)
    {
        if (!maximumPlacements) throw std::invalid_argument("Empty instance block capacity");
        std::vector<std::vector<const Placement*>> blocks;
        for (const auto& [key, placements] : tiles)
            for (const auto* placement : placements)
            {
                if (blocks.empty() || blocks.back().size() == maximumPlacements)
                    blocks.emplace_back();
                blocks.back().push_back(placement);
            }
        return blocks;
    }

    void Reserve(size_t placementCount)
    {
        m_transforms.reserve(placementCount);
        m_placementIds.reserve(placementCount);
        m_instances->transforms.reserve(placementCount);
        m_instances->placementIds.reserve(placementCount);
    }

    void SetPlacements(std::vector<glm::mat4> transforms,
        std::vector<uint64_t> placementIds)
    {
        if (transforms.size() != placementIds.size())
            throw std::invalid_argument(
                "Instance layer transforms and placement IDs must match");
        m_transforms = std::move(transforms);
        m_placementIds = std::move(placementIds);
        m_instances = std::make_shared<Mesh::InstanceData>();
        m_instances->transforms.reserve(m_transforms.size());
        m_instances->placementIds = m_placementIds;
        for (const auto& matrix : m_transforms)
        {
            Mesh::InstanceTransform transform{};
            for (int row = 0; row < 3; ++row)
                transform.rows[row] = { matrix[0][row], matrix[1][row],
                    matrix[2][row], matrix[3][row] };
            m_instances->transforms.push_back(transform);
        }
        ++m_revision;
    }

    void AddPlacement(const glm::mat4& matrix, uint64_t placementId)
    {
        m_transforms.push_back(matrix);
        m_placementIds.push_back(placementId);
        Mesh::InstanceTransform transform{};
        for (int row = 0; row < 3; ++row)
            transform.rows[row] = { matrix[0][row], matrix[1][row],
                matrix[2][row], matrix[3][row] };
        m_instances->transforms.push_back(transform);
        m_instances->placementIds.push_back(placementId);
        m_instances->MarkDirty();
        ++m_revision;
    }

    Batch BuildBatch(const Mesh& source,
        const glm::vec3& localBoundsPadding = glm::vec3(0.f))
    {
        auto result = MeshInstanceBatchBuilder::Build(source, m_transforms,
            m_placementIds, m_instances, localBoundsPadding);
        m_instances = result.instances;
        return result;
    }

    const std::vector<glm::mat4>& GetTransforms() const { return m_transforms; }
    const std::vector<uint64_t>& GetPlacementIds() const { return m_placementIds; }
    const std::shared_ptr<Mesh::InstanceData>& GetInstanceData() const
    { return m_instances; }
    uint64_t GetRevision() const { return m_revision; }
    size_t Size() const { return m_transforms.size(); }

private:
    std::vector<glm::mat4> m_transforms;
    std::vector<uint64_t> m_placementIds;
    std::shared_ptr<Mesh::InstanceData> m_instances =
        std::make_shared<Mesh::InstanceData>();
    uint64_t m_revision = 1;
};
}
