#pragma once

#include "Core/Compoonents/Mesh/Mesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace Engine::Components
{
// Builds renderer-ready instance data and conservative world bounds from
// arbitrary transforms. Placement ownership, streaming, and surface sampling
// stay with the caller.
class MeshInstanceBatchBuilder
{
public:
    struct Result
    {
        std::shared_ptr<Mesh::InstanceData> instances;
        glm::vec3 minimum{0.f};
        glm::vec3 maximum{0.f};
        bool hasBounds = false;
    };

    static Result Build(const Mesh& source,
        const std::vector<glm::mat4>& placementTransforms,
        const std::vector<uint64_t>& placementIds,
        std::shared_ptr<Mesh::InstanceData> existing = {},
        const glm::vec3& localBoundsPadding = glm::vec3(0.f))
    {
        if (placementTransforms.size() != placementIds.size())
            throw std::invalid_argument(
                "Prefab instance transforms and placement IDs must match");

        Result result;
        result.instances = existing ? std::move(existing)
            : std::make_shared<Mesh::InstanceData>();
        if (result.instances->transforms.empty() &&
            result.instances->placementIds.empty())
        {
            result.instances->transforms.reserve(placementTransforms.size());
            result.instances->placementIds.reserve(placementIds.size());
            for (size_t index = 0; index < placementTransforms.size(); ++index)
            {
                const glm::mat4& matrix = placementTransforms[index];
                Mesh::InstanceTransform transform{};
                for (int row = 0; row < 3; ++row)
                    transform.rows[row] = { matrix[0][row], matrix[1][row],
                        matrix[2][row], matrix[3][row] };
                result.instances->transforms.push_back(transform);
                result.instances->placementIds.push_back(placementIds[index]);
            }
        }
        else if (result.instances->transforms.size() != placementTransforms.size() ||
            result.instances->placementIds.size() != placementIds.size() ||
            result.instances->placementIds != placementIds)
        {
            throw std::invalid_argument(
                "Shared prefab instance data does not match its placements");
        }

        if (!source.HasBounds() || placementTransforms.empty())
            return result;

        const glm::vec3 localMinimum = source.GetBoundsMin() - localBoundsPadding;
        const glm::vec3 localMaximum = source.GetBoundsMax() + localBoundsPadding;
        const glm::vec3 sourceCenter = (localMinimum + localMaximum) * .5f;
        const glm::vec3 sourceExtents = (localMaximum - localMinimum) * .5f;
        result.minimum = glm::vec3(std::numeric_limits<float>::max());
        result.maximum = glm::vec3(-std::numeric_limits<float>::max());
        for (const glm::mat4& matrix : placementTransforms)
        {
            const glm::vec3 center(matrix * glm::vec4(sourceCenter, 1.f));
            const glm::mat3 linear(matrix);
            const glm::vec3 extents = glm::abs(linear[0]) * sourceExtents.x +
                glm::abs(linear[1]) * sourceExtents.y +
                glm::abs(linear[2]) * sourceExtents.z;
            result.minimum = glm::min(result.minimum, center - extents);
            result.maximum = glm::max(result.maximum, center + extents);
        }
        result.hasBounds = true;

        if (!result.instances->hasLodBounds)
        {
            result.instances->lodMinimum = localMinimum;
            result.instances->lodMaximum = localMaximum;
            result.instances->hasLodBounds = true;
        }
        else
        {
            result.instances->lodMinimum = glm::min(
                result.instances->lodMinimum, localMinimum);
            result.instances->lodMaximum = glm::max(
                result.instances->lodMaximum, localMaximum);
        }
        return result;
    }
};
}
