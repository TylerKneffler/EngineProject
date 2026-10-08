#include "pch.h"
#include "Engine/Editor/EditorState.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include "Engine/Editor/Core/View/Views/ConsoleView.h"
#include "Core/Scene/Scene.h"
#include "Core/Object.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/MeshEditGeometry.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>

namespace Engine::Editor
{
namespace
{
std::string CaptureMeshSnapshot(const Engine::Components::Mesh& mesh)
{
    const auto& vertices = mesh.GetVertices();
    const auto& indices = mesh.GetIndices();
    const uint32_t counts[]{ static_cast<uint32_t>(vertices.size()),
        static_cast<uint32_t>(indices.size()) };
    std::string result(reinterpret_cast<const char*>(counts), sizeof(counts));
    result.append(reinterpret_cast<const char*>(vertices.data()),
        vertices.size() * sizeof(Engine::Components::Mesh::Vertex));
    result.append(reinterpret_cast<const char*>(indices.data()),
        indices.size() * sizeof(uint32_t));
    return result;
}

std::string AssetPathIdentity(const std::string& path)
{
    std::error_code error;
    std::string identity = std::filesystem::weakly_canonical(path, error)
        .generic_string();
    if (error) identity = std::filesystem::path(path).lexically_normal()
        .generic_string();
#ifdef _WIN32
    std::transform(identity.begin(), identity.end(), identity.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
    return identity;
}
}
void EditorState::DrawMeshEditTools(IEditorUi& ui)
{
    MeshEditSession* session = ActiveMeshEditSession();
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    if (!session || !session->enabled || !scene)
    {
        ui.DisabledLabel("Choose Mesh in a Scene or Prefab mode selector.");
        return;
    }
    SyncMeshEditSelection(scene, *session);
    Engine::Components::Mesh* mesh = session->activeMesh;
    if (!mesh)
    {
        ui.DisabledLabel("Select an object with a mesh in the hierarchy.");
        return;
    }
    const bool directTool = !m_toolbarDirectTool.empty();
    const bool toolPopup = directTool || !m_toolbarPopupTool.empty();
    if (!toolPopup)
    {
        ui.ValueLabel("Active mesh", mesh->Owner ? mesh->Owner->name.c_str() : "Mesh");
        ui.ValueLabel("Shared asset", session->savePath.empty()
            ? "Save the scene to choose a mesh path" : session->savePath.c_str());
        ui.SearchInput("Find tool", session->search, sizeof(session->search),
            "Search mesh tools");
        const char* modes[]{ "Vertex", "Edge", "Face" };
        if (ui.Combo("Selection", &session->selectionMode, modes, 3))
        {
            session->selectedElement = 0;
            session->selectedElements = { 0 };
            session->gizmoDragging = false;
            session->gizmoStartPositions.clear();
        }
        const char* selectionTools[]{ "Click", "Box", "Lasso" };
        ui.Combo("Select with", &session->selectionTool, selectionTools, 3);
        ui.DisabledLabel("Drag in viewport; Ctrl toggles selection.");
        const std::string selectionCount = std::to_string(
            session->selectedElements.size());
        ui.ValueLabel("Selected", selectionCount.c_str());
    }
    std::optional<MeshEditGeometry> geometry;
    const auto draft = [&]() -> MeshEditGeometry&
    {
        if (!geometry) geometry = MeshEditGeometry::FromMesh(*mesh);
        return *geometry;
    };
    const auto& storedIndices = mesh->GetIndices();
    const uint32_t count = session->selectionMode == 0
        ? static_cast<uint32_t>(mesh->GetVertices().size())
        : (session->selectionMode == 1
            ? static_cast<uint32_t>(CachedMeshEdges(*session, *mesh).size())
            : static_cast<uint32_t>((storedIndices.empty()
                ? mesh->GetVertices().size() : storedIndices.size()) / 3));
    if (count)
    {
        session->selectedElement = std::min<uint32_t>(
            session->selectedElement, count - 1);
        int selected = static_cast<int>(std::min<uint32_t>(
            session->selectedElement, count - 1));
        if (!toolPopup && count > 1 && ui.SliderInt("Element", &selected, 0,
            static_cast<int>(count - 1)))
        {
            session->selectedElement = static_cast<uint32_t>(selected);
            session->selectedElements = { session->selectedElement };
        }
    }
    if (!directTool && !session->toolError.empty())
        ui.DisabledLabel(session->toolError.c_str());
    if (!toolPopup) ui.Separator();
    const auto matches = [&](const char* name)
    {
        if (directTool) return m_toolbarDirectTool == name;
        if (toolPopup) return m_toolbarPopupTool == name;
        std::string filter(session->search), label(name);
        std::transform(filter.begin(), filter.end(), filter.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::transform(label.begin(), label.end(), label.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return label.find(filter) != std::string::npos;
    };
    const auto apply = [&](MeshEditGeometry&& edit)
    {
        if (!Engine::Components::Mesh::ValidateAuthoredGeometry(
                edit.vertices, edit.indices)) return false;
        const uint32_t nextCount = session->selectionMode == 0
            ? static_cast<uint32_t>(edit.vertices.size())
            : (session->selectionMode == 1 ? edit.EdgeCount() : edit.FaceCount());
        const std::string before = CaptureMeshSnapshot(*mesh);
        if (!mesh->SetIndexedGeometry(std::move(edit.vertices),
                std::move(edit.indices))) return false;
        if (!session->savePath.empty())
            mesh->SetAuthoredFilePath(session->savePath);
        session->selectedElements.erase(std::remove_if(
            session->selectedElements.begin(), session->selectedElements.end(),
            [nextCount](uint32_t id) { return id >= nextCount; }),
            session->selectedElements.end());
        if (nextCount)
            session->selectedElement = std::min(session->selectedElement,
                nextCount - 1);
        if (m_historyLimit > 0 && session->undo.size() >= m_historyLimit)
            session->undo.pop_front();
        if (m_historyLimit > 0) session->undo.push_back(before);
        session->redo.clear();
        session->baseline = CaptureMeshSnapshot(*mesh);
        session->dirty = session->baseline != session->savedSnapshot;
        if (!session->savePath.empty())
        {
            MeshEditSession cached = *session;
            cached.activeMesh = nullptr;
            m_meshEditCache[AssetPathIdentity(session->savePath)] =
                std::move(cached);
        }
        if (m_renderer) m_renderer->MarkDirty();
        return true;
    };
    const auto selected = [&]()
    {
        return session->selectedElements;
    };
    const auto run = [&](const char* error, bool success)
    {
        if (success && geometry && apply(std::move(*geometry)))
            session->toolError.clear();
        else session->toolError = error;
    };
    const auto trigger = [&](const char* label)
    {
        return directTool ? m_toolbarDirectTool == label : ui.Button(label);
    };
    if (matches("Undo") && ui.Button("Undo mesh edit"))
        ApplyMeshHistory(false);
    if (matches("Redo") && ui.Button("Redo mesh edit"))
        ApplyMeshHistory(true);
    if (count && matches("Transform"))
    {
        ui.DragFloat3("Translate", session->move, .01f);
        ui.DragFloat3("Rotate degrees", session->rotate, 1.f);
        ui.DragFloat3("Scale", session->scale, .01f);
        const char* pivots[]{ "Median", "Bounds", "Origin", "Custom" };
        ui.Combo("Pivot", &session->pivotMode, pivots, 4);
        if (session->pivotMode == 3)
            ui.DragFloat3("Custom pivot", session->customPivot, .01f);
        ui.DragFloat("Translation snap (0 off)", &session->snapStep, .01f);
        ui.DragFloat("Rotation snap (0 off)", &session->rotationSnap, 1.f);
        ui.DragFloat("Scale snap (0 off)", &session->scaleSnap, .01f);
        if (ui.Button("Transform selection"))
        {
            run("Select a valid element and use positive scale values.",
                draft().TransformSelection(session->selectionMode, selected(),
                    { session->move[0], session->move[1], session->move[2] },
                    { session->rotate[0], session->rotate[1], session->rotate[2] },
                    { session->scale[0], session->scale[1], session->scale[2] },
                    session->pivotMode,
                    { session->customPivot[0], session->customPivot[1],
                        session->customPivot[2] }, session->snapStep,
                    session->rotationSnap, session->scaleSnap));
        }
    }
    if (mesh->HasMorphTargets())
    {
        if (!directTool)
            ui.DisabledLabel("Topology tools require a mesh without morph targets.");
    }
    else if (session->selectionMode == 1 && count && matches("Split edge"))
    {
        if (trigger("Split edge"))
            run("Select an edge to split.", draft().SplitEdge(session->selectedElement));
    }
    else if (session->selectionMode == 2 && count)
    {
        if (matches("Extrude face"))
        {
            ui.DragFloat("Extrude distance", &session->extrudeDistance, .01f);
            if (ui.Button("Extrude face"))
                run("Select a nondegenerate face and a nonzero distance.",
                    draft().ExtrudeFace(session->selectedElement,
                        session->extrudeDistance));
        }
        if (matches("Inset face"))
        {
            ui.SliderFloat("Inset amount", &session->insetAmount, .01f, .95f);
            if (ui.Button("Inset face"))
                run("Select a face and an inset between 0 and 1.",
                    draft().InsetFace(session->selectedElement,
                        session->insetAmount));
        }
    }
    if (!mesh->HasMorphTargets() && count)
    {
        if (session->selectionMode == 1)
        {
            if (matches("Bevel edge"))
            {
                ui.SliderFloat("Bevel width", &session->bevelWidth, .01f, .49f);
                if (ui.Button("Bevel edge"))
                    run("Bevel requires an interior edge shared by two faces.",
                        draft().BevelEdge(session->selectedElement,
                            session->bevelWidth));
            }
            if (matches("Loop cut") && trigger("Loop cut"))
                run("Loop cut requires a planar quad strip edge.",
                    draft().LoopCut(session->selectedElement));
            if (matches("Fill boundary") && trigger("Fill boundary"))
                run("Fill requires a selected convex planar boundary loop.",
                    draft().FillBoundary(selected()));
            if (matches("Bridge boundaries") && trigger("Bridge boundaries"))
                run("Bridge requires two selected boundary loops of equal size.",
                    draft().BridgeBoundaries(selected()));
        }
        if (session->selectionMode == 0 && matches("Weld vertices") &&
            trigger("Weld vertices"))
            run("Weld requires at least two selected vertices and a remaining face.",
                draft().WeldVertices(selected()));
        if (session->selectionMode == 2 && matches("Duplicate faces") &&
            ui.Button("Duplicate faces"))
            run("Select faces to duplicate.", draft().DuplicateFaces(selected(),
                { session->move[0], session->move[1], session->move[2] }));
        if (matches("Delete selection") && trigger("Delete selection"))
            run("Delete must leave at least one face in the mesh.",
                draft().DeleteSelection(session->selectionMode, selected()));
    }
    if (!toolPopup)
    {
        ui.Separator();
        if (ui.Button(session->dirty ? "Save Mesh *" : "Save Mesh"))
            SaveMeshEditSession(*session);
    }
}

void EditorState::DrawMeshStageTools(IEditorUi& ui,
    SceneAssetDocument& document)
{
    SceneAssetDocument* raw = &document;
                if (!raw->meshStage) return;
                const bool uvMapOnly = m_toolbarPopupTool == "UV Map";
                const bool attributesOnly =
                    m_toolbarPopupTool == "Vertex Attributes";
                std::vector<Engine::Components::Mesh*> meshes;
                for (const auto& object : raw->scene->GetObjects())
                    if (object)
                        for (Engine::Core::Component* component : object->Components)
                            if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(component);
                                mesh && !mesh->GetVertices().empty())
                                meshes.push_back(mesh);
                if (meshes.size() > 1)
                {
                    std::vector<std::string> meshLabels;
                    std::vector<const char*> meshNames;
                    int selectedMesh = 0;
                    for (size_t index = 0; index < meshes.size(); ++index)
                    {
                        const std::string label = (meshes[index]->Owner
                            ? meshes[index]->Owner->name : std::string("Mesh")) +
                            " [" + std::to_string(index + 1) + "]";
                        meshLabels.push_back(label);
                        if (meshes[index] == raw->mesh)
                            selectedMesh = static_cast<int>(index);
                    }
                    for (const std::string& label : meshLabels)
                        meshNames.push_back(label.c_str());
                    if (ui.Combo("Mesh", &selectedMesh, meshNames.data(),
                        static_cast<int>(meshNames.size())) && selectedMesh >= 0 &&
                        static_cast<size_t>(selectedMesh) < meshes.size())
                    {
                        raw->mesh = meshes[static_cast<size_t>(selectedMesh)];
                        raw->subject = raw->mesh->Owner;
                        raw->selectedVertex = 0;
                        raw->meshSavePath = raw->mesh->GetFilePath();
                        if (raw->meshSavePath.empty())
                            raw->meshSavePath =
                                (std::filesystem::path(raw->path).parent_path() /
                                    (raw->subject->name + ".mesh")).string();
                        raw->scene->SetSelectedObject(raw->subject);
                        ApplyMeshStageVisibility(*raw);
                        SetActiveSceneAssetDocument(raw, true);
                        if (m_renderer) m_renderer->MarkDirty();
                    }
                }
                if (!raw->mesh || raw->mesh->GetVertices().empty())
                {
                    ui.ColoredLabel("Mesh has no editable triangle-list vertices.",
                        { 1.f, 0.6f, 0.25f, 1.f });
                    return;
                }
                std::vector<Engine::Components::Mesh::Vertex> vertices =
                    raw->mesh->GetVertices();
                ui.ColoredLabel(uvMapOnly ? "UV Map" : "Vertex Attributes",
                    { 0.35f, 0.75f, 1.f, 1.f });
                ui.ValueLabel("Save target", raw->meshSavePath.c_str());
                const std::string count = std::to_string(vertices.size());
                ui.ValueLabel("Triangle-list vertices", count.c_str());
                int selected = static_cast<int>(std::min<uint32_t>(
                    raw->selectedVertex, static_cast<uint32_t>(vertices.size() - 1)));
                if (vertices.size() > 1)
                    ui.SliderInt("Vertex", &selected, 0,
                        static_cast<int>(vertices.size() - 1));
                raw->selectedVertex = static_cast<uint32_t>(selected);
                auto& vertex = vertices[raw->selectedVertex];
                bool changed = false;
                if (!uvMapOnly)
                    changed |= ui.DragFloat3("Position", vertex.pos, 0.01f);
                changed |= ui.DragFloat("UV U", &vertex.uv[0], 0.005f);
                changed |= ui.DragFloat("UV V", &vertex.uv[1], 0.005f);
                if (!uvMapOnly)
                    changed |= ui.ColorEdit4("Vertex Color", vertex.color);
                const auto& indices = raw->mesh->GetIndices();
                if (!attributesOnly)
                {
                std::vector<float> uvPairs;
                uvPairs.reserve(vertices.size() * 2);
                for (const auto& item : vertices)
                {
                    uvPairs.push_back(item.uv[0]);
                    uvPairs.push_back(item.uv[1]);
                }
                const EditorUiUvMapResult uvResult = ui.UvMapEditor(
                    "MeshUVMap", uvPairs.data(), vertices.size(),
                    indices.data(), indices.size(), &selected, 280.f);
                if (selected >= 0)
                    raw->selectedVertex = static_cast<uint32_t>(selected);
                const EditorUiContextMenuResult uvContext = ui.ContextMenu(
                    raw, "Reset Selected UV", nullptr, false);
                if (uvContext.addRequested && raw->selectedVertex < vertices.size())
                {
                    vertices[raw->selectedVertex].uv[0] = 0.5f;
                    vertices[raw->selectedVertex].uv[1] = 0.5f;
                    uvPairs[static_cast<size_t>(raw->selectedVertex) * 2] = 0.5f;
                    uvPairs[static_cast<size_t>(raw->selectedVertex) * 2 + 1] = 0.5f;
                    changed = true;
                }
                if (uvResult.coordinatesChanged)
                {
                    for (size_t index = 0; index < vertices.size(); ++index)
                    {
                        vertices[index].uv[0] = uvPairs[index * 2];
                        vertices[index].uv[1] = uvPairs[index * 2 + 1];
                    }
                    changed = true;
                }
                }
                if (!uvMapOnly)
                {
                int selectedInfluence = static_cast<int>(
                    std::min(raw->selectedInfluence, 7u));
                if (ui.SliderInt("Skin influence", &selectedInfluence, 0, 7))
                    raw->selectedInfluence = static_cast<uint32_t>(selectedInfluence);
                const uint32_t influenceIndex = raw->selectedInfluence % 4u;
                float* joint = raw->selectedInfluence < 4u
                    ? &vertex.joints0[influenceIndex]
                    : &vertex.joints1[influenceIndex];
                float* weight = raw->selectedInfluence < 4u
                    ? &vertex.weights0[influenceIndex]
                    : &vertex.weights1[influenceIndex];
                changed |= ui.DragFloat("Joint palette index", joint, 1.f, 0.f, 255.f);
                changed |= ui.SliderFloat("Influence weight", weight, 0.f, 1.f);
                if (changed)
                {
                    float total = 0.f;
                    for (float value : vertex.weights0) total += value;
                    for (float value : vertex.weights1) total += value;
                    if (total > 0.f)
                    {
                        for (float& value : vertex.weights0) value /= total;
                        for (float& value : vertex.weights1) value /= total;
                    }
                }
                }
                if (changed)
                {
                    const bool applied = indices.empty()
                        ? raw->mesh->SetAuthoredVertices(std::move(vertices))
                        : raw->mesh->SetIndexedGeometry(std::move(vertices),
                            std::vector<uint32_t>(indices));
                    if (applied && m_renderer) m_renderer->MarkDirty();
                    if (!applied && m_primaryConsole)
                        m_primaryConsole->AddLog(ConsoleView::Level::Error,
                            "Mesh edit rejected: invalid geometry or attributes.");
                }
                if (m_toolbarPopupTool.empty() && ui.Button("Save Mesh"))
                    SaveSceneAssetDocument(*raw);
}

}
