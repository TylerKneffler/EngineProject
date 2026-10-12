#include "Scripts/Physics/FoxRagdollBlend.h"
#include "Scripts/Physics/SoftTissueDemoDriver.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/SoftTissueSurface.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include "Core/Object.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cstdio>

int main()
{
    Engine::Scene::Scene scene;
    if (!Engine::Serialization::SceneSerializer::Load(scene,
            "Engine/Core/Assets/Scenes/Physics/skinned_soft_tissue_demo.scene",
            nullptr)) return 1;
    auto* fox = scene.FindObjectByName("Fox");
    auto* probe = scene.FindObjectByName("Fox Soft Tissue Probe");
    auto* controller = scene.FindObjectByName("Soft Tissue Demo Controller");
    if (!fox || !probe || !controller ||
        !controller->GetComponent<SoftTissueDemoDriver>() ||
        !controller->GetComponent<FoxRagdollBlend>()) return 2;
    scene.Start();
    auto* tissue = controller->GetComponent<Engine::Components::SoftTissueSurface>();
    auto* collider = fox->GetComponent<Engine::Components::MeshObjectCollider>();
    if (!tissue || !collider || tissue->GetOffsets().empty()) return 3;
    float largestOffset = 0.f;
    unsigned mappedContacts = 0;
    auto* driver = controller->GetComponent<SoftTissueDemoDriver>();
    for (int frame = 0; frame < 360; ++frame)
    {
        scene.Update(1.f / 60.f);
        for (const auto& contact : collider->GetContacts())
            if (contact.surfaceMapped) ++mappedContacts;
        for (const glm::vec3& offset : tissue->GetOffsets())
            largestOffset = std::max(largestOffset, glm::length(offset));
    }
    std::fprintf(stderr,
        "Soft-tissue demo: mapped=%u releases=%u maxOffset=%.4f\n",
        mappedContacts, driver->GetReleaseCount(), largestOffset);
    return mappedContacts > 0 && driver->GetReleaseCount() > 0 &&
        largestOffset > 0.001f ? 0 : 4;
}
