#include "Core/Compoonents/Physics/MeshObjectCollider.h"

#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Object.h"
#include <algorithm>
#include <cmath>
#include <limits>

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
    const glm::vec3& point, const glm::vec3& normal,
    float separation, float normalImpulse)
{
    constexpr size_t maxContacts = 64;
    if (m_contacts.size() >= maxContacts) return;
    Contact contact;
    contact.otherBody = other;
    contact.pointWorld = point;
    contact.normalWorld = normal;
    contact.separation = separation;
    contact.normalImpulse = std::max(normalImpulse, 0.f);
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

    const float maximumDistanceSquared =
        std::max(maxSurfaceMappingDistance, 0.f) *
        std::max(maxSurfaceMappingDistance, 0.f);
    for (Contact& contact : m_contacts)
    {
        float bestDistanceSquared = std::numeric_limits<float>::infinity();
        size_t bestIndex = 0;
        glm::vec3 bestPoint(0.f), bestBarycentric(0.f);
        for (size_t index = 0; index + 2 < positions.size(); index += 3)
        {
            const glm::vec3 edgeA = positions[index + 1] - positions[index];
            const glm::vec3 edgeB = positions[index + 2] - positions[index];
            if (glm::dot(glm::cross(edgeA, edgeB),
                    glm::cross(edgeA, edgeB)) < 1e-12f)
                continue;
            glm::vec3 barycentric(0.f);
            const glm::vec3 point = ClosestTrianglePoint(contact.pointWorld,
                positions[index], positions[index + 1], positions[index + 2],
                barycentric);
            const float distanceSquared = glm::dot(
                point - contact.pointWorld, point - contact.pointWorld);
            if (distanceSquared >= bestDistanceSquared) continue;
            bestDistanceSquared = distanceSquared;
            bestIndex = index;
            bestPoint = point;
            bestBarycentric = barycentric;
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
