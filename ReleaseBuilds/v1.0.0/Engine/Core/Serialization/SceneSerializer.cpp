#include "Core/Serialization/SceneSerializer.h"

// SceneSerializer is intentionally a stable public facade. Its implementation
// is divided by behavior across:
//   Scene/Registry/SceneSerializerRegistry.cpp
//   Scene/Xml/SceneSerializerXmlBehavior.cpp
//   Scene/Object/SceneObjectSerializationBehavior.cpp
//   Scene/Document/SceneDocumentSerializationBehavior.cpp
//   Scene/Prefab/ScenePrefabSerializationBehavior.cpp
