#pragma once

#include <string>
namespace Engine::Scene { class Scene; }

namespace Engine::Components { class Skeleton; }

namespace Engine::Editor::SkeletonBindPose
{
bool Commit(Engine::Scene::Scene& scene,
    Engine::Components::Skeleton& skeleton, std::string& error);
// Restores the authored bind transforms after animation preview stops or
// before the static Skeleton Edit and Weight Paint views are displayed.
bool Restore(Engine::Components::Skeleton& skeleton);
}
