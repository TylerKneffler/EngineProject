#include "pch.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Engine/Editor/Core/View/Views/HierarchyView.h"
#include "Engine/Editor/Core/View/Views/PropertiesView.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Physics/RigidBody.h"

namespace Engine::Editor
{
void EditorState::DrawObjectEditTools(IEditorUi& ui)
{
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    Engine::Core::Object* object = scene ? scene->GetSelectedObject() : nullptr;
    if (!object)
    {
        ui.DisabledLabel("Select an object in the hierarchy.");
        return;
    }
    ui.ValueLabel("Selected object", object->name.c_str());
    auto& transform = object->transform;
    const glm::vec3 previousPosition = transform.position;
    const glm::vec3 previousRotation = transform.rotation;
    const glm::vec3 previousScale = transform.scale;
    bool changed = ui.DragFloat3("Position", &transform.position.x, .01f);
    changed |= ui.DragFloat3("Rotation", &transform.rotation.x, .01f);
    changed |= ui.DragFloat3("Scale", &transform.scale.x, .01f);
    if (!changed) return;
    uint8_t channels = 0;
    if (transform.position != previousPosition)
        channels |= Engine::Components::Transform::EditorPosition;
    if (transform.rotation != previousRotation)
        channels |= Engine::Components::Transform::EditorRotation;
    if (transform.scale != previousScale)
        channels |= Engine::Components::Transform::EditorScale;
    transform.NotifyEditorTransformChanged(channels);
    if (auto* body = object->GetComponent<Engine::Components::RigidBody>())
        body->NotifyEditorTransformChanged();
    if (Engine::Core::Object* prefabRoot = object->GetPrefabInstanceRoot())
        prefabRoot->PrefabOverrideCacheValid = false;
    if (m_activeSceneAssetDocument)
    {
        m_activeSceneAssetDocument->dirty = true;
        RefreshSceneAssetDocumentTitle(*m_activeSceneAssetDocument);
    }
    else if (scene == m_prefabScene.get())
        SetPrefabDirty(true);
    else
        MarkSceneEdited();
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::DrawObjectStageTools(IEditorUi& ui,
    SceneAssetDocument& document)
{
    SceneAssetDocument* raw = &document;
                if (!raw->objectStage) return;
                ui.ColoredLabel("Object Edit Stage", { 0.35f, 0.75f, 1.f, 1.f });
                if (ui.Checkbox("Include child hierarchy", &raw->showChildHierarchy))
                {
                    ApplyObjectStageVisibility(*raw);
                    if (!raw->showChildHierarchy)
                    {
                        raw->scene->SetSelectedObject(raw->subject);
                        if (m_primaryHierarchy)
                            m_primaryHierarchy->SetSelectedObject(raw->subject);
                        if (m_primaryProperties)
                            m_primaryProperties->SetSelectedObject(raw->subject);
                    }
                    if (m_renderer) m_renderer->MarkDirty();
                }
}

}
