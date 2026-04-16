#include "PropertiesView.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Mesh.h"
#include "Core/Compoonents/Material.h"
#include "Core/Compoonents/Camera.h"

// ---------------------------------------------------------------------------
// DrawPanel
// ---------------------------------------------------------------------------
void PropertiesView::DrawPanel()
{
    ImGui::Begin(m_title.c_str(), &m_open);

    if (!m_selectedObject)
    {
        ImGui::TextDisabled("No object selected");
        ImGui::End();
        return;
    }

    // ---- Object name ----
    char nameBuf[256];
    strncpy_s(nameBuf, m_selectedObject->name.c_str(), sizeof(nameBuf));
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
        m_selectedObject->name = nameBuf;

    ImGui::Separator();

    DrawTransform();
    DrawMesh();
    DrawMaterial();
    DrawCamera();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// DrawTransform
// ---------------------------------------------------------------------------
void PropertiesView::DrawTransform()
{
    // Transform is a direct member of Object, always present.
    Transform& t = m_selectedObject->transform;

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::DragFloat3("Position", &t.position.x, 0.01f);

        // Display rotation in degrees; store in radians.
        glm::vec3 deg = glm::degrees(t.rotation);
        if (ImGui::DragFloat3("Rotation", &deg.x, 0.5f))
            t.rotation = glm::radians(deg);

        ImGui::DragFloat3("Scale", &t.scale.x, 0.01f, 0.001f, 1000.f);
    }
}

// ---------------------------------------------------------------------------
// DrawMesh
// ---------------------------------------------------------------------------
void PropertiesView::DrawMesh()
{
    Mesh* mesh = m_selectedObject->GetComponent<Mesh>();
    if (!mesh) return;

    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::LabelText("Vertices", "%u", mesh->GetVertexCount());
        ImGui::LabelText("Ready",    "%s", mesh->IsReady() ? "Yes" : "No");
    }
}

// ---------------------------------------------------------------------------
// DrawMaterial
// ---------------------------------------------------------------------------
void PropertiesView::DrawMaterial()
{
    Material* mat = m_selectedObject->GetComponent<Material>();
    if (!mat) return;

    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::ColorEdit3("Diffuse",  &mat->diffuseColor.r);
        ImGui::ColorEdit3("Ambient",  &mat->ambientColor.r);
        ImGui::ColorEdit3("Specular", &mat->specularColor.r);
        ImGui::DragFloat("Shininess", &mat->shininess, 0.5f, 1.f, 256.f);
    }
}

// ---------------------------------------------------------------------------
// DrawCamera
// ---------------------------------------------------------------------------
void PropertiesView::DrawCamera()
{
    Camera* cam = m_selectedObject->GetComponent<Camera>();
    if (!cam) return;

    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::DragFloat("FOV",        &cam->fov,       0.5f,  1.f,  179.f);
        ImGui::DragFloat("Near Plane", &cam->nearPlane, 0.001f, 0.001f, 10.f);
        ImGui::DragFloat("Far Plane",  &cam->farPlane,  1.f,    1.f,   10000.f);
        ImGui::DragFloat3("Target",    &cam->target.x,  0.01f);
    }
}
