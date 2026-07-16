#include "PropertiesView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Camera.h"

void PropertiesView::DrawPanel(IEditorUi& ui)
{
    ui.BeginWindow(m_title.c_str(), &m_open);
    if (!m_selectedObject) { ui.DisabledLabel("No object selected"); ui.EndWindow(); return; }
    char name[256]; strncpy_s(name, m_selectedObject->name.c_str(), sizeof(name));
    if (ui.InputText("##name", name, sizeof(name))) m_selectedObject->name = name;
    ui.Separator(); DrawTransform(ui); DrawMesh(ui); DrawMaterial(ui); DrawCamera(ui); ui.EndWindow();
}

void PropertiesView::DrawTransform(IEditorUi& ui)
{
    Transform& t=m_selectedObject->transform;
    if(ui.CollapsingHeader("Transform")){ui.DragFloat3("Position",&t.position.x,.01f);glm::vec3 d=glm::degrees(t.rotation);if(ui.DragFloat3("Rotation",&d.x,.5f))t.rotation=glm::radians(d);ui.DragFloat3("Scale",&t.scale.x,.01f,.001f,1000.f);}
}
void PropertiesView::DrawMesh(IEditorUi& ui)
{
    Mesh* m=m_selectedObject->GetComponent<Mesh>();if(!m)return;if(ui.CollapsingHeader("Mesh")){std::string count=std::to_string(m->GetVertexCount());ui.ValueLabel("Vertices",count.c_str());ui.ValueLabel("Ready",m->IsReady()?"Yes":"No");}
}
void PropertiesView::DrawMaterial(IEditorUi& ui)
{
    Material* m=m_selectedObject->GetComponent<Material>();if(!m)return;if(ui.CollapsingHeader("Material")){ui.ColorEdit3("Diffuse",&m->diffuseColor.r);ui.ColorEdit3("Ambient",&m->ambientColor.r);ui.ColorEdit3("Specular",&m->specularColor.r);ui.DragFloat("Shininess",&m->shininess,.5f,1.f,256.f);}
}
void PropertiesView::DrawCamera(IEditorUi& ui)
{
    Camera* c=m_selectedObject->GetComponent<Camera>();if(!c)return;if(ui.CollapsingHeader("Camera")){ui.DragFloat("FOV",&c->fov,.5f,1.f,179.f);ui.DragFloat("Near Plane",&c->nearPlane,.001f,.001f,10.f);ui.DragFloat("Far Plane",&c->farPlane,1.f,1.f,10000.f);ui.DragFloat3("Target",&c->target.x,.01f);}
}
