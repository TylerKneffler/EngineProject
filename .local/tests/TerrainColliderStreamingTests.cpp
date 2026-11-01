#include "Core/Scene/Scene.h"
#include "Core/Compoonents/Obj/TerrainManager.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Physics/MeshObjectCollider.h"
#include <iostream>
#include <stdexcept>
#include <map>
#include "Core/Compoonents/Physics/PrimitiveObjectCollider.h"
#include <limits>
#include <fstream>

void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

int main(int argc, char** argv)
{
    try
    {
        using namespace Engine::Components;
        Engine::Scene::Scene scene;
        auto* viewer = scene.AddObject("Player");
        auto* landscape = scene.AddObject("Landscape");
        landscape->transform.position = {1024, 0, -2048};
        auto* manager = landscape->AddComponent<TerrainManager>();
        manager->cellsX = manager->cellsY = 16;
        manager->cellWidth = manager->cellDepth = 1;
        manager->generateColliders = true;
        manager->manageAuthoredCellColliders = true;
        manager->viewerObjectName = "Player";
        manager->colliderActivationDistance = 1;
        manager->loadedRadius = 8; // Visual residency must not enlarge collision residency.
        std::map<TerrainManager::Cell, RigidBody*> bodies;
        for (int z = -2; z <= 2; ++z)
            for (int x = -2; x <= 2; ++x)
            {
                auto* cell = scene.AddObject("Terrain");
                cell->Parent = landscape;
                landscape->Children.push_back(cell);
                cell->transform.position = {x * 16., 0, z * 16.};
                auto* terrain = cell->AddComponent<Terrain>();
                terrain->Resize(16,16,1,1);
                terrain->OnAfterDeserialize(nullptr);
                cell->AddComponent<MeshObjectCollider>()->convex = false;
                auto* body = cell->AddComponent<RigidBody>();
                body->bodyType = "Static";
                body->useGravity = false;
                bodies[{x,z}] = body;
            }
        const auto probe = [&](double x, double z, size_t expected)
        {
            viewer->transform.position = {1024 + x, 2, -2048 + z};
            manager->Update();
            Require(manager->GetManagedColliderCellCount() == 25, "authored terrain discovery failed");
            Require(manager->GetActiveColliderCellCount() == expected, "wrong active terrain cell count");
            scene.GetPhysics().Step(1./60.);
            size_t native = 0, enabled = 0;
            for (const auto& [coordinate, body] : bodies)
            {
                const bool active = !body->IsCollisionSuspended();
                enabled += active;
                native += body->GetPhysicsBodyGeneration() != 0;
                Require(body->Owner->GetComponent<MeshObjectCollider>()->collisionEnabled == active,
                    "collider and native body residency disagree");
                Require(body->Owner->enabled, "collision streaming disabled terrain rendering");
            }
            Require(native == expected && enabled == expected, "distant bodies remained in physics");
            const auto hits = scene.GetPhysics().RaycastAll(
                {static_cast<float>(1024+x), 5.f, static_cast<float>(-2048+z)}, {0,-1,0}, 10);
            Require(!hits.empty(), "missing terrain collision beneath player");
        };
        probe(8,8,1);
        probe(15.5,8,2);
        probe(15.5,15.5,4);
        probe(16.5,16.5,4);
        probe(24,24,1);
        probe(.5,8,2);
        probe(.5,.5,4);
        probe(-.5,-.5,4);
        probe(-8,-8,1);
        probe(0,0,4);
        probe(8,-8,1); // Teleport retires the previous region before the next step.
        probe(47.5,47.5,1); // Finite map corner has no outside neighbor.
        landscape->transform.scale = {2,2,2};
        manager->UpdateColliderStreamingAtPosition({1054,2,-2032}); // Two world metres from the edge.
        Require(manager->GetActiveColliderCellCount() == 1, "activation distance changed with landscape scale");
        manager->UpdateColliderStreamingAtPosition({1055.5,2,-2032});
        Require(manager->GetActiveColliderCellCount() == 2, "scaled landscape edge activation failed");
        landscape->transform.scale = {1,1,1};
        viewer->transform.position = {1000, 1.05, -2072};
        auto* capsule = viewer->AddComponent<PrimitiveObjectCollider>();
        capsule->shape = "Capsule"; capsule->radius = .35f; capsule->height = 1.8f;
        auto* playerBody = viewer->AddComponent<RigidBody>();
        playerBody->mass = 75; playerBody->useGravity = true;
        playerBody->restitution = 0; playerBody->friction = .8f;
        playerBody->continuousCollision = true;
        playerBody->freezeRotationX = playerBody->freezeRotationY = playerBody->freezeRotationZ = true;
        double low = 1.e9, high = -1.e9;
        float previousHeight = 0, maxStep = 0;
        std::ofstream walkTrace(".local/performance-logs/capsule-walk-check.csv");
        for (int frame = 0; frame < 480; ++frame)
        {
            const float dt = frame < 120 ? 1.f/60.f : (frame % 17 == 0 ? .08f : .04f);
            auto velocity = playerBody->GetLinearVelocity();
            const float speed = frame < 120 ? 0.f : 4.2f;
            if (argc < 2 || std::string(argv[1]) != "--no-pose")
                playerBody->SetWorldPose(viewer->transform.GetWorldPosition(), glm::quat(1,0,0,0));
            playerBody->SetLinearVelocity({speed, velocity.y, speed});
            manager->Update();
            scene.GetPhysics().Step(dt);
            const auto position = viewer->transform.GetWorldPosition();
            const float height = position.y;
            walkTrace << frame << ',' << position.x-1024 << ',' << position.z+2048 << ',' << height << ','
                << playerBody->GetLinearVelocity().y << ',' << manager->GetActiveColliderCellCount() << '\n';
            if (frame >= 120)
            {
                low = std::min(low, static_cast<double>(height));
                high = std::max(high, static_cast<double>(height));
                if (frame > 120) maxStep = std::max(maxStep, std::abs(height-previousHeight));
            }
            previousHeight = height;
        }
        Require(high-low < .01 && maxStep < .005f,
            "capsule bounced while crossing flat terrain triangles or cell seams");
        std::cout << "Walking capsule height range: " << low << " .. " << high
            << " m; largest vertical frame step " << maxStep << " m.\n";
        Require(manager->Serialize()["manageAuthoredCellColliders"].AsBool(), "collision mode not serialized");
        Require(manager->Serialize()["colliderActivationDistance"].AsFloat() == 1, "edge distance not serialized");

        Engine::Scene::Scene streamed;
        auto* owner = streamed.AddObject("Streamed landscape");
        auto* map = owner->AddComponent<TerrainManager>();
        map->cellsX = map->cellsY = 16;
        map->generateColliders = true;
        map->loadedRadius = map->farRadius = 2;
        for (int z = -1; z <= 1; ++z)
            for (int x = -1; x <= 1; ++x) Require(map->AddCell(x,z), "streamed cell add failed");
        map->UpdateStreamingAtPosition({8,2,8});
        Require(map->GetActiveColliderCellCount() == 1, "streamed manager kept a broad collider radius");
        map->UpdateStreamingAtPosition({15.5,2,15.5});
        Require(map->GetActiveColliderCellCount() == 4, "streamed corner activation failed");
        std::cout << "Terrain collision: centers=1, edges=2, corners=4; negative coordinates, teleports, finite edges, rendering, native body removal, and streamed cells passed.\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
