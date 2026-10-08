#include "Core/Compoonents/Physics/MeshObjectCollider.h"

#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Engine::Components
{
namespace
{
// Closest point on a nondegenerate triangle, with barycentric coordinates.
glm::vec3 ClosestTrianglePoint(const glm::vec3& point,
    const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
    glm::vec3& barycentric)
{
    const glm::vec3 ab = b - a, ac = c - a, ap = point - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f) { barycentric = { 1.f, 0.f, 0.f }; return a; }
    const glm::vec3 bp = point - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3) { barycentric = { 0.f, 1.f, 0.f }; return b; }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
    {
        const float v = d1 / (d1 - d3);
        barycentric = { 1.f - v, v, 0.f };
        return a + v * ab;
    }
    const glm::vec3 cp = point - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6) { barycentric = { 0.f, 0.f, 1.f }; return c; }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
    {
        const float w = d2 / (d2 - d6);
        barycentric = { 1.f - w, 0.f, w };
        return a + w * ac;
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && d4 - d3 >= 0.f && d5 - d6 >= 0.f)
    {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        barycentric = { 0.f, 1.f - w, w };
        return b + w * (c - b);
    }
    const float denominator = 1.f / (va + vb + vc);
    const float v = vb * denominator, w = vc * denominator;
    barycentric = { 1.f - v - w, v, w };
    return a + v * ab + w * ac;
}

void AddWeight(MeshObjectCollider::Contact& contact, unsigned paletteIndex,
    float weight)
{
    if (!std::isfinite(weight) || weight <= 0.f) return;
    for (uint8_t i = 0; i < contact.boneWeightCount; ++i)
        if (contact.boneWeights[i].paletteIndex == paletteIndex)
        {
            contact.boneWeights[i].weight += weight;
            return;
        }
    if (contact.boneWeightCount < contact.boneWeights.size())
        contact.boneWeights[contact.boneWeightCount++] = { paletteIndex, weight };
}

float DistanceSquaredToBox(const glm::vec3& point,
    const glm::vec3& minimum, const glm::vec3& maximum)
{
    const glm::vec3 outside = glm::max(glm::max(minimum - point,
        point - maximum), glm::vec3(0.f));
    return glm::dot(outside, outside);
}
}

MeshObjectCollider::MeshObjectCollider()
{
    SetTypeName(COMPONENT_TYPE_NAME(MeshObjectCollider));
    RegisterField("meshReference", meshReference, "Mesh");
    RegisterField("meshPath", meshPath, "Mesh");
    RegisterField("convex", convex, "Mesh");
    RegisterField("maxSurfaceMappingDistance", maxSurfaceMappingDistance,
        "Contacts");
}

float MeshObjectCollider::Contact::WeightForPaletteIndex(unsigned index) const
{
    for (uint8_t i = 0; i < boneWeightCount; ++i)
        if (boneWeights[i].paletteIndex == index)
            return boneWeights[i].weight;
    return 0.f;
}

bool MeshObjectCollider::IsActiveForBody(const RigidBody* body) const
{
    if (!collisionEnabled || !Owner || !body || body->Owner != Owner)
        return false;
    for (Object* ancestor = Owner; ancestor; ancestor = ancestor->Parent)
        if (auto* skeleton = ancestor->GetComponent<Skeleton>();
            skeleton && skeleton->ResolveMeshCollider() == this)
            return skeleton->UsesMeshCollider();
    // Bullet reports the owner body, not which child of a compound shape
    // produced a manifold point. Do not attribute mixed-shape contacts to
    // this mesh until child-shape attribution is available.
    for (Component* component : Owner->Components)
        if (auto* collider = dynamic_cast<Collider*>(component);
            collider && collider != this && collider->collisionEnabled)
            return false;
    return true;
}

void MeshObjectCollider::BeginContactFrame()
{
    m_contacts.clear();
    if (++m_contactRevision == 0) ++m_contactRevision;
}

void MeshObjectCollider::RecordContact(const RigidBody* other,
    uint64_t contactId, uint32_t substepIndex,
    const glm::vec3& point, const glm::vec3& normal,
    float separation, float normalImpulse)
{
    constexpr size_t maxContacts = 64;
    for (Contact& existing : m_contacts)
        if (existing.contactId == contactId &&
            existing.otherBody == other)
        {
            existing.pointWorld = point;
            existing.normalWorld = normal;
            existing.separation = std::min(existing.separation, separation);
            existing.normalImpulse += std::max(normalImpulse, 0.f);
            existing.impulseWorld += normal * std::max(normalImpulse, 0.f);
            if (existing.lastSubstepIndex != substepIndex)
            {
                existing.lastSubstepIndex = substepIndex;
                if (existing.substepSamples <
                    std::numeric_limits<uint16_t>::max())
                    ++existing.substepSamples;
            }
            return;
        }
    if (m_contacts.size() >= maxContacts) return;
    Contact contact;
    contact.otherBody = other;
    contact.contactId = contactId;
    contact.substepSamples = 1;
    contact.lastSubstepIndex = substepIndex;
    contact.pointWorld = point;
    contact.normalWorld = normal;
    contact.separation = separation;
    contact.normalImpulse = std::max(normalImpulse, 0.f);
    contact.impulseWorld = normal * contact.normalImpulse;
    m_contacts.push_back(contact);
}

void MeshObjectCollider::ResolveContactSurfaces()
{
    if (m_contacts.empty() || !Owner) return;
    Mesh* mesh = meshReference.IsAssigned()
        ? Engine::Core::ResolveComponentReference<Mesh>(Owner, meshReference)
        : meshPath.empty() ? Owner->GetComponent<Mesh>() : nullptr;
    if (!mesh || !mesh->Owner) return;
    auto* skin = mesh->Owner->GetComponent<SkinnedMesh>();
    const std::vector<glm::mat4>* palette = skin ? &skin->BuildPalette() : nullptr;
    Skeleton* skeleton = skin ? skin->ResolveSkeleton() : nullptr;
    const std::vector<AnimationBone*>* bones = skeleton
        ? &skeleton->ResolveBones() : nullptr;
    const std::vector<Mesh::Vertex> triangles =
        mesh->BuildPortalCutTriangleStream(palette);
    if (triangles.size() < 3) return;
    const glm::mat4 world = mesh->Owner->transform.GetWorldMatrix();
    std::vector<glm::vec3> positions;
    positions.reserve(triangles.size());
    for (const Mesh::Vertex& vertex : triangles)
        positions.emplace_back(world * glm::vec4(vertex.pos[0],
            vertex.pos[1], vertex.pos[2], 1.f));

    const uint32_t triangleCount =
        static_cast<uint32_t>(positions.size() / 3);
    if (m_queryTriangles.size() != triangleCount || m_queryNodes.empty())
    {
        m_queryTriangles.resize(triangleCount);
        std::iota(m_queryTriangles.begin(), m_queryTriangles.end(), 0u);
        m_queryNodes.clear();
        m_queryNodes.reserve(triangleCount * 2);
        const auto build = [&](auto&& self, uint32_t first,
            uint32_t count) -> uint32_t
        {
            const uint32_t nodeIndex =
                static_cast<uint32_t>(m_queryNodes.size());
            m_queryNodes.emplace_back();
            m_queryNodes[nodeIndex].first = first;
            m_queryNodes[nodeIndex].count = count;
            if (count <= 8) return nodeIndex;
            glm::vec3 minimum(std::numeric_limits<float>::infinity());
            glm::vec3 maximum(-std::numeric_limits<float>::infinity());
            for (uint32_t i = first; i < first + count; ++i)
            {
                const uint32_t base = m_queryTriangles[i] * 3;
                const glm::vec3 centroid = (positions[base] +
                    positions[base + 1] + positions[base + 2]) / 3.f;
                minimum = glm::min(minimum, centroid);
                maximum = glm::max(maximum, centroid);
            }
            const glm::vec3 extent = maximum - minimum;
            const int axis = extent.x >= extent.y && extent.x >= extent.z
                ? 0 : extent.y >= extent.z ? 1 : 2;
            const uint32_t middle = first + count / 2;
            std::nth_element(m_queryTriangles.begin() + first,
                m_queryTriangles.begin() + middle,
                m_queryTriangles.begin() + first + count,
                [&](uint32_t a, uint32_t b)
                {
                    const uint32_t ai = a * 3, bi = b * 3;
                    const float ac = positions[ai][axis] +
                        positions[ai + 1][axis] + positions[ai + 2][axis];
                    const float bc = positions[bi][axis] +
                        positions[bi + 1][axis] + positions[bi + 2][axis];
                    return ac == bc ? a < b : ac < bc;
                });
            const uint32_t left = self(self, first, middle - first);
            const uint32_t right = self(self, middle, count -
                (middle - first));
            m_queryNodes[nodeIndex].left = left;
            m_queryNodes[nodeIndex].right = right;
            return nodeIndex;
        };
        build(build, 0u, triangleCount);
    }

    const auto refit = [&](auto&& self, uint32_t index) -> void
    {
        QueryNode& node = m_queryNodes[index];
        if (node.count > 8)
        {
            self(self, node.left);
            self(self, node.right);
            node.minimum = glm::min(m_queryNodes[node.left].minimum,
                m_queryNodes[node.right].minimum);
            node.maximum = glm::max(m_queryNodes[node.left].maximum,
                m_queryNodes[node.right].maximum);
            return;
        }
        node.minimum = glm::vec3(std::numeric_limits<float>::infinity());
        node.maximum = glm::vec3(-std::numeric_limits<float>::infinity());
        for (uint32_t i = node.first; i < node.first + node.count; ++i)
        {
            const uint32_t base = m_queryTriangles[i] * 3;
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                node.minimum = glm::min(node.minimum,
                    positions[base + corner]);
                node.maximum = glm::max(node.maximum,
                    positions[base + corner]);
            }
        }
    };
    refit(refit, 0u);

    const float maximumDistanceSquared =
        std::max(maxSurfaceMappingDistance, 0.f) *
        std::max(maxSurfaceMappingDistance, 0.f);
    std::vector<uint32_t> queryStack;
    queryStack.reserve(m_queryNodes.size());
    for (Contact& contact : m_contacts)
    {
        float bestDistanceSquared = std::numeric_limits<float>::infinity();
        size_t bestIndex = 0;
        glm::vec3 bestPoint(0.f), bestBarycentric(0.f);
        queryStack.clear();
        queryStack.push_back(0u);
        while (!queryStack.empty())
        {
            const QueryNode& node = m_queryNodes[queryStack.back()];
            queryStack.pop_back();
            if (DistanceSquaredToBox(contact.pointWorld, node.minimum,
                    node.maximum) > bestDistanceSquared)
                continue;
            if (node.count > 8)
            {
                const float leftDistance = DistanceSquaredToBox(
                    contact.pointWorld, m_queryNodes[node.left].minimum,
                    m_queryNodes[node.left].maximum);
                const float rightDistance = DistanceSquaredToBox(
                    contact.pointWorld, m_queryNodes[node.right].minimum,
                    m_queryNodes[node.right].maximum);
                if (leftDistance < rightDistance)
                {
                    queryStack.push_back(node.right);
                    queryStack.push_back(node.left);
                }
                else
                {
                    queryStack.push_back(node.left);
                    queryStack.push_back(node.right);
                }
                continue;
            }
            for (uint32_t i = node.first; i < node.first + node.count; ++i)
            {
                const size_t index = static_cast<size_t>(
                    m_queryTriangles[i]) * 3;
                const glm::vec3 edgeA = positions[index + 1] - positions[index];
                const glm::vec3 edgeB = positions[index + 2] - positions[index];
                const glm::vec3 area = glm::cross(edgeA, edgeB);
                if (glm::dot(area, area) < 1e-12f) continue;
                glm::vec3 barycentric(0.f);
                const glm::vec3 point = ClosestTrianglePoint(contact.pointWorld,
                    positions[index], positions[index + 1],
                    positions[index + 2], barycentric);
                const float distanceSquared = glm::dot(
                    point - contact.pointWorld, point - contact.pointWorld);
                // Adjacent triangles often share an equally close edge. Use
                // source order to make that choice deterministic across poses.
                if (distanceSquared > bestDistanceSquared ||
                    (distanceSquared == bestDistanceSquared &&
                        index >= bestIndex)) continue;
                bestDistanceSquared = distanceSquared;
                bestIndex = index;
                bestPoint = point;
                bestBarycentric = barycentric;
            }
        }
        if (!std::isfinite(bestDistanceSquared)) continue;
        contact.surfaceDistance = std::sqrt(bestDistanceSquared);
        if (bestDistanceSquared > maximumDistanceSquared) continue;
        contact.surfaceMapped = true;
        contact.surfacePointWorld = bestPoint;
        contact.triangleIndex = static_cast<uint32_t>(bestIndex / 3);
        contact.barycentric = bestBarycentric;
        if (!palette || palette->empty()) continue;
        for (int corner = 0; corner < 3; ++corner)
        {
            const Mesh::Vertex& vertex = triangles[bestIndex + corner];
            const float cornerWeight = bestBarycentric[corner];
            for (int influence = 0; influence < 4; ++influence)
            {
                const float first = vertex.joints0[influence];
                const float second = vertex.joints1[influence];
                if (std::isfinite(first) && first >= 0.f &&
                    first < static_cast<float>(palette->size()))
                    AddWeight(contact, static_cast<unsigned>(first),
                        cornerWeight * vertex.weights0[influence]);
                if (std::isfinite(second) && second >= 0.f &&
                    second < static_cast<float>(palette->size()))
                    AddWeight(contact, static_cast<unsigned>(second),
                        cornerWeight * vertex.weights1[influence]);
            }
        }
        float total = 0.f;
        for (uint8_t i = 0; i < contact.boneWeightCount; ++i)
            total += contact.boneWeights[i].weight;
        if (total > 1e-7f)
            for (uint8_t i = 0; i < contact.boneWeightCount; ++i)
                contact.boneWeights[i].weight /= total;
        if (bones)
            for (uint8_t i = 0; i < contact.boneWeightCount; ++i)
            {
                BoneWeight& influence = contact.boneWeights[i];
                if (influence.paletteIndex < bones->size())
                    influence.bone = (*bones)[influence.paletteIndex];
            }
    }
}
}
