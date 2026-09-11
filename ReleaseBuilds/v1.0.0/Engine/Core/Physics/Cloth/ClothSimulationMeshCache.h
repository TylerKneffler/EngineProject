#pragma once

#include <memory>
#include <string>

namespace Engine::Components { class Mesh; }

namespace Engine::Physics
{
// Loads and shares the immutable source mesh used to build cloth soft bodies.
// Cache entries are invalidated when the source asset changes on disk.
class ClothSimulationMeshCache final
{
public:
    static std::shared_ptr<Engine::Components::Mesh> Acquire(
        const std::string& path);
};
}
