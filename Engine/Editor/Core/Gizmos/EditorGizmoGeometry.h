#pragma once

#include "Engine/Editor/UI/IEditorUi.h"
#include <glm/glm.hpp>

namespace Engine::Core { class Object; }
namespace Engine::Components
{
class AnimationBone;
class Camera;
class Collider;
class Light;
class Mesh;
class Skeleton;
}
namespace Engine::Scene { class Scene; }

namespace Engine::Editor::GizmoGeometry
{
inline constexpr EditorUiColor kXColor{ 0.95f, 0.20f, 0.18f, 1.f };
inline constexpr EditorUiColor kYColor{ 0.25f, 0.85f, 0.30f, 1.f };
inline constexpr EditorUiColor kZColor{ 0.22f, 0.48f, 1.f, 1.f };
inline constexpr EditorUiColor kHoverColor{ 1.f, 0.82f, 0.16f, 1.f };
inline constexpr EditorUiColor kOutline{ 0.04f, 0.04f, 0.05f, 0.92f };

const Engine::Components::Skeleton& DisplaySkeleton(
    const Engine::Components::Skeleton& skeleton);
EditorUiVec2 Add(EditorUiVec2 a, EditorUiVec2 b);
EditorUiVec2 Subtract(EditorUiVec2 a, EditorUiVec2 b);
EditorUiVec2 Multiply(EditorUiVec2 value, float scale);
float Dot(EditorUiVec2 a, EditorUiVec2 b);
float Length(EditorUiVec2 value);
int DragDirection(float value);
glm::mat3 WorldRotation(const Engine::Core::Object& object);
glm::vec3 WorldPosition(Engine::Core::Object* object);
EditorUiColor PoseConnectionColor(float ratio);
bool ControllerFacingRotation(const Engine::Core::Object& joint,
    const Engine::Components::AnimationBone& controller, glm::quat& rotation);
bool ApplyControllerFacing(Engine::Core::Object& joint,
    const Engine::Components::AnimationBone& controller);
bool ProjectPoint(const glm::mat4& viewProjection, const glm::vec3& world,
    const EditorUiVec2& viewport, EditorUiVec2& screen);
bool RotationPlaneDirection(const glm::mat4& inverseViewProjection,
    const EditorUiVec2& mouse, const EditorUiVec2& viewport,
    const glm::vec3& pivot, const glm::vec3& axis, glm::vec3& direction);
bool ViewPlanePoint(const glm::mat4& inverseViewProjection,
    EditorUiVec2 mouse, EditorUiVec2 viewport, glm::vec3 pivot,
    glm::vec3 normal, glm::vec3& point);
float DistanceToSegment(EditorUiVec2 point, EditorUiVec2 start,
    EditorUiVec2 end, float& parameter);
void DrawWorldBox(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const glm::mat4& world, glm::vec3 minimum,
    glm::vec3 maximum, EditorUiColor color);
void DrawWorldLine(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, glm::vec3 a, glm::vec3 b, EditorUiColor color);
void DrawColliderRing(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const glm::mat4& world, float radius,
    float height, int plane, EditorUiColor color);
void DrawColliderOverlay(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const Engine::Core::Object& object,
    const Engine::Components::Collider& collider);
void DrawMeshNormals(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const Engine::Core::Object& object,
    const Engine::Components::Mesh& mesh);
void DrawLightVolume(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const Engine::Core::Object& object,
    const Engine::Components::Light& light);
void DrawCameraFrustum(IEditorUi& ui, const glm::mat4& viewProjection,
    EditorUiVec2 viewport, const Engine::Components::Camera& camera);
void DrawLightIcon(IEditorUi& ui, EditorUiVec2 center, bool selected);
void DrawSpotGizmo(IEditorUi& ui, const glm::mat4& viewProjection,
    const EditorUiVec2& viewport, const Engine::Core::Object& object,
    const Engine::Components::Light& light);
void DrawCameraIcon(IEditorUi& ui, EditorUiVec2 center, bool selected);
void DrawProbeIcon(IEditorUi& ui, EditorUiVec2 center, bool selected,
    bool volume);
void DrawBoxHandle(IEditorUi& ui, EditorUiVec2 center, float half,
    EditorUiColor color);
bool SceneContains(const Engine::Scene::Scene& scene,
    const Engine::Core::Object* object);
void DrawBoneShape(IEditorUi& ui, EditorUiVec2 root, EditorUiVec2 tip,
    bool selected, float opacity = 1.f);
}
