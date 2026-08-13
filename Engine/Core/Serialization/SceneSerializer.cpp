#include "Core/Serialization/SceneSerializer.h"

// SceneSerializer is intentionally a stable public facade. Its implementation
// is divided by behavior across:
//   SceneSerializerRegistry.cpp
//   SceneSerializerXmlBehavior.cpp
//   SceneObjectSerializationBehavior.cpp
//   SceneDocumentSerializationBehavior.cpp
//   ScenePrefabSerializationBehavior.cpp
