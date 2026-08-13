#pragma once

#include "Core/Model/SceneSettings.h"
#include "Core/Serialization/Json.h"

namespace Engine::Scene
{
Engine::Serialization::JsonValue SerializeSceneSettings(
    const Engine::Model::SceneSettings& settings);
void DeserializeSceneSettings(Engine::Model::SceneSettings& settings,
    const Engine::Serialization::JsonValue& value);
}
