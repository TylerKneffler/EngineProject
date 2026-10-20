#pragma once

#include "Engine/Editor/UI/IEditorUi.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <functional>
#include <utility>
#include <vector>

namespace Engine::Core { class Object; }
namespace Engine::Components { class Skeleton; }
namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
enum class EditorTransformTool
{
    Translate,
    Rotate,
    Scale,
    Hand
};

struct EditorGizmoResult
{
    Engine::Core::Object* selectedObject = nullptr;
    bool selectionRequested = false;
    bool consumedClick = false;
    bool transformDragging = false;
};

// Scene-view overlays for non-mesh objects and interactive transform tools.
// Coordinates are projected into the current viewport; drawing remains behind
// IEditorUi so Editor/Core does not depend on a specific UI package.
class EditorGizmoSystem
{
public:
    void SetModelManipulation(bool enabled, int selectionMode, bool rotate)
    { m_modelManipulation = enabled; m_modelSelectionMode = selectionMode;
        m_modelRotate = rotate; }
    void SetModelScrollDepth(bool enabled) { m_modelScrollDepthEnabled = enabled; }
    void SetModelScope(int scope) { m_modelScope = scope; }
    bool IsModelDragging() const { return m_modelDragObject != nullptr; }
    bool ModelDragEnded() const { return m_modelDragEnded; }
    bool IsModelPinned(const Engine::Core::Object* joint, bool rotation) const;
    bool IsModelControllerActive(const Engine::Core::Object* joint) const;
    void SetModelControllerActive(Engine::Core::Object* joint, bool active);
    bool ApplyModelFacing(Engine::Core::Object* joint);
    void SetModelPin(Engine::Core::Object* joint, bool rotation, bool enabled);
    void ClearModelPins(Engine::Scene::Scene& scene);
    const std::vector<Engine::Core::Object*>& ModelChangedBones() const
    { return m_modelChangedBones; }
    void SetModelPickFilter(std::function<bool(const Engine::Core::Object*)> filter)
    { m_modelPickFilter = std::move(filter); }
    void SetBoneEditing(bool enabled) { m_boneEditing = enabled; }
    void SetPoseEditing(bool enabled) { m_poseEditing = enabled; }
    void SetPoseTranslationHandler(std::function<bool(Engine::Core::Object*,
        const glm::vec3&)> handler) { m_poseTranslationHandler = std::move(handler); }
    void SetBoneTransformSettings(int pivotMode, const glm::vec3& customPivot,
        float translationSnap, float rotationSnapDegrees, float scaleSnap)
    {
        m_bonePivotMode = pivotMode;
        m_boneCustomPivot = customPivot;
        m_boneTranslationSnap = translationSnap;
        m_boneRotationSnap = rotationSnapDegrees;
        m_boneScaleSnap = scaleSnap;
    }
    EditorGizmoResult DrawAndHandle(
        Engine::Scene::Scene& scene, IEditorUi& ui,
        const EditorUiViewportInput& input, EditorTransformTool tool);
    static void DrawSkeletonOverlay(Engine::Scene::Scene& scene,
        const Engine::Components::Skeleton& skeleton, IEditorUi& ui,
        const EditorUiViewportInput& input, int selectedPaletteIndex);

private:
    bool m_modelManipulation = false;
    struct ModelPin
    {
        Engine::Core::Object* joint = nullptr;
        bool position = false;
        bool rotation = false;
        glm::vec3 worldPosition{};
        glm::quat worldRotation{1.f, 0.f, 0.f, 0.f};
        glm::vec3 pole{};
    };
    std::vector<ModelPin> m_modelPins;
    std::vector<Engine::Core::Object*> m_modelSelectedPoints;
    std::vector<std::pair<Engine::Core::Object*, glm::vec3>> m_modelDragGroup;
    std::vector<Engine::Core::Object*> m_modelChangedBones;
    int m_modelScope = 1;
    int m_modelEffectiveScope = 1;
    int m_modelControllerMode = 0;
    int m_modelChainDepth = 2;
    int m_modelSelectionMode = 2;
    bool m_modelRotate = false;
    Engine::Core::Object* m_modelDragObject = nullptr;
    glm::vec3 m_modelStartWorld{};
    glm::vec3 m_modelStartHandleWorld{};
    glm::vec3 m_modelStartRotation{};
    glm::vec3 m_modelPlaneHit{};
    EditorUiVec2 m_modelStartMouse{};
    bool m_modelDragRotate = false;
    bool m_modelDragDirection = false;
    bool m_modelDragTwist = false;
    glm::vec3 m_modelTwistAxis{};
    glm::vec3 m_modelTwistBaseWorld{};
    bool m_modelDidMove = false;
    bool m_modelDragEnded = false;
    bool m_modelScrollDepthEnabled = false;
    float m_modelScrollDepth = 0.f;
    bool m_modelDragIK = false;
    Engine::Core::Object* m_modelRoot = nullptr;
    glm::vec3 m_modelSupportCenter{};
    float m_modelSupportRadius = 0.f;
    struct ModelPosePreference
    {
        Engine::Core::Object* joint = nullptr;
        glm::quat localRotation { 1.f, 0.f, 0.f, 0.f };
        float stiffness = 0.f;
    };
    std::vector<ModelPosePreference> m_modelPosePreferences;
    glm::vec3 m_modelPole{};
    glm::quat m_modelTipRotation{1.f, 0.f, 0.f, 0.f};
    std::function<bool(const Engine::Core::Object*)> m_modelPickFilter;
    bool m_boneEditing = false;
    bool m_poseEditing = false;
    std::function<bool(Engine::Core::Object*, const glm::vec3&)> m_poseTranslationHandler;
    int m_bonePivotMode = 0;
    glm::vec3 m_boneCustomPivot{};
    float m_boneTranslationSnap = 0.f;
    float m_boneRotationSnap = 0.f;
    float m_boneScaleSnap = 0.f;
    glm::vec3 m_dragPivotWorld{};
    glm::vec3 m_dragStartWorldPosition{};
    bool m_dragWasBone = false;
    Engine::Core::Object* m_dragObject = nullptr;
    int m_dragAxis = -1;
    glm::vec3 m_dragStartLocalPosition{};
    glm::vec3 m_dragStartLocalRotation{};
    glm::vec3 m_dragStartLocalScale{1.f};
    glm::vec3 m_dragWorldAxis{};
    glm::vec3 m_dragPlaneDirection{};
    float m_dragAccumulatedAngle = 0.f;
    bool m_dragPlaneValid = false;
    EditorUiVec2 m_dragStartMouse{};
    EditorUiVec2 m_dragLastMouse{};
    EditorUiVec2 m_dragScreenDirection{};
    float m_dragWorldUnitsPerPixel = 0.f;
    EditorTransformTool m_dragTool = EditorTransformTool::Translate;
    glm::vec3 m_dragLastWrittenPosition{};
    glm::vec3 m_dragLastWrittenRotation{};
    glm::vec3 m_dragLastWrittenScale{1.f};
    bool m_dragHasWritten = false;
    int m_dragBlockedDirection = 0;
    int m_dragLastAttemptDirection = 0;
};
}
