#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"

namespace Engine::Scene
{

bool Scene::Save(const std::string& path) const
{
    return Engine::Serialization::SceneSerializer::Save(*this, path);
}

bool Scene::Load(const std::string& path)
{
    if (!m_graphicsProvider)
        return false;
    return Engine::Serialization::SceneSerializer::Load(*this, path, m_graphicsProvider);
}

std::string Scene::SaveToString() const
{
    return Engine::Serialization::SceneSerializer::SaveToString(*this);
}

bool Scene::LoadFromString(const std::string& source)
{
    if (!m_graphicsProvider)
        return false;
    return Engine::Serialization::SceneSerializer::LoadFromString(
        *this, source, m_graphicsProvider);
}

}
