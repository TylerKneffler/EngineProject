#pragma once

#include "Core/Model/SceneSettings.h"
#include "Core/Serialization/Json.h"

JsonValue SerializeSceneSettings(const SceneSettings& settings);
void DeserializeSceneSettings(SceneSettings& settings, const JsonValue& value);
