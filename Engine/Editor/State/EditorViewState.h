#pragma once

#include "Core/Rendering/SceneViewOverlays.h"
#include "Core/Scene/SceneObserver.h"
#include <cstdint>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Engine::Scene { class Scene; }

namespace Engine::Editor
{
// Editor view state for one scene: selection, previews, mesh/rig editing
// overlays, per-object viewport overlays and camera focus/follow. The editor
// owns it; the scene only sees the render overlays it binds
// (Scene::SetViewOverlays) and notifies it of structural changes.
class EditorViewState final : public Engine::Rendering::SceneViewOverlays,
                              public Engine::Scene::ISceneObserver
{
public:
    using Scene = Engine::Scene::Scene;

    // The state for a scene, created and bound on first use. It is released
    // automatically when the scene is destroyed.
    static EditorViewState& For(Scene& scene);
    // Existing state for a scene, or null.
    static EditorViewState* Find(const Scene* scene);

    // ---- Selection ----------------------------------------------------------
    // A terrain placement proxy also selects (and outlines) its single instance,
    // whichever UI path made the selection.
    void SelectObject(Object* object);
    void SelectPlacement(uint64_t id, Object* layer = nullptr)
    {
        selectedPlacementId = id;
        selectedPlacementLayer = id != 0 ? layer : nullptr;
    }

    // ---- Mesh and rig editing (invalidate prepared render data) --------------
    void SetMeshEditPose(bool enabled);
    void SetWeightPaint(Mesh* mesh, int bone);
    // Viewport preference only; individual rig visibility and saved assets
    // are left intact.
    bool bonesVisible = true;

    // ---- Per-object viewport overlays ----------------------------------------
    // Transient and never serialized; every object starts with none shown.
    enum Overlay : uint8_t { PositionOverlay = 1, RotationOverlay = 2,
        ScaleOverlay = 4, BoneOverlay = 8, SkeletonOverlay = 16,
        BoundsOverlay = 32, LightVolumeOverlay = 64, CameraFrustumOverlay = 128 };
    std::unordered_map<const Object*, uint8_t> overlays;
    // Overlay flags for an object through its scene's state (0 without one).
    static uint8_t OverlaysOf(const Object* object);
    // Mutable flags for toggling; valid until the object is removed. Objects
    // outside a scene get a scratch value.
    static uint8_t* MutableOverlays(Object* object);

    // ---- Editor camera (the scene's fallback viewer) ------------------------
    // Frame the object at a comfortable distance and follow it as it moves.
    // Pass nullptr to reset to the default startup position.
    void FocusCamera(Object* object);
    void UpdateCameraFollow();
    void StopCameraFollow() { m_cameraFollowTarget = nullptr; }

    // ---- ISceneObserver -------------------------------------------------------
    void OnObjectsRemoving(const std::vector<Object*>& objects) override;
    void OnObjectsClearing() override;
    void OnSceneDestroying() override;

    explicit EditorViewState(Scene& scene) : m_scene(&scene) {}
    EditorViewState(const EditorViewState&) = delete;
    EditorViewState& operator=(const EditorViewState&) = delete;

private:
    Scene* m_scene = nullptr;
    Object* m_cameraFollowTarget = nullptr;
    glm::vec3 m_cameraFollowPosition { 0.f };
    glm::quat m_cameraFollowRotation { 1.f, 0.f, 0.f, 0.f };
};
}
