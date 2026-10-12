#include "Core/Compoonents/Animation/SoftTissueSurface.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Object.h"
#include "Core/ComponentReference.h"
#include "Core/Scene/Scene.h"
#include "Core/Graphics/IGraphicsBuffer.h"
#include <cassert>
#include <cmath>
#include <cstring>

class CpuBuffer final : public Engine::Graphics::IGraphicsBuffer
{
public:
    CpuBuffer(Usage usage, AccessMode access, uint64_t size, const void* data)
        : m_usage(usage), m_access(access), m_data(static_cast<size_t>(size))
    { if (data) std::memcpy(m_data.data(), data, m_data.size()); }
    Usage GetUsage() const override { return m_usage; }
    AccessMode GetAccessMode() const override { return m_access; }
    uint64_t GetSize() const override { return m_data.size(); }
    void* Map() override { return m_data.data(); }
    void Unmap() override {}
    void* GetNativeHandle() const override { return nullptr; }
private:
    Usage m_usage;
    AccessMode m_access;
    std::vector<unsigned char> m_data;
};

class CpuBufferFactory final : public Engine::Graphics::IGraphicsBufferFactory
{
public:
    std::unique_ptr<Engine::Graphics::IGraphicsBuffer> CreateBuffer(
        CpuBuffer::Usage usage, CpuBuffer::AccessMode access,
        uint64_t size, const void* data = nullptr, uint32_t = 0) override
    { return std::make_unique<CpuBuffer>(usage, access, size, data); }
};

using namespace Engine::Components;

static bool Near(const glm::vec3& a, const glm::vec3& b)
{
    return glm::length(a - b) < 1e-5f;
}

static glm::vec3 Position(const Mesh::Vertex& vertex)
{
    return { vertex.pos[0], vertex.pos[1], vertex.pos[2] };
}

int main()
{
    Engine::Scene::Scene scene;
    auto* object = scene.AddObject("Surface");
    auto* mesh = object->AddComponent<Mesh>();
    std::vector<Mesh::Vertex> vertices(4);
    vertices[0].pos[0] = 0.f; vertices[0].pos[1] = 0.f;
    vertices[1].pos[0] = 1.f; vertices[1].pos[1] = 0.f;
    vertices[2].pos[0] = 0.f; vertices[2].pos[1] = 1.f;
    vertices[3].pos[0] = 1.f; vertices[3].pos[1] = 1.f;
    for (auto& vertex : vertices)
    {
        vertex.normal[2] = 1.f;
        vertex.tangent[0] = 1.f;
        vertex.tangent[3] = 1.f;
        vertex.weights0[0] = 1.f;
    }
    assert(mesh->SetIndexedGeometry(vertices, { 0, 1, 2, 1, 3, 2 }));
    const auto original = mesh->GetVertices();
    const auto topology = mesh->GetIndices();
    const auto revision = mesh->GetAuthoredGeometryRevision();
    auto* skin = object->AddComponent<SkinnedMesh>();
    auto* tissue = object->AddComponent<SoftTissueSurface>();
    tissue->poseStrength = 0.f;
    tissue->maxDisplacement = 0.05f;
    tissue->Start();
    for (int step = 0; step < 120; ++step)
        tissue->PostPhysicsUpdate(step % 2 ? 1.f / 60.f : 1.f / 120.f);
    for (const auto& offset : tissue->GetOffsets())
        assert(Near(offset, glm::vec3(0.f)));
    tissue->propagationRadius = 0.4f;
    MeshObjectCollider::Contact impact;
    impact.surfaceMapped = true;
    impact.contactId = 42;
    impact.triangleIndex = 0;
    impact.barycentric = { 1.f, 0.f, 0.f };
    impact.normalWorld = { 0.f, 0.f, -1.f };
    impact.normalImpulse = 5.f;
    impact.substepSamples = 1;
    tissue->QueueMappedContact(impact);
    tissue->PostPhysicsUpdate(1.f / 60.f);
    assert(tissue->GetOffsets()[0].z < -0.0001f);
    assert(Near(tissue->GetOffsets()[3], glm::vec3(0.f)));
    const float initialDent = tissue->GetOffsets()[0].z;
    for (int step = 0; step < 120; ++step)
    {
        tissue->QueueMappedContact(impact);
        tissue->PostPhysicsUpdate(1.f / 60.f);
    }
    assert(tissue->GetOffsets()[0].z > initialDent);
    assert(std::abs(tissue->GetOffsets()[0].z) < 0.001f);
    impact.surfaceMapped = false;
    impact.contactId = 43;
    tissue->QueueMappedContact(impact);
    tissue->PostPhysicsUpdate(1.f / 60.f);
    assert(std::abs(tissue->GetOffsets()[0].z) < 0.001f);
    tissue->Reset();
    tissue->Start();
    impact.surfaceMapped = true;
    impact.contactId = 44;
    impact.substepSamples = 4;
    impact.normalImpulse = 20.f;
    tissue->QueueMappedContact(impact);
    tissue->PostPhysicsUpdate(1.f / 120.f);
    assert(tissue->GetOffsets()[0].z < 0.f);
    for (int step = 0; step < 240; ++step)
        tissue->PostPhysicsUpdate(1.f / 120.f);
    assert(std::abs(tissue->GetOffsets()[0].z) < 0.001f);
    tissue->Reset();
    tissue->responseMask.assign(4, 0.f);
    tissue->Start();
    impact.contactId = 45;
    tissue->QueueMappedContact(impact);
    tissue->PostPhysicsUpdate(1.f / 60.f);
    assert(Near(tissue->GetOffsets()[0], glm::vec3(0.f)));
    tissue->responseMask.clear();

    std::vector<glm::vec3> offsets(4, glm::vec3(0.f));
    offsets[0] = { 0.f, 0.f, 0.02f };
    mesh->SetSurfaceOffsets(offsets, nullptr);
    const auto posed = mesh->BuildPosedVertices();
    const auto triangles = mesh->BuildPortalCutTriangleStream();
    assert(Near(Position(posed[0]), Position(triangles[0])));
    assert(Near(Position(posed[1]), Position(triangles[1])));
    assert(Near(Position(posed[2]), Position(triangles[2])));
    assert(Near(Position(posed[1]), Position(triangles[3])));
    assert(Near(Position(posed[0]), glm::vec3(0.f, 0.f, 0.02f)));
    assert(mesh->GetIndices() == topology);
    assert(mesh->GetAuthoredGeometryRevision() == revision);
    for (size_t i = 0; i < original.size(); ++i)
    {
        assert(Near(Position(mesh->GetVertices()[i]), Position(original[i])));
        assert(mesh->GetVertices()[i].weights0[0] == original[i].weights0[0]);
    }
    Mesh::MorphTarget morph;
    morph.positions.resize(4, glm::vec3(0.f));
    morph.positions[0].x = 0.1f;
    mesh->SetMorphData(0, { morph }, { 0.5f });
    glm::mat4 joint(1.f);
    joint[3].z = 0.3f;
    const std::vector<glm::mat4> palette { joint };
    const auto morphSkinPose = mesh->BuildPosedVertices(&palette);
    const auto portalPose = mesh->BuildPortalCutTriangleStream(&palette);
    assert(Near(Position(morphSkinPose[0]), glm::vec3(0.05f, 0.f, 0.32f)));
    assert(Near(Position(morphSkinPose[0]), Position(portalPose[0])));
    CpuBufferFactory bufferFactory;
    mesh->CreateBuffer(&bufferFactory);
    mesh->SetSurfaceOffsets(offsets, &palette);
    auto* gpuStream = mesh->GetPosedVertexBuffer();
    assert(gpuStream && gpuStream->GetSize() ==
        morphSkinPose.size() * sizeof(Mesh::Vertex));
    const auto* uploaded = static_cast<const Mesh::Vertex*>(gpuStream->Map());
    for (size_t i = 0; i < morphSkinPose.size(); ++i)
    {
        assert(Near(Position(uploaded[i]), Position(morphSkinPose[i])));
        assert(Near(glm::vec3(uploaded[i].normal[0], uploaded[i].normal[1],
            uploaded[i].normal[2]), glm::vec3(morphSkinPose[i].normal[0],
            morphSkinPose[i].normal[1], morphSkinPose[i].normal[2])));
    }
    gpuStream->Unmap();
    tissue->Reset();
    assert(!mesh->HasSurfaceOffsets());
    assert(Near(Position(mesh->BuildPortalCutTriangleStream()[0]),
        glm::vec3(0.05f, 0.f, 0.f)));
    const auto saved = tissue->Serialize();
    SoftTissueSurface restored;
    restored.Deserialize(saved);
    assert(restored.GetOffsets().empty());
    assert(restored.maxDisplacement == tissue->maxDisplacement);
    (void)skin;

    Engine::Scene::Scene posedScene;
    auto* rig = posedScene.AddObject("Rig");
    auto* model = rig->AddComponent<Model>();
    auto* skeleton = rig->AddComponent<Skeleton>();
    skeleton->skinIndex = 0;
    skeleton->modelReference =
        Engine::Core::CaptureComponentReference(model, "Model");
    skeleton->jointNodes = { 0, 1 };
    skeleton->inverseBindMatrices.assign(2, glm::mat4(1.f));
    auto* left = posedScene.AddObject("Left");
    auto* right = posedScene.AddObject("Right");
    assert(posedScene.MoveObject(left, rig,
        Engine::Scene::Scene::ObjectPlacement::AsChild));
    assert(posedScene.MoveObject(right, rig,
        Engine::Scene::Scene::ObjectPlacement::AsChild));
    model->BindNode(0, left);
    model->BindNode(1, right);
    auto* surface = posedScene.AddObject("Animated surface");
    assert(posedScene.MoveObject(surface, rig,
        Engine::Scene::Scene::ObjectPlacement::AsChild));
    auto* posedMesh = surface->AddComponent<Mesh>();
    auto posedVertices = vertices;
    posedVertices[1].joints0[0] = 1.f;
    posedVertices[3].joints0[0] = 1.f;
    assert(posedMesh->SetIndexedGeometry(posedVertices, topology));
    auto* posedSkin = surface->AddComponent<SkinnedMesh>();
    posedSkin->skinIndex = 0;
    posedSkin->meshReference =
        Engine::Core::CaptureComponentReference(posedMesh, "Mesh");
    posedSkin->skeletonReference =
        Engine::Core::CaptureComponentReference(skeleton, "Skeleton");
    auto* posedTissue = surface->AddComponent<SoftTissueSurface>();
    posedTissue->poseStrength = 0.04f;
    posedScene.Start();
    right->transform.position.x = -0.25f;
    for (int step = 0; step < 120; ++step)
        posedTissue->PostPhysicsUpdate(1.f / 60.f);
    assert(posedTissue->GetOffsets()[0].z < -0.001f);
    assert(posedMesh->GetVertices()[1].joints0[0] == 1.f);
    right->transform.position.x = 0.f;
    for (int step = 0; step < 120; ++step)
        posedTissue->PostPhysicsUpdate(1.f / 60.f);
    assert(std::abs(posedTissue->GetOffsets()[0].z) < 0.001f);

    // A dent must settle onto each new animated pose, not the bind pose.
    posedTissue->poseStrength = 0.f;
    posedTissue->Reset();
    posedTissue->Start();
    MeshObjectCollider::Contact movingImpact;
    movingImpact.surfaceMapped = true;
    movingImpact.contactId = 101;
    movingImpact.triangleIndex = 0;
    movingImpact.barycentric = { 1.f, 0.f, 0.f };
    movingImpact.normalWorld = { 0.f, 0.f, -1.f };
    movingImpact.normalImpulse = 5.f;
    movingImpact.substepSamples = 1;
    posedTissue->QueueMappedContact(movingImpact);
    posedTissue->PostPhysicsUpdate(1.f / 60.f);
    assert(posedTissue->GetOffsets()[0].z < -0.0001f);
    for (int step = 0; step < 180; ++step)
    {
        right->transform.position.x = -0.2f + 0.1f *
            std::sin(static_cast<float>(step) * 0.07f);
        posedTissue->PostPhysicsUpdate(1.f / 60.f);
    }
    const auto animated = posedMesh->BuildPosedVertices(
        &posedSkin->BuildPalette(), false);
    const auto deformed = posedMesh->BuildPosedVertices(
        &posedSkin->BuildPalette());
    for (size_t i = 0; i < animated.size(); ++i)
        assert(glm::length(Position(deformed[i]) - Position(animated[i])) < 0.001f);
    assert(std::abs(Position(animated[1]).x - Position(original[1]).x) > 0.05f);
}
