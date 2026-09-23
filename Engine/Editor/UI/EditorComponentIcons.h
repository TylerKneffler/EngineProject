#pragma once

#include "Engine/Editor/UI/IEditorUi.h"
#include <string_view>

namespace Engine::Editor
{
inline EditorUiObjectIcon ComponentIconForType(std::string_view type)
{
    if (type == "Transform") return EditorUiObjectIcon::Transform;
    if (type == "Light") return EditorUiObjectIcon::Light;
    if (type == "Camera" || type == "CameraTrack")
        return EditorUiObjectIcon::Camera;
    if (type == "AudioSource") return EditorUiObjectIcon::Audio;
    if (type == "Sprite" || type == "SpriteAnimationManager")
        return EditorUiObjectIcon::Sprite;
    if (type == "Canvas" || type == "UIObject" || type == "UIImage" ||
        type == "UIText" || type == "UIButton")
        return EditorUiObjectIcon::UserInterface;
    if (type == "Mesh" || type == "Model" || type == "SkinnedMesh" ||
        type == "Material" || type == "Animation" ||
        type == "AnimationManager" || type == "Skeleton")
        return EditorUiObjectIcon::Mesh;
    if (type == "RigidBody" || type == "Cloth" ||
        type.find("Collider") != std::string_view::npos)
        return EditorUiObjectIcon::Physics;
    return EditorUiObjectIcon::Object;
}
}
