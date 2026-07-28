#include "PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Camera.h"

void PropertiesView::DrawPanel(IEditorUi& ui)
{
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }
    if (!m_selectedObject) { ui.DisabledLabel("No object selected"); ui.EndWindow(); return; }
    Object* prefabRoot = m_selectedObject->GetPrefabInstanceRoot();
    const bool linked = prefabRoot != nullptr;
    char name[256]; strncpy_s(name, m_selectedObject->name.c_str(), sizeof(name));
    ui.BeginDisabled(linked);
    if (ui.InputText("##name", name, sizeof(name))) m_selectedObject->name = name;
    ui.EndDisabled();
    if (prefabRoot && prefabRoot->Prefab)
    {
        ui.ValueLabel("Prefab", prefabRoot->Prefab->GetPath().c_str());
        ui.DisabledLabel(
            m_selectedObject == prefabRoot
                ? "Properties come from the prefab; root transform is instance placement."
                : "Properties and transform come from the prefab asset.");
    }
    else
        ui.DisabledLabel("Scene-only object");
    ui.Separator(); DrawTransform(ui); DrawMesh(ui); DrawMaterial(ui); DrawCamera(ui); ui.EndWindow();
}

void PropertiesView::DrawTransform(IEditorUi& ui)
{
    Transform& t=m_selectedObject->transform;
    const Object* prefabRoot = m_selectedObject->GetPrefabInstanceRoot();
    const bool locked = prefabRoot && prefabRoot != m_selectedObject;
    if(ui.CollapsingHeader("Transform")){ui.BeginDisabled(locked);ui.DragFloat3("Position",&t.position.x,.01f);glm::vec3 d=glm::degrees(t.rotation);if(ui.DragFloat3("Rotation",&d.x,.5f))t.rotation=glm::radians(d);ui.DragFloat3("Scale",&t.scale.x,.01f,.001f,1000.f);ui.EndDisabled();}
}
void PropertiesView::DrawMesh(IEditorUi& ui)
{
    Mesh* m=m_selectedObject->GetComponent<Mesh>();if(!m)return;if(ui.CollapsingHeader("Mesh")){ui.BeginDisabled(m_selectedObject->IsPartOfPrefabInstance());ui.ValueLabel("Asset",m->GetFilePath().c_str());std::string count=std::to_string(m->GetVertexCount());ui.ValueLabel("Vertices",count.c_str());ui.ValueLabel("Ready",m->IsReady()?"Yes":"No");ui.EndDisabled();}
}
void PropertiesView::DrawMaterial(IEditorUi& ui)
{
    Material* m=m_selectedObject->GetComponent<Material>();if(!m)return;if(ui.CollapsingHeader("Material")){ui.BeginDisabled(m_selectedObject->IsPartOfPrefabInstance());ui.ColorEdit3("Diffuse",&m->diffuseColor.r);ui.ColorEdit3("Ambient",&m->ambientColor.r);ui.ColorEdit3("Specular",&m->specularColor.r);ui.DragFloat("Shininess",&m->shininess,.5f,1.f,256.f);ui.EndDisabled();}
}
void PropertiesView::DrawCamera(IEditorUi& ui)
{
    Camera* c=m_selectedObject->GetComponent<Camera>();if(!c)return;if(ui.CollapsingHeader("Camera")){ui.BeginDisabled(m_selectedObject->IsPartOfPrefabInstance());ui.DragFloat("FOV",&c->fov,.5f,1.f,179.f);ui.DragFloat("Near Plane",&c->nearPlane,.001f,.001f,10.f);ui.DragFloat("Far Plane",&c->farPlane,1.f,1.f,10000.f);ui.DragFloat3("Target",&c->target.x,.01f);ui.EndDisabled();}
}
