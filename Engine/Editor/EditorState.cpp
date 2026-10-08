#include "pch.h"
#include "EditorState.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/IEditorPanel.h"
#include "Core/View/View.h"
#include "Core/View/Views/PreferencesView.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/View/Views/SceneView.h"
#include "Core/View/Views/GameView.h"
#include "Core/View/Views/AssetDocumentView.h"
#include "Core/Window.h"
#include "Core/Renderers/RendererFactory.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Animation/AnimationBone.h"
#include "Core/Compoonents/Animation/SkinnedMesh.h"
#include "Core/Compoonents/Animation/AnimationManager.h"
#include "Core/Compoonents/Animation/Model.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Compoonents/Physics/RigidBody.h"
#include "Core/Compoonents/Obj/Sprite.h"
#include "Core/Compoonents/Sprite/SpriteAnimationManager.h"
#include "Core/AssetRecord.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/Importers/ModelImporter.h"
#include "Core/MeshEditGeometry.h"
#include "Engine/Editor/Core/View/Templates/Assets/AssetPreviewCache.h"
#include <chrono>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <numeric>
#include <optional>
#include <set>
#include <unordered_set>
#include <commdlg.h>
#include <glm/gtc/matrix_inverse.hpp>

namespace Engine::Editor
{
namespace
{
class EditToolsPanel final : public IEditorPanel
{
public:
    std::function<void(IEditorUi&)> DrawTools;
    EditToolsPanel()
    {
        SetTitle("Edit Tools");
        SetDefaultDockArea(EditorPanelDockArea::RightSidebar);
        SetOpen(false);
    }
    void DrawPanel(IEditorUi& ui) override
    {
        if (!ui.BeginWindow(m_title.c_str(), &m_open))
        {
            ui.EndWindow();
            return;
        }
        if (ui.IsWindowFocused() && OnFocused) OnFocused();
        if (DrawTools) DrawTools(ui);
        ui.EndWindow();
    }
};

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

bool RestoreMeshSnapshot(Engine::Components::Mesh& mesh,
    const std::string& snapshot)
{
    using Vertex = Engine::Components::Mesh::Vertex;
    if (snapshot.size() < sizeof(uint32_t) * 2) return false;
    uint32_t counts[2]{};
    std::memcpy(counts, snapshot.data(), sizeof(counts));
    const size_t vertexBytes = static_cast<size_t>(counts[0]) * sizeof(Vertex);
    const size_t indexBytes = static_cast<size_t>(counts[1]) * sizeof(uint32_t);
    if (!counts[0] || vertexBytes > snapshot.size() - sizeof(counts) ||
        indexBytes != snapshot.size() - sizeof(counts) - vertexBytes)
        return false;
    std::vector<Vertex> vertices(counts[0]);
    std::vector<uint32_t> indices(counts[1]);
    std::memcpy(vertices.data(), snapshot.data() + sizeof(counts), vertexBytes);
    std::memcpy(indices.data(), snapshot.data() + sizeof(counts) + vertexBytes,
        indexBytes);
    return indices.empty()
        ? mesh.SetAuthoredVertices(std::move(vertices))
        : mesh.SetIndexedGeometry(std::move(vertices), std::move(indices));
}

struct ViewportModeControl
{
    bool changed = false;
    bool showSave = false;
};

ViewportModeControl DrawViewportModeControl(IEditorUi& ui, int& mode)
{
    const float available = ui.AvailableContentWidth();
    const bool compact = available < 125.f;
    const bool showSave = available >= 175.f;
    ui.SetNextItemWidth(std::max(48.f,
        available - (compact ? 0.f : 47.f) - (showSave ? 56.f : 0.f)));
    const char* choices[]{ "Object", "Mesh" };
    const bool changed = ui.Combo(compact ? "##Mode" : "Mode",
        &mode, choices, 2);
    if (compact) ui.Tooltip("Editor mode");
    return { changed, showSave };
}
}
namespace
{
    void LogStartupFailure(const std::string& message)
    {
        std::ofstream log("editor-startup.log", std::ios::app);
        if (log)
            log << message << '\n';
    }

    std::string NormalizeAssetPath(const std::string& path)
    {
        std::error_code error;
        std::filesystem::path normalized =
            std::filesystem::weakly_canonical(path, error);
        if (error)
            normalized = std::filesystem::path(path).lexically_normal();
        return normalized.generic_string();
    }

    std::string AssetPathIdentity(const std::string& path)
    {
        std::string identity = NormalizeAssetPath(path);
#ifdef _WIN32
        std::transform(identity.begin(), identity.end(), identity.begin(),
            [](unsigned char value)
            {
                return static_cast<char>(std::tolower(value));
            });
#endif
        return identity;
    }

    bool ReplaceAllInText(std::string& text, const std::string& from,
        const std::string& to)
    {
        if (from.empty() || from == to)
            return false;
        bool replaced = false;
        size_t position = 0;
        while ((position = text.find(from, position)) != std::string::npos)
        {
            text.replace(position, from.size(), to);
            position += to.size();
            replaced = true;
        }
        return replaced;
    }

    std::string RemapPathPrefix(const std::string& value,
        const std::string& oldPrefix, const std::string& newPrefix)
    {
        if (value == oldPrefix)
            return newPrefix;
        if (value.size() <= oldPrefix.size() ||
            value.compare(0, oldPrefix.size(), oldPrefix) != 0)
            return value;

        const char separator = value[oldPrefix.size()];
        if (separator != '/' && separator != '\\')
            return value;

        std::string remapped = newPrefix;
        remapped += value.substr(oldPrefix.size());
        return remapped;
    }
}

// ---------------------------------------------------------------------------
// EditorState::EditorState
// ---------------------------------------------------------------------------
EditorState::EditorState(HINSTANCE hInstance, const Engine::Model::ProjectSettings& projectSettings,
    std::string projectFilePath)
    : m_projectSettings(projectSettings)
    , m_projectFilePath(std::move(projectFilePath))
    , m_historyLimit(std::min(projectSettings.editorHistoryLimit, 1000u))
{
    try
    {
        m_window = std::make_unique<::Engine::Core::Window>(hInstance, L"Engine Editor", 1280, 720);
    }
    catch (const std::exception&)
    {
        // Window creation failed
    }
}

// ---------------------------------------------------------------------------
// EditorState::~EditorState
// ---------------------------------------------------------------------------
EditorState::~EditorState()
{
}

bool EditorState::HasUnsavedChanges() const
{
    if (m_hasUnsavedChanges || m_prefabHasUnsavedChanges)
        return true;
    if (m_mainMeshEdit.dirty || m_prefabMeshEdit.dirty)
        return true;
    for (const auto& [path, edit] : m_meshEditCache)
        if (edit.dirty) return true;
    for (const auto& document : m_sceneAssetDocuments)
        if (document && (document->dirty || document->meshEdit.dirty))
            return true;
    for (AssetDocumentView* document : m_assetDocuments)
        if (document && document->IsDirty())
            return true;
    return false;
}

bool EditorState::HasActiveDocumentUnsavedChanges() const
{
    if (m_activeSceneAssetDocument)
        return m_activeSceneAssetDocument->dirty ||
            m_activeSceneAssetDocument->meshEdit.dirty;
    if (m_prefabDocumentFocused && !m_activePrefabPath.empty())
        return m_prefabHasUnsavedChanges || m_prefabMeshEdit.dirty;
    if (m_activeAssetDocument)
        return m_activeAssetDocument->IsDirty();
    return m_hasUnsavedChanges || m_mainMeshEdit.dirty;
}

bool EditorState::IsEditingPrefab() const
{
    return (m_activeSceneAssetDocument && m_activeSceneAssetDocument->prefab) ||
        (!m_activePrefabPath.empty() && m_prefabDocumentFocused);
}

// ---------------------------------------------------------------------------
// EditorState::Init
// ---------------------------------------------------------------------------
bool EditorState::Init()
{
    // Window was created in constructor; get its handle
    if (!m_window)
    {
        LogStartupFailure("EditorState: window creation failed");
        return false;
    }

    HWND hwnd = m_window->GetHWND();
    if (!hwnd)
    {
        LogStartupFailure("EditorState: window handle is invalid");
        return false;
    }

    // Initialize renderer
    OutputDebugStringA("[EditorState] Creating renderer...\n");
    try
    {
        m_renderer = ::Engine::Renderers::RendererFactory::CreateEditorRenderer(m_projectSettings);
    }
    catch (const std::exception& e)
    {
        std::string api = m_projectSettings.editorRenderingAPI;
        std::string msg = "Failed to create the " + api + " renderer.\n\n"
            "Please ensure " + api + " is installed and your GPU supports it.\n\n"
            "Details: " + e.what();
        MessageBoxA(hwnd, msg.c_str(), "Renderer Initialization Error", MB_OK | MB_ICONERROR);
        return false;
    }
    if (!m_renderer)
    {
        std::string api = m_projectSettings.editorRenderingAPI;
        std::string msg = "Failed to create the " + api + " renderer.\n\n"
            "Please ensure " + api + " is installed and your GPU supports it.";
        MessageBoxA(hwnd, msg.c_str(), "Renderer Initialization Error", MB_OK | MB_ICONERROR);
        return false;
    }
    
    OutputDebugStringA("[EditorState] Initializing renderer...\n");
    if (!m_renderer->Init(hwnd, 1280, 720))
    {
        LogStartupFailure("EditorState: renderer Init returned false");
        std::string api = m_projectSettings.editorRenderingAPI;
        std::string msg = "Failed to initialize the " + api + " renderer.\n\n"
            "Please ensure " + api + " is installed and your GPU driver is up to date.";
        MessageBoxA(hwnd, msg.c_str(), "Renderer Initialization Error", MB_OK | MB_ICONERROR);
        return false;
    }
    OutputDebugStringA("[EditorState] Renderer initialized\n");

    // Initialize scene
    OutputDebugStringA("[EditorState] Creating scene...\n");
    m_scene = std::make_unique<Engine::Scene::Scene>();
    if (!m_scene)
        return false;
    OutputDebugStringA("[EditorState] Scene created\n");
    
    OutputDebugStringA("[EditorState] Initializing scene...\n");
    Engine::Graphics::IGraphicsProvider* graphicsProvider = m_renderer->GetGraphicsProvider();
    if (!graphicsProvider)

    {
        LogStartupFailure("EditorState: renderer did not provide a graphics provider");
        OutputDebugStringA("[EditorState] ERROR: Failed to get graphics provider from renderer\n");
        return false;
    }
    
    try
    {
        m_scene->Init(graphicsProvider);
        m_scene->SetRealtimeShadowSettings(m_projectSettings.realtimeShadows);
        m_scene->SetDistanceLightingSettings(m_projectSettings.distanceLighting);
        OutputDebugStringA("[EditorState] Scene initialized\n");
    }
    catch (const std::exception& e)
    {
        LogStartupFailure(std::string("EditorState: scene initialization failed: ") + e.what());
        std::string errorMsg = "[EditorState] ERROR: Scene initialization failed: ";
        errorMsg += e.what();
        errorMsg += "\n";
        OutputDebugStringA(errorMsg.c_str());
        return false;
    }

    // Initialize view factory
    OutputDebugStringA("[EditorState] Creating view factory...\n");
    m_viewFactory = std::make_unique<ViewFactory>(m_renderer.get(), m_scene.get(), m_projectSettings);
    if (!m_viewFactory)
        return false;
    OutputDebugStringA("[EditorState] View factory created\n");

    // Initialize timing
    QueryPerformanceFrequency(&m_perfFreq);
    QueryPerformanceCounter(&m_lastCounter);
    m_deltaTime = 0.0f;
    OutputDebugStringA("[EditorState] Timing initialized\n");

    OutputDebugStringA("[EditorState] Init completed\n");
    return true;
}

void EditorState::InitializeUiState()
{
    if (!m_panels.empty() || m_preferences)
        return;
    WireupCallbacks();
    InitializePanels();
}

// ---------------------------------------------------------------------------
// EditorState::SaveScene
// ---------------------------------------------------------------------------
EditorState::MeshEditSession* EditorState::ActiveMeshEditSession()
{
    if (m_activeSceneAssetDocument)
        return &m_activeSceneAssetDocument->meshEdit;
    if (m_prefabDocumentFocused && m_prefabScene)
        return &m_prefabMeshEdit;
    return &m_mainMeshEdit;
}

Engine::Components::Mesh* EditorState::ResolveMeshForSelection(
    Engine::Core::Object* selected) const
{
    if (!selected) return nullptr;
    // Preorder traversal chooses the selected object's first mesh, then the
    // highest mesh in its child hierarchy, in the same order as Hierarchy.
    std::vector<Engine::Core::Object*> pending{ selected };
    while (!pending.empty())
    {
        Engine::Core::Object* object = pending.back();
        pending.pop_back();
        for (Engine::Core::Component* component : object->Components)
            if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(component);
                mesh && !mesh->GetVertices().empty())
                return mesh;
        for (auto it = object->Children.rbegin(); it != object->Children.rend();
            ++it)
            pending.push_back(*it);
    }
    return nullptr;
}

void EditorState::SyncMeshEditSelection(Engine::Scene::Scene* scene,
    MeshEditSession& session)
{
    if (!scene || !session.enabled) return;
    scene->SetEditorMeshEditPose(true);
    Engine::Components::Mesh* mesh = ResolveMeshForSelection(
        scene->GetSelectedObject());
    if (session.activeMesh != mesh)
    {
        if (session.dirty && !session.savePath.empty())
        {
            MeshEditSession cached = session;
            cached.activeMesh = nullptr;
            m_meshEditCache[AssetPathIdentity(session.savePath)] =
                std::move(cached);
        }
        session.activeMesh = mesh;
        session.edgeCache.reset();
        session.gizmoDragging = false;
        session.gizmoChanged = false;
        session.gizmoLastPixels = 0.f;
        session.gizmoStartPositions.clear();
        session.gizmoBeforeSnapshot.clear();
        session.selectedElement = 0;
        session.selectedElements = mesh ? std::vector<uint32_t>{ 0 }
            : std::vector<uint32_t>{};
        session.toolError.clear();
        session.undo.clear();
        session.redo.clear();
        session.baseline = mesh ? CaptureMeshSnapshot(*mesh) : std::string{};
        session.savedSnapshot = session.baseline;
        session.dirty = false;
        session.savePath = mesh ? mesh->GetFilePath() : std::string{};
        if (mesh && !session.savePath.empty())
        {
            std::filesystem::path native(session.savePath);
            native.replace_extension(".mesh");
            session.savePath = native.string();
        }
        if (mesh && session.savePath.empty())
        {
            const std::string source = scene == m_prefabScene.get()
                ? m_activePrefabPath
                : (scene == m_scene.get() ? m_currentScenePath
                    : (m_activeSceneAssetDocument
                        ? m_activeSceneAssetDocument->path : std::string{}));
            if (!source.empty())
                session.savePath = (std::filesystem::path(source).parent_path() /
                    ((mesh->Owner ? mesh->Owner->name : std::string("Mesh")) +
                        ".mesh")).string();
        }
        if (mesh && !session.savePath.empty())
            if (auto found = m_meshEditCache.find(
                    AssetPathIdentity(session.savePath));
                found != m_meshEditCache.end())
            {
                const bool enabled = session.enabled;
                session = found->second;
                session.enabled = enabled;
                session.activeMesh = mesh;
                RestoreMeshSnapshot(*mesh, session.baseline);
            }
        if (m_renderer) m_renderer->MarkDirty();
    }
    scene->SetEditorSelectedMesh(mesh);
}

void EditorState::SetMeshEditMode(Engine::Scene::Scene* scene,
    MeshEditSession& session, bool enabled)
{
    session.enabled = enabled;
    if (scene)
    {
        scene->SetEditorSelectedMesh(nullptr);
        scene->SetEditorMeshEditPose(enabled);
    }
    for (const auto& panel : m_panels)
        if (auto* view = dynamic_cast<SceneView*>(panel.get());
            view && view->GetScene() == scene)
            view->AllowObjectTransform = !enabled;
    if (enabled) SyncMeshEditSelection(scene, session);
    m_pendingEditToolsOpen = true;
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::DrawEditTools(IEditorUi& ui)
{
    if (m_activeAssetDocument)
    {
        ui.DisabledLabel("Select a Scene or Prefab document for edit tools.");
        return;
    }
    if (m_activeSceneAssetDocument)
    {
        SceneAssetDocument& document = *m_activeSceneAssetDocument;
        if (document.meshStage && document.meshStageTools)
        {
            ui.ColoredLabel("Mesh Mode", { .35f, .75f, 1.f, 1.f });
            if (document.meshEdit.enabled)
            {
                DrawMeshEditTools(ui);
                ui.Separator();
                ui.ColoredLabel("Vertex attributes and UV", { .35f, .75f, 1.f, 1.f });
            }
            document.meshStageTools(ui);
            return;
        }
        if (document.skeletonStage && document.skeletonStageTools)
        {
            ui.ColoredLabel("Skeleton Mode", { .35f, .75f, 1.f, 1.f });
            document.skeletonStageTools(ui);
            return;
        }
        if (document.objectStage && document.objectStageTools)
        {
            ui.ColoredLabel("Object Mode", { .35f, .75f, 1.f, 1.f });
            document.objectStageTools(ui);
            ui.Separator();
            DrawObjectEditTools(ui);
            return;
        }
    }
    if (MeshEditSession* edit = ActiveMeshEditSession();
        edit && edit->enabled)
    {
        ui.ColoredLabel("Mesh Mode", { .35f, .75f, 1.f, 1.f });
        DrawMeshEditTools(ui);
    }
    else
    {
        ui.ColoredLabel("Object Mode", { .35f, .75f, 1.f, 1.f });
        DrawObjectEditTools(ui);
    }
}

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

const std::vector<std::pair<uint32_t, uint32_t>>&
EditorState::CachedMeshEdges(MeshEditSession& session,
    const Engine::Components::Mesh& mesh)
{
    const auto& indices = mesh.GetIndices();
    const size_t vertexCount = mesh.GetVertices().size();
    if (session.edgeCache && session.edgeCache->mesh == &mesh &&
        session.edgeCache->vertexCount == vertexCount &&
        session.edgeCache->indices == indices)
        return session.edgeCache->edges;
    auto cache = std::make_shared<MeshEditSession::EdgeCache>();
    cache->mesh = &mesh;
    cache->vertexCount = vertexCount;
    cache->indices = indices;
    if (indices.empty())
    {
        std::vector<uint32_t> implicitIndices(vertexCount);
        std::iota(implicitIndices.begin(), implicitIndices.end(), 0u);
        cache->edges = MeshEditGeometry::EdgeListFromIndices(implicitIndices);
    }
    else cache->edges = MeshEditGeometry::EdgeListFromIndices(indices);
    session.edgeCache = std::move(cache);
    return session.edgeCache->edges;
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
        if (count > 1 && ui.SliderInt("Element", &selected, 0,
            static_cast<int>(count - 1)))
        {
            session->selectedElement = static_cast<uint32_t>(selected);
            session->selectedElements = { session->selectedElement };
        }
    }
    if (!session->toolError.empty()) ui.DisabledLabel(session->toolError.c_str());
    ui.Separator();
    const auto matches = [&](const char* name)
    {
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
        ui.DisabledLabel("Topology tools require a mesh without morph targets.");
    else if (session->selectionMode == 1 && count && matches("Split edge"))
    {
        if (ui.Button("Split edge"))
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
            if (matches("Loop cut") && ui.Button("Loop cut"))
                run("Loop cut requires a planar quad strip edge.",
                    draft().LoopCut(session->selectedElement));
            if (matches("Fill boundary") && ui.Button("Fill boundary"))
                run("Fill requires a selected convex planar boundary loop.",
                    draft().FillBoundary(selected()));
            if (matches("Bridge boundaries") && ui.Button("Bridge boundaries"))
                run("Bridge requires two selected boundary loops of equal size.",
                    draft().BridgeBoundaries(selected()));
        }
        if (session->selectionMode == 0 && matches("Weld vertices") &&
            ui.Button("Weld vertices"))
            run("Weld requires at least two selected vertices and a remaining face.",
                draft().WeldVertices(selected()));
        if (session->selectionMode == 2 && matches("Duplicate faces") &&
            ui.Button("Duplicate faces"))
            run("Select faces to duplicate.", draft().DuplicateFaces(selected(),
                { session->move[0], session->move[1], session->move[2] }));
        if (matches("Delete selection") && ui.Button("Delete selection"))
            run("Delete must leave at least one face in the mesh.",
                draft().DeleteSelection(session->selectionMode, selected()));
    }
    ui.Separator();
    if (ui.Button(session->dirty ? "Save Mesh *" : "Save Mesh"))
        SaveMeshEditSession(*session);
}

bool EditorState::HandleMeshViewport(IEditorUi& ui,
    const EditorUiViewportInput& input, Engine::Scene::Scene* scene,
    MeshEditSession& session, EditorTransformTool tool)
{
    if (!scene || !session.enabled || input.available.x <= 1.f ||
        input.available.y <= 1.f) return false;
    SyncMeshEditSelection(scene, session);
    auto* mesh = session.activeMesh;
    auto* camera = scene->editorCamera.GetComponent<Engine::Components::Camera>();
    if (!mesh || !mesh->Owner || !camera) return false;
    const auto& vertices = mesh->GetVertices();
    const auto& storedIndices = mesh->GetIndices();
    std::vector<uint32_t> implicitIndices;
    if (storedIndices.empty())
    {
        implicitIndices.resize(vertices.size());
        std::iota(implicitIndices.begin(), implicitIndices.end(), 0u);
    }
    const auto& indices = storedIndices.empty() ? implicitIndices : storedIndices;
    const uint32_t faceCount = static_cast<uint32_t>(indices.size() / 3);
    const std::vector<std::pair<uint32_t, uint32_t>> emptyEdges;
    const auto& edges = session.selectionMode == 1
        ? CachedMeshEdges(session, *mesh) : emptyEdges;
    const glm::mat4 transform = camera->GetProjectionMatrix(
        input.available.x / input.available.y) * camera->GetViewMatrix() *
        mesh->Owner->transform.GetWorldMatrix();
    struct Point { EditorUiVec2 screen{}; float depth = 0.f; bool valid = false; };
    std::vector<Point> points(vertices.size());
    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto& p = vertices[i].pos;
        glm::vec4 clip = transform * glm::vec4(p[0], p[1], p[2], 1.f);
        if (clip.w <= .0001f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.z < 0.f || ndc.z > 1.f) continue;
        points[i] = { { (ndc.x * .5f + .5f) * input.available.x,
            (.5f - ndc.y * .5f) * input.available.y }, ndc.z, true };
    }
    struct Candidate { EditorUiVec2 screen{}; float depth = 0.f; bool valid = false; };
    std::vector<Candidate> candidates;
    candidates.reserve(session.selectionMode == 0 ? points.size()
        : (session.selectionMode == 1 ? edges.size() : faceCount));
    if (session.selectionMode == 0)
    {
        for (const auto& p : points)
            candidates.push_back({ p.screen, p.depth, p.valid });
    }
    else if (session.selectionMode == 1)
    {
        for (uint32_t i = 0; i < edges.size(); ++i)
        {
            auto [a, b] = edges[i];
            bool valid = points[a].valid && points[b].valid;
            candidates.push_back({ { (points[a].screen.x + points[b].screen.x) * .5f,
                (points[a].screen.y + points[b].screen.y) * .5f },
                (points[a].depth + points[b].depth) * .5f, valid });
        }
    }
    else
    {
        for (uint32_t i = 0; i < faceCount; ++i)
        {
            const auto& a = points[indices[i * 3]];
            const auto& b = points[indices[i * 3 + 1]];
            const auto& c = points[indices[i * 3 + 2]];
            candidates.push_back({ { (a.screen.x + b.screen.x + c.screen.x) / 3.f,
                (a.screen.y + b.screen.y + c.screen.y) / 3.f },
                (a.depth + b.depth + c.depth) / 3.f,
                a.valid && b.valid && c.valid });
        }
    }
    const EditorUiColor plain{ .7f, .8f, .95f, .65f };
    const EditorUiColor active{ 1.f, .7f, .15f, 1.f };
    const std::unordered_set<uint32_t> selectedSet(
        session.selectedElements.begin(), session.selectedElements.end());
    const uint32_t vertexCellsX = session.selectionMode == 0
        ? static_cast<uint32_t>(std::ceil(input.available.x / 4.f)) : 0u;
    const uint32_t vertexCellsY = session.selectionMode == 0
        ? static_cast<uint32_t>(std::ceil(input.available.y / 4.f)) : 0u;
    std::vector<uint8_t> drawnVertexCells(
        static_cast<size_t>(vertexCellsX) * vertexCellsY);
    const uint32_t edgeCellsX = session.selectionMode == 1
        ? static_cast<uint32_t>(std::ceil(input.available.x / 8.f)) : 0u;
    const uint32_t edgeCellsY = session.selectionMode == 1
        ? static_cast<uint32_t>(std::ceil(input.available.y / 8.f)) : 0u;
    std::vector<uint8_t> drawnShortEdgeCells(
        static_cast<size_t>(edgeCellsX) * edgeCellsY * 4u);
    for (uint32_t i = 0; i < candidates.size(); ++i)
    {
        if (!candidates[i].valid) continue;
        const bool selected = selectedSet.count(i) != 0;
        if (session.selectionMode == 0)
        {
            if (!selected)
            {
                const auto p = candidates[i].screen;
                if (p.x < 0.f || p.y < 0.f ||
                    p.x > input.available.x || p.y > input.available.y)
                    continue;
                const uint32_t x = std::min(vertexCellsX - 1,
                    static_cast<uint32_t>(p.x / 4.f));
                const uint32_t y = std::min(vertexCellsY - 1,
                    static_cast<uint32_t>(p.y / 4.f));
                uint8_t& occupied = drawnVertexCells[
                    static_cast<size_t>(y) * vertexCellsX + x];
                if (occupied) continue;
                occupied = 1;
            }
            ui.DrawViewportCircle(candidates[i].screen, selected ? 6.f : 3.f,
                selected ? active : plain, true);
        }
        else if (session.selectionMode == 1)
        {
            auto [a, b] = edges[i];
            const auto first = points[a].screen, second = points[b].screen;
            if (!selected)
            {
                const float dx = second.x - first.x,
                    dy = second.y - first.y;
                const float length2 = dx * dx + dy * dy;
                if (length2 < 6.25f ||
                    (first.x < 0.f && second.x < 0.f) ||
                    (first.y < 0.f && second.y < 0.f) ||
                    (first.x > input.available.x &&
                        second.x > input.available.x) ||
                    (first.y > input.available.y &&
                        second.y > input.available.y))
                    continue;
                if (length2 < 144.f)
                {
                    const float midX = (first.x + second.x) * .5f,
                        midY = (first.y + second.y) * .5f;
                    if (midX >= 0.f && midY >= 0.f &&
                        midX < input.available.x &&
                        midY < input.available.y)
                    {
                        const uint32_t x = std::min(edgeCellsX - 1,
                            static_cast<uint32_t>(midX / 8.f));
                        const uint32_t y = std::min(edgeCellsY - 1,
                            static_cast<uint32_t>(midY / 8.f));
                        const uint32_t direction =
                            std::abs(dx) > 2.f * std::abs(dy) ? 0u :
                            (std::abs(dy) > 2.f * std::abs(dx) ? 1u :
                                (dx * dy >= 0.f ? 2u : 3u));
                        uint8_t& occupied = drawnShortEdgeCells[
                            (static_cast<size_t>(y) * edgeCellsX + x) * 4u +
                            direction];
                        if (occupied) continue;
                        occupied = 1;
                    }
                }
            }
            ui.DrawViewportLine(points[a].screen, points[b].screen,
                selected ? active : plain, selected ? 3.f : 1.f);
        }
        else if (selected)
        {
            auto a = points[indices[i * 3]].screen;
            auto b = points[indices[i * 3 + 1]].screen;
            auto c = points[indices[i * 3 + 2]].screen;
            ui.DrawViewportTriangle(a, b, c, { 1.f, .7f, .15f, .2f });
            ui.DrawViewportLine(a, b, active, 2.f);
            ui.DrawViewportLine(b, c, active, 2.f);
            ui.DrawViewportLine(c, a, active, 2.f);
        }
    }
    std::vector<uint32_t> selectedVertices;
    selectedVertices.reserve(session.selectedElements.size() *
        (session.selectionMode == 2 ? 3u : 2u));
    std::unordered_set<uint32_t> selectedVertexSet;
    const auto addSelectedVertex = [&](uint32_t id)
    {
        if (selectedVertexSet.insert(id).second)
            selectedVertices.push_back(id);
    };
    for (uint32_t id : session.selectedElements)
    {
        if (session.selectionMode == 0 && id < vertices.size())
            addSelectedVertex(id);
        else if (session.selectionMode == 1 && id < edges.size())
        {
            addSelectedVertex(edges[id].first);
            addSelectedVertex(edges[id].second);
        }
        else if (session.selectionMode == 2 && id < faceCount)
            for (int corner = 0; corner < 3; ++corner)
                addSelectedVertex(indices[id * 3 + corner]);
    }
    if (!selectedVertices.empty() &&
        (tool != EditorTransformTool::Hand || session.gizmoDragging))
    {
        const EditorTransformTool activeTool = session.gizmoDragging
            ? session.gizmoTool : tool;
        glm::vec3 pivot{};
        if (session.pivotMode == 3)
            pivot = { session.customPivot[0], session.customPivot[1],
                session.customPivot[2] };
        else if (session.pivotMode == 0)
        {
            for (uint32_t id : selectedVertices)
            {
                const auto& p = vertices[id].pos;
                pivot += glm::vec3(p[0], p[1], p[2]);
            }
            pivot /= static_cast<float>(selectedVertices.size());
        }
        else if (session.pivotMode == 1)
        {
            glm::vec3 low(INFINITY), high(-INFINITY);
            for (uint32_t id : selectedVertices)
            {
                const auto& p = vertices[id].pos;
                glm::vec3 value(p[0], p[1], p[2]);
                low = glm::min(low, value); high = glm::max(high, value);
            }
            pivot = (low + high) * .5f;
        }
        const glm::mat4 world = mesh->Owner->transform.GetWorldMatrix();
        const glm::mat4 viewProjection = camera->GetProjectionMatrix(
            input.available.x / input.available.y) * camera->GetViewMatrix();
        const glm::vec3 worldPivot = glm::vec3(world * glm::vec4(pivot, 1.f));
        const glm::vec3 cameraWorld = glm::vec3(
            scene->editorCamera.transform.GetWorldMatrix()[3]);
        const float axisScale = std::clamp(glm::length(
            worldPivot - cameraWorld) * .18f, .35f, 8.f);
        const auto projectWorld = [&](glm::vec3 position, EditorUiVec2& screen)
        {
            glm::vec4 clip = viewProjection * glm::vec4(position, 1.f);
            if (clip.w <= .0001f) return false;
            glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.z < 0.f || ndc.z > 1.f) return false;
            screen = { (ndc.x * .5f + .5f) * input.available.x,
                (.5f - ndc.y * .5f) * input.available.y };
            return true;
        };
        EditorUiVec2 origin{};
        if (projectWorld(worldPivot, origin))
        {
            const EditorUiColor colors[3]{
                { .95f, .22f, .18f, 1.f }, { .25f, .85f, .3f, 1.f },
                { .22f, .48f, 1.f, 1.f } };
            const EditorUiColor hover{ 1.f, .82f, .16f, 1.f };
            glm::vec3 axes[3]{};
            for (int axis = 0; axis < 3; ++axis)
            {
                axes[axis] = glm::vec3(world[axis]);
                if (glm::length(axes[axis]) > 1e-6f)
                    axes[axis] = glm::normalize(axes[axis]);
            }
            EditorUiVec2 ends[3]{};
            bool visible[3]{};
            int hoveredAxis = -1;
            float closest = 9.f;
            EditorUiVec2 hoverDirection{};
            float hoverPixels = 1.f;
            const auto segmentDistance = [&](EditorUiVec2 a, EditorUiVec2 b,
                EditorUiVec2 p)
            {
                float dx = b.x - a.x, dy = b.y - a.y;
                float length2 = dx * dx + dy * dy;
                float t = length2 > 1e-5f ? std::clamp(
                    ((p.x - a.x) * dx + (p.y - a.y) * dy) /
                        length2, 0.f, 1.f) : 0.f;
                return std::hypot(p.x - a.x - t * dx,
                    p.y - a.y - t * dy);
            };
            for (int axis = 0; axis < 3; ++axis)
            {
                if (glm::length(axes[axis]) < .5f) continue;
                if (activeTool != EditorTransformTool::Rotate)
                {
                    visible[axis] = projectWorld(worldPivot + axes[axis] *
                        axisScale, ends[axis]);
                    if (!visible[axis]) continue;
                    float distance = segmentDistance(origin, ends[axis],
                        input.mousePosInViewport);
                    if (distance < closest &&
                        std::hypot(input.mousePosInViewport.x - origin.x,
                            input.mousePosInViewport.y - origin.y) > 10.f)
                    {
                        closest = distance; hoveredAxis = axis;
                        float dx = ends[axis].x - origin.x,
                            dy = ends[axis].y - origin.y;
                        hoverPixels = std::hypot(dx, dy);
                        if (hoverPixels > 1.f)
                            hoverDirection = { dx / hoverPixels, dy / hoverPixels };
                    }
                }
                else
                {
                    EditorUiVec2 previous{}; bool previousVisible = false;
                    for (int segment = 0; segment <= 48; ++segment)
                    {
                        float angle = 6.2831853f * segment / 48.f;
                        EditorUiVec2 current{};
                        bool currentVisible = projectWorld(worldPivot +
                            (axes[(axis + 1) % 3] * std::cos(angle) +
                                axes[(axis + 2) % 3] * std::sin(angle)) *
                            axisScale * .78f, current);
                        if (currentVisible && previousVisible)
                        {
                            float distance = segmentDistance(previous, current,
                                input.mousePosInViewport);
                            if (distance < closest)
                            {
                                closest = distance; hoveredAxis = axis;
                                float dx = current.x - previous.x,
                                    dy = current.y - previous.y;
                                hoverPixels = std::hypot(dx, dy);
                                if (hoverPixels > 1.f)
                                    hoverDirection = { dx / hoverPixels,
                                        dy / hoverPixels };
                            }
                        }
                        previous = current; previousVisible = currentVisible;
                    }
                }
            }
            ui.DrawViewportCircle(origin, 4.f, hover, true);
            for (int axis = 0; axis < 3; ++axis)
            {
                const EditorUiColor color = axis == hoveredAxis ||
                    (session.gizmoDragging && axis == session.gizmoAxis)
                        ? hover : colors[axis];
                if (activeTool != EditorTransformTool::Rotate)
                {
                    if (!visible[axis]) continue;
                    ui.DrawViewportLine(origin, ends[axis],
                        { 0.f, 0.f, 0.f, .9f }, 6.f);
                    ui.DrawViewportLine(origin, ends[axis], color, 3.f);
                    if (activeTool == EditorTransformTool::Scale)
                        ui.DrawViewportCircle(ends[axis], 5.f, color, true);
                    else
                    {
                        float dx = ends[axis].x - origin.x,
                            dy = ends[axis].y - origin.y;
                        float len = std::hypot(dx, dy);
                        if (len > 8.f)
                        {
                            dx /= len; dy /= len;
                            ui.DrawViewportTriangle(ends[axis],
                                { ends[axis].x - dx * 12.f - dy * 5.f,
                                    ends[axis].y - dy * 12.f + dx * 5.f },
                                { ends[axis].x - dx * 12.f + dy * 5.f,
                                    ends[axis].y - dy * 12.f - dx * 5.f },
                                color);
                        }
                    }
                }
                else
                {
                    EditorUiVec2 previous{}; bool previousVisible = false;
                    for (int segment = 0; segment <= 48; ++segment)
                    {
                        float angle = 6.2831853f * segment / 48.f;
                        EditorUiVec2 current{};
                        bool currentVisible = projectWorld(worldPivot +
                            (axes[(axis + 1) % 3] * std::cos(angle) +
                                axes[(axis + 2) % 3] * std::sin(angle)) *
                            axisScale * .78f, current);
                        if (currentVisible && previousVisible)
                            ui.DrawViewportLine(previous, current, color,
                                axis == hoveredAxis ? 3.f : 2.f);
                        previous = current; previousVisible = currentVisible;
                    }
                }
            }
            if (session.gizmoDragging)
            {
                if (input.leftDown)
                {
                    const float pixels =
                        (input.mousePosInViewport.x - session.gizmoStartMouse.x) *
                            session.gizmoScreenDirection.x +
                        (input.mousePosInViewport.y - session.gizmoStartMouse.y) *
                            session.gizmoScreenDirection.y;
                    if (std::abs(pixels - session.gizmoLastPixels) < .001f)
                        return true;
                    session.gizmoLastPixels = pixels;
                    MeshEditGeometry preview{ mesh->GetVertices(), {} };
                    if (session.gizmoStartPositions.size() == selectedVertices.size())
                    {
                        for (const auto& [id, position] : session.gizmoStartPositions)
                            if (id < preview.vertices.size())
                                std::copy(position.begin(), position.end(),
                                    preview.vertices[id].pos);
                        std::array<float, 3> move{}, rotation{}, sizing{ 1.f, 1.f, 1.f };
                        if (session.gizmoTool == EditorTransformTool::Translate)
                            move[session.gizmoAxis] = pixels *
                                session.gizmoUnitsPerPixel;
                        else if (session.gizmoTool == EditorTransformTool::Rotate)
                            rotation[session.gizmoAxis] = pixels * .5f;
                        else
                            sizing[session.gizmoAxis] = std::max(.01f,
                                1.f + pixels * .01f);
                        if (preview.TransformSelection(0,
                            selectedVertices, move, rotation, sizing,
                            session.pivotMode,
                            { session.customPivot[0], session.customPivot[1],
                                session.customPivot[2] }, session.snapStep,
                            session.rotationSnap, session.scaleSnap) &&
                            mesh->UpdateAuthoredVertices(
                                std::move(preview.vertices)))
                        {
                            session.gizmoChanged = std::abs(pixels) > .001f;
                            if (m_renderer) m_renderer->MarkDirty();
                        }
                    }
                }
                else
                {
                    session.gizmoDragging = false;
                    session.gizmoStartPositions.clear();
                    const std::string after = CaptureMeshSnapshot(*mesh);
                    if (session.gizmoChanged &&
                        after != session.gizmoBeforeSnapshot)
                    {
                        if (m_historyLimit > 0 &&
                            session.undo.size() >= m_historyLimit)
                            session.undo.pop_front();
                        if (m_historyLimit > 0)
                            session.undo.push_back(session.gizmoBeforeSnapshot);
                        session.gizmoBeforeSnapshot.clear();
                        session.gizmoChanged = false;
                        session.redo.clear();
                        session.baseline = after;
                        session.dirty = session.baseline != session.savedSnapshot;
                        if (!session.savePath.empty())
                        {
                            MeshEditSession cached = session;
                            cached.activeMesh = nullptr;
                            m_meshEditCache[AssetPathIdentity(session.savePath)] =
                                std::move(cached);
                        }
                    }
                    session.gizmoBeforeSnapshot.clear();
                    session.gizmoChanged = false;
                }
                return true;
            }
            if (input.leftClicked && input.hovered && hoveredAxis >= 0 &&
                !session.selecting &&
                (hoverDirection.x != 0.f || hoverDirection.y != 0.f))
            {
                session.gizmoDragging = true;
                session.gizmoChanged = false;
                session.gizmoLastPixels = 0.f;
                session.gizmoAxis = hoveredAxis;
                session.gizmoTool = activeTool;
                session.gizmoStartMouse = input.mousePosInViewport;
                session.gizmoScreenDirection = hoverDirection;
                const float localAxisScale = glm::length(glm::vec3(world[hoveredAxis]));
                session.gizmoUnitsPerPixel = axisScale /
                    std::max(hoverPixels * localAxisScale, 1.f);
                session.gizmoBeforeSnapshot = CaptureMeshSnapshot(*mesh);
                session.gizmoStartPositions.clear();
                session.gizmoStartPositions.reserve(selectedVertices.size());
                for (uint32_t id : selectedVertices)
                {
                    const auto& vertex = vertices[id];
                    session.gizmoStartPositions.push_back({ id,
                        { vertex.pos[0], vertex.pos[1], vertex.pos[2] } });
                }
                return true;
            }
        }
    }
    if (tool == EditorTransformTool::Hand) return false;
    if (input.leftClicked && input.hovered && !input.rightDown &&
        !input.middleDown)
    {
        session.selecting = true;
        session.selectionStart = input.mousePosInViewport;
        session.lasso = { input.mousePosInViewport };
    }
    if (session.selecting && input.leftDown && session.selectionTool == 2)
    {
        const auto& last = session.lasso.back();
        const float dx = input.mousePosInViewport.x - last.x,
            dy = input.mousePosInViewport.y - last.y;
        if (dx * dx + dy * dy >= 9.f)
        {
            if (session.lasso.size() >= 512)
            {
                std::vector<EditorUiVec2> reduced;
                reduced.reserve(257);
                for (size_t i = 0; i < session.lasso.size(); i += 2)
                    reduced.push_back(session.lasso[i]);
                session.lasso = std::move(reduced);
            }
            session.lasso.push_back(input.mousePosInViewport);
        }
    }
    if (session.selecting && session.selectionTool == 1)
    {
        auto a = session.selectionStart, b = input.mousePosInViewport;
        EditorUiVec2 c{ b.x, a.y }, d{ a.x, b.y };
        ui.DrawViewportLine(a, c, active, 1.f);
        ui.DrawViewportLine(c, b, active, 1.f);
        ui.DrawViewportLine(b, d, active, 1.f);
        ui.DrawViewportLine(d, a, active, 1.f);
    }
    if (session.selecting && session.selectionTool == 2)
        for (size_t i = 1; i < session.lasso.size(); ++i)
            ui.DrawViewportLine(session.lasso[i - 1], session.lasso[i], active, 1.f);
    if (!session.selecting || (input.leftDown && !input.leftReleased))
        return session.selecting;
    session.selecting = false;
    std::vector<uint32_t> hits;
    const auto cursor = input.mousePosInViewport;
    if (session.selectionTool == 0)
    {
        float closest = 12.f * 12.f, depth = 1.f;
        for (uint32_t i = 0; i < candidates.size(); ++i)
        {
            const auto& c = candidates[i];
            if (!c.valid) continue;
            float distance = 0.f;
            if (session.selectionMode == 1)
            {
                auto [first, second] = edges[i];
                const auto a = points[first].screen, b = points[second].screen;
                const float vx = b.x - a.x, vy = b.y - a.y;
                const float length2 = vx * vx + vy * vy;
                const float t = length2 > 1e-5f ? std::clamp(
                    ((cursor.x - a.x) * vx + (cursor.y - a.y) * vy) /
                        length2, 0.f, 1.f) : 0.f;
                const float dx = cursor.x - (a.x + t * vx);
                const float dy = cursor.y - (a.y + t * vy);
                distance = dx * dx + dy * dy;
            }
            else if (session.selectionMode == 2)
            {
                const auto a = points[indices[i * 3]].screen;
                const auto b = points[indices[i * 3 + 1]].screen;
                const auto d = points[indices[i * 3 + 2]].screen;
                const auto cross = [](EditorUiVec2 p, EditorUiVec2 q,
                    EditorUiVec2 r)
                { return (q.x - p.x) * (r.y - p.y) -
                    (q.y - p.y) * (r.x - p.x); };
                const float ab = cross(a, b, cursor),
                    bc = cross(b, d, cursor), ca = cross(d, a, cursor);
                if ((ab < 0.f || bc < 0.f || ca < 0.f) &&
                    (ab > 0.f || bc > 0.f || ca > 0.f)) continue;
                distance = 0.f;
            }
            else
            {
                const float dx = cursor.x - c.screen.x,
                    dy = cursor.y - c.screen.y;
                distance = dx * dx + dy * dy;
            }
            if (distance < closest || (distance == closest && c.depth < depth))
            { closest = distance; depth = c.depth; hits = { i }; }
        }
    }
    else
    {
        float lowX = std::min(session.selectionStart.x, cursor.x),
            highX = std::max(session.selectionStart.x, cursor.x),
            lowY = std::min(session.selectionStart.y, cursor.y),
            highY = std::max(session.selectionStart.y, cursor.y);
        if (session.selectionTool == 2 && !session.lasso.empty())
        {
            lowX = lowY = INFINITY; highX = highY = -INFINITY;
            for (const auto& point : session.lasso)
            {
                lowX = std::min(lowX, point.x);
                highX = std::max(highX, point.x);
                lowY = std::min(lowY, point.y);
                highY = std::max(highY, point.y);
            }
        }
        for (uint32_t i = 0; i < candidates.size(); ++i)
        {
            const auto& c = candidates[i];
            if (!c.valid || c.screen.x < lowX || c.screen.x > highX ||
                c.screen.y < lowY || c.screen.y > highY) continue;
            bool inside = session.selectionTool == 1;
            if (session.selectionTool == 2 && session.lasso.size() >= 3)
                for (size_t j = 0, k = session.lasso.size() - 1;
                    j < session.lasso.size(); k = j++)
                {
                    const auto& a = session.lasso[j];
                    const auto& b = session.lasso[k];
                    if ((a.y > c.screen.y) != (b.y > c.screen.y) &&
                        c.screen.x < (b.x - a.x) * (c.screen.y - a.y) /
                            (b.y - a.y) + a.x) inside = !inside;
                }
            if (inside) hits.push_back(i);
        }
    }
    const bool multiSelect = ui.IsMultiSelectModifierDown();
    if (!multiSelect) session.selectedElements.clear();
    std::unordered_set<uint32_t> selectionMembership(
        session.selectedElements.begin(), session.selectedElements.end());
    std::unordered_set<uint32_t> removed;
    for (uint32_t id : hits)
    {
        if (selectionMembership.insert(id).second)
            session.selectedElements.push_back(id);
        else if (multiSelect) removed.insert(id);
    }
    if (!removed.empty())
        session.selectedElements.erase(std::remove_if(
            session.selectedElements.begin(), session.selectedElements.end(),
            [&](uint32_t id) { return removed.count(id) != 0; }),
            session.selectedElements.end());
    if (!session.selectedElements.empty())
        session.selectedElement = session.selectedElements.back();
    return true;
}

bool EditorState::ApplyMeshHistory(bool redo)
{
    MeshEditSession* session = ActiveMeshEditSession();
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    if (!session || !session->enabled || !scene) return false;
    SyncMeshEditSelection(scene, *session);
    auto& source = redo ? session->redo : session->undo;
    auto& destination = redo ? session->undo : session->redo;
    if (!session->activeMesh || source.empty()) return false;
    const std::string target = source.back();
    if (!RestoreMeshSnapshot(*session->activeMesh, target)) return false;
    source.pop_back();
    destination.push_back(session->baseline);
    session->baseline = target;
    session->dirty = target != session->savedSnapshot;
    if (!session->savePath.empty())
    {
        MeshEditSession cached = *session;
        cached.activeMesh = nullptr;
        m_meshEditCache[AssetPathIdentity(session->savePath)] = std::move(cached);
    }
    if (m_renderer) m_renderer->MarkDirty();
    return true;
}

bool EditorState::SaveMeshEditSession(MeshEditSession& session)
{
    if (session.activeMesh && session.savePath.empty() &&
        &session == &m_mainMeshEdit && m_currentScenePath.empty())
        SaveMainScene();
    if (session.activeMesh && session.savePath.empty())
    {
        const std::string source = &session == &m_mainMeshEdit
            ? m_currentScenePath : (&session == &m_prefabMeshEdit
                ? m_activePrefabPath : std::string{});
        if (!source.empty())
            session.savePath = (std::filesystem::path(source).parent_path() /
                ((session.activeMesh->Owner
                    ? session.activeMesh->Owner->name : std::string("Mesh")) +
                    ".mesh")).string();
    }
    if (!session.activeMesh || session.savePath.empty())
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Save the scene or prefab before saving a generated mesh.");
        return false;
    }
    Engine::Components::Mesh* mesh = session.activeMesh;
    std::error_code directoryError;
    const std::filesystem::path parent =
        std::filesystem::path(session.savePath).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, directoryError);
    if (directoryError)
        return false;
    if (!Engine::Components::Mesh::SaveNativeFile(session.savePath,
            mesh->GetVertices(), mesh->GetIndices()))
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not save mesh asset: " + session.savePath);
        return false;
    }
    mesh->SetAuthoredFilePath(session.savePath);
    session.baseline = CaptureMeshSnapshot(*mesh);
    session.savedSnapshot = session.baseline;
    session.dirty = false;
    MeshEditSession cached = session;
    cached.activeMesh = nullptr;
    m_meshEditCache[AssetPathIdentity(session.savePath)] = std::move(cached);
    InvalidateMeshEditPointers();
    if (m_viewFactory && m_viewFactory->OnAssetContentsChanged)
        m_viewFactory->OnAssetContentsChanged(session.savePath);
    if (m_renderer) m_renderer->MarkDirty();
    return true;
}

void EditorState::InvalidateMeshEditPointers()
{
    m_mainMeshEdit.activeMesh = nullptr;
    m_prefabMeshEdit.activeMesh = nullptr;
    if (m_scene) m_scene->SetEditorSelectedMesh(nullptr);
    if (m_prefabScene) m_prefabScene->SetEditorSelectedMesh(nullptr);
    for (const auto& document : m_sceneAssetDocuments)
        if (document)
        {
            document->meshEdit.activeMesh = nullptr;
            if (document->scene)
                document->scene->SetEditorSelectedMesh(nullptr);
        }
}

bool EditorState::SavePendingMeshEdits()
{
    for (auto& [identity, edit] : m_meshEditCache)
    {
        if (!edit.dirty) continue;
        Engine::Components::Mesh geometry;
        std::error_code directoryError;
        const std::filesystem::path parent =
            std::filesystem::path(edit.savePath).parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, directoryError);
        if (edit.savePath.empty() ||
            directoryError ||
            !RestoreMeshSnapshot(geometry, edit.baseline) ||
            !Engine::Components::Mesh::SaveNativeFile(edit.savePath,
                geometry.GetVertices(), geometry.GetIndices()))
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Could not save pending mesh: " + edit.savePath);
            return false;
        }
        edit.savedSnapshot = edit.baseline;
        edit.dirty = false;
        const auto markSaved = [&](MeshEditSession& session)
        {
            if (AssetPathIdentity(session.savePath) != identity) return;
            session.savedSnapshot = session.baseline;
            session.dirty = false;
            session.activeMesh = nullptr;
        };
        markSaved(m_mainMeshEdit);
        markSaved(m_prefabMeshEdit);
        for (const auto& document : m_sceneAssetDocuments)
            if (document) markSaved(document->meshEdit);
        InvalidateMeshEditPointers();
        if (m_viewFactory && m_viewFactory->OnAssetContentsChanged)
            m_viewFactory->OnAssetContentsChanged(edit.savePath);
    }
    return true;
}

void EditorState::SaveScene()
{
    if (!m_playModeSceneSnapshot.empty())
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Scenes cannot be saved during Play mode. Stop the game first.");
        return;
    }

    if (MeshEditSession* meshEdit = ActiveMeshEditSession();
        meshEdit && meshEdit->enabled && meshEdit->dirty &&
        !SaveMeshEditSession(*meshEdit))
        return;
    if (!SavePendingMeshEdits()) return;

    if (m_activeSceneAssetDocument)
    {
        SaveSceneAssetDocument(*m_activeSceneAssetDocument);
        return;
    }

    if (!m_activePrefabPath.empty() && m_prefabDocumentFocused)
    {
        if (!m_prefabScene)
            return;
        Engine::Core::Object* root = nullptr;
        for (const auto& object : m_prefabScene->GetObjects())
            if (object && !object->Parent)
            {
                if (root)
                {
                    if (m_primaryConsole)
                        m_primaryConsole->AddLog(ConsoleView::Level::Error,
                            "Prefab stage must contain exactly one root object.");
                    return;
                }
                root = object.get();
            }
        if (!root || !Engine::Serialization::SceneSerializer::SavePrefab(*root, m_activePrefabPath))
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Failed to save prefab: " + m_activePrefabPath);
            return;
        }
        SetPrefabDirty(false);
        if (m_scene)
        {
            ::Engine::Scene::Scene::ObjectPath selectionPath;
            const bool hadSelection = m_scene->GetSelectedObject() &&
                m_scene->TryGetObjectPath(m_scene->GetSelectedObject(), selectionPath);
            if (!Engine::Serialization::SceneSerializer::RefreshPrefabInstances(*m_scene,
                m_activePrefabPath, m_scene->GetGraphicsProvider()) && m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Prefab saved, but scene instances could not be refreshed.");
            if (hadSelection)
                RefreshSelectionAfterReload(selectionPath);
        }
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Prefab saved: " + m_activePrefabPath);
        return;
    }

    if (m_activeAssetDocument)
    {
        if (!m_activeAssetDocument->Save() && m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Failed to save asset: " + m_activeAssetDocument->GetPath());
        return;
    }

    SaveMainScene();
}

bool EditorState::SaveMainScene()
{
    if (!m_scene || !m_playModeSceneSnapshot.empty())
        return false;

    std::string destination = m_currentScenePath;
    if (destination.empty() && !m_projectSettings.defaultScene.empty())
        destination = m_projectSettings.defaultScene;

    if (destination.empty())
    {
        wchar_t filePath[MAX_PATH] = {};
        std::filesystem::path initialDirectory = m_projectSettings.sceneDirectory.empty()
            ? std::filesystem::current_path()
            : std::filesystem::path(m_projectSettings.sceneDirectory);
        std::wstring initial = initialDirectory.wstring();
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window ? m_window->GetHWND() : nullptr;
        dialog.lpstrFile = filePath;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrFilter = L"Scene Files (*.scene;*.xml)\0*.scene;*.xml\0All Files\0*.*\0";
        dialog.lpstrDefExt = L"scene";
        dialog.lpstrInitialDir = initial.c_str();
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&dialog))
            return false;
        destination = std::filesystem::path(filePath).string();
    }

    if (std::filesystem::path(destination).extension().empty())
        destination += ".scene";

    if (m_scene->Save(destination))
    {
        m_currentScenePath = NormalizeAssetPath(destination);
        m_hasUnsavedChanges = false;
        RefreshSceneDocumentTitle();
        m_savedSceneSnapshot = m_scene->SaveToString();
        AssetPreviewCache::CaptureScene(m_currentScenePath, *m_scene,
            m_scene->GetGraphicsProvider());
        if (m_primaryConsole)
            m_primaryConsole->AddLog(
                ConsoleView::Level::Info, "Scene saved: " + m_currentScenePath);
        return true;
    }
    else if (m_primaryConsole)
    {
        m_primaryConsole->AddLog(
            ConsoleView::Level::Error, "Failed to save scene: " + destination);
    }

    return false;
}

void EditorState::SaveAll()
{
    if (!m_playModeSceneSnapshot.empty())
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Documents cannot be saved during Play mode. Stop the game first.");
        return;
    }

    if (!SavePendingMeshEdits()) return;

    for (AssetDocumentView* document : m_assetDocuments)
        if (document && document->IsDirty())
            document->Save();

    for (auto& document : m_sceneAssetDocuments)
        if (document && (document->dirty || document->meshEdit.dirty))
            SaveSceneAssetDocument(*document);

    if (!m_activePrefabPath.empty())
    {
        const bool previousFocus = m_prefabDocumentFocused;
        AssetDocumentView* previousAssetDocument = m_activeAssetDocument;
        m_activeAssetDocument = nullptr;
        m_prefabDocumentFocused = true;
        SaveScene();
        m_prefabDocumentFocused = previousFocus;
        m_activeAssetDocument = previousAssetDocument;
    }
    SaveMainScene();
    const bool projectSaved = !m_preferences || m_projectFilePath.empty()
        ? true : m_preferences->SaveSettings();
    if (m_primaryConsole)
        m_primaryConsole->AddLog(projectSaved
            ? ConsoleView::Level::Info : ConsoleView::Level::Error,
            projectSaved
                ? "Saved all open documents and project settings."
                : "Save All completed with errors.");
}

std::string EditorState::GetActiveDocumentName() const
{
    if (m_activeSceneAssetDocument)
    {
        std::string title = std::filesystem::path(
            m_activeSceneAssetDocument->path).filename().string();
        if (m_activeSceneAssetDocument->dirty) title += " *";
        return title;
    }
    if (m_activeAssetDocument)
    {
        std::string title = std::filesystem::path(
            m_activeAssetDocument->GetPath()).filename().string();
        if (m_activeAssetDocument->IsDirty())
            title += " *";
        return title;
    }
    std::string sceneName = m_currentScenePath.empty()
        ? "Untitled" : std::filesystem::path(m_currentScenePath).filename().string();
    if (m_hasUnsavedChanges)
        sceneName += " *";
    if (m_activePrefabPath.empty())
        return sceneName;

    std::string prefabName =
        std::filesystem::path(m_activePrefabPath).filename().string();
    if (m_prefabHasUnsavedChanges || m_prefabMeshEdit.dirty)
        prefabName += " *";

    return sceneName + " | " + prefabName;
}

void EditorState::BakeLighting()
{
    if (!m_scene)
        return;
    const std::string assets = m_projectSettings.assetsDirectory.empty()
        ? "Assets" : m_projectSettings.assetsDirectory;
    std::string sceneName = m_currentScenePath.empty()
        ? m_projectSettings.name
        : std::filesystem::path(m_currentScenePath).stem().string();
    if (sceneName.empty())
        sceneName = "Scene";
    const auto result = m_scene->BakeLighting(
        assets, sceneName, m_projectSettings.bakedLighting);
    if (m_primaryConsole)
        m_primaryConsole->AddLog(result.succeeded
            ? ConsoleView::Level::Info : ConsoleView::Level::Error,
            result.message);
    if (result.succeeded)
    {
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
    }
}

void EditorState::ClearBakedLighting()
{
    if (!m_scene)
        return;
    m_scene->ClearBakedLighting();
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            m_scene->GetLightingBakeStatus());
    m_hasUnsavedChanges = true;
    MarkSceneEdited();
}

// ---------------------------------------------------------------------------
// EditorState::LoadScene
// ---------------------------------------------------------------------------
void EditorState::LoadScene(const std::string& path)
{
    RequestSceneLoad(path);
}

void EditorState::RequestSceneLoad(const std::string& path)
{
    if (!m_scene)
    {
        OutputDebugStringA("[EditorState::LoadScene] ERROR: Scene is null\n");
        return;
    }

    if (m_hasUnsavedChanges)
    {
        m_sceneToLoad = path;
        m_showUnsavedWarning = true;
        if (m_renderer)
            m_renderer->MarkDirty();
        return;
    }

    LoadSceneNow(path);
}

bool EditorState::ConfirmSceneLoad(bool saveCurrentScene)
{
    if (saveCurrentScene && !SaveMainScene())
        return false;

    const std::string path = std::move(m_sceneToLoad);
    m_sceneToLoad.clear();
    m_showUnsavedWarning = false;
    if (m_renderer)
        m_renderer->MarkDirty();
    LoadSceneNow(path);
    return true;
}

void EditorState::CancelSceneLoad()
{
    m_sceneToLoad.clear();
    m_showUnsavedWarning = false;
    if (m_renderer)
        m_renderer->MarkDirty();
}

void EditorState::LoadSceneNow(const std::string& path)
{

    OutputDebugStringA(("[EditorState::LoadScene] Loading scene: " + path + "\n").c_str());
    
    // Check if file exists
    std::string resolvedPath = path;
    if (!std::filesystem::exists(resolvedPath))
    {
        OutputDebugStringA(("[EditorState::LoadScene] File not found at: " + resolvedPath + "\n").c_str());
        OutputDebugStringA(("[EditorState::LoadScene] Current directory: " + std::filesystem::current_path().string() + "\n").c_str());
        
        if (m_primaryConsole)
        {
            m_primaryConsole->AddLog(ConsoleView::Level::Error, "Scene file not found: " + resolvedPath);
            m_primaryConsole->AddLog(ConsoleView::Level::Info, "Current directory: " + std::filesystem::current_path().string());
        }
        return;
    }

    // Loading another editor document is a hard runtime boundary. Let the
    // host perform its normal Stop flow before this scene replaces the active
    // graph, so Playing/Paused state and the pre-play snapshot cannot leak
    // into the newly loaded scene.
    if (OnSceneLoadRequested)
        OnSceneLoadRequested(resolvedPath);
    
    // Load the scene
    try
    {
        if (m_scene->Load(resolvedPath))
        {
            m_mainMeshEdit = {};
            m_scene->SetEditorSelectedMesh(nullptr);
            OutputDebugStringA("[EditorState::LoadScene] Scene loaded successfully\n");
            LogStartupFailure("Scene loaded successfully: " + resolvedPath);
            if (m_preferences)
                m_preferences->SetSpatialDebugVisuals(
                    m_scene->settings.portalDebugVisuals);
            m_hasUnsavedChanges = false;
            m_currentScenePath = NormalizeAssetPath(resolvedPath);
            RefreshSceneDocumentTitle();
            ResetHistory(true);
            if (OnSceneLoadConfirmed)
                OnSceneLoadConfirmed();
            
            if (m_primaryConsole)
            {
                m_primaryConsole->AddLog(ConsoleView::Level::Info, "Scene loaded: " + resolvedPath);
            }
            return;
        }
        else
        {
            OutputDebugStringA("[EditorState::LoadScene] Failed to load scene\n");
            LogStartupFailure("Failed to load scene: " + resolvedPath);
            if (m_primaryConsole)
            {
                m_primaryConsole->AddLog(ConsoleView::Level::Error, "Failed to load scene: " + resolvedPath);
            }
            return;
        }
    }
    catch (const std::exception& e)
    {
        std::string errorMsg = "[EditorState::LoadScene] Exception during scene loading: ";
        errorMsg += e.what();
        errorMsg += "\n";
        OutputDebugStringA(errorMsg.c_str());
        
        if (m_primaryConsole)
        {
            m_primaryConsole->AddLog(ConsoleView::Level::Error, "Exception loading scene: " + std::string(e.what()));
        }
    }
}

void EditorState::OpenPrefabStage(const std::string& path)
{
    if (!m_scene || !m_viewFactory || path.empty()) return;
    const std::string normalized = NormalizeAssetPath(path);
    if (normalized == m_activePrefabPath) return;
    if (!m_activePrefabPath.empty() && m_prefabHasUnsavedChanges)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Save the active prefab before opening another prefab.");
        return;
    }

    if (!m_activePrefabPath.empty())
        ClosePrefabStage();

    auto prefabScene = std::make_unique<Engine::Scene::Scene>();
    Engine::Core::Object* root = nullptr;
    try
    {
        prefabScene->SetEditorMode2D(m_scene->IsEditorMode2D());
        prefabScene->Init(m_renderer->GetGraphicsProvider());
        prefabScene->SetRealtimeShadowSettings(
            m_projectSettings.realtimeShadows);
        prefabScene->SetDistanceLightingSettings(
            m_projectSettings.distanceLighting);
        root = Engine::Serialization::SceneSerializer::InstantiatePrefab(
            *prefabScene, normalized, prefabScene->GetGraphicsProvider());
    }
    catch (const std::exception& error)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not open prefab editor: " + std::string(error.what()));
        return;
    }
    if (!root)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not open prefab stage: " + normalized);
        return;
    }

    // In the isolated stage this is the editable asset root, not an instance.
    root->Prefab.reset();
    const std::string prefabName =
        std::filesystem::path(normalized).filename().string();
    auto sceneView = m_viewFactory->CreateSceneView(
        prefabScene.get(), prefabName, EditorPanelDockArea::MainDocument);
    if (!sceneView)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not create prefab editor viewport: " + normalized);
        return;
    }

    m_prefabScene = std::move(prefabScene);
    m_prefabSceneView = sceneView.get();
    m_prefabSceneView->SetDocumentPath(normalized);
    m_prefabSceneView->SetTitle(prefabName + "###PrefabDocument:" + normalized);
    m_prefabSceneView->OnObjectSelected = [this](Engine::Core::Object* object)
    {
        if (m_prefabScene) m_prefabScene->SetSelectedObject(object);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
    };
    m_prefabSceneView->OnObjectCreated = [this](Engine::Core::Object* object)
    {
        SetPrefabDirty(true);
        if (m_prefabScene) m_prefabScene->SetSelectedObject(object);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
    };
    m_prefabSceneView->OnDeleteSelectionRequested = [this]()
    {
        if (m_primaryHierarchy)
            m_primaryHierarchy->RequestDeleteSelectedObject();
    };
    m_prefabSceneView->OnGizmoInteraction = [this](bool active)
    {
        if (active) SetPrefabDirty(true);
    };
    m_prefabSceneView->OnMeshViewportInput = [this](IEditorUi& ui,
        const EditorUiViewportInput& input, EditorTransformTool tool)
    { return HandleMeshViewport(ui, input, m_prefabScene.get(), m_prefabMeshEdit, tool); };
    m_prefabSceneView->OnDrawDocumentTools = [this](IEditorUi& ui)
    {
        if (!ui.BeginViewportHeader("##PrefabEditMode", 230.f)) return false;
        int mode = m_prefabMeshEdit.enabled ? 1 : 0;
        const ViewportModeControl controls = DrawViewportModeControl(ui, mode);
        if (controls.changed)
            SetMeshEditMode(m_prefabScene.get(), m_prefabMeshEdit, mode == 1);
        if (m_prefabMeshEdit.enabled)
            SyncMeshEditSelection(m_prefabScene.get(), m_prefabMeshEdit);
        if (controls.showSave)
        {
            ui.SameLine();
            if (ui.Button("Save")) SaveScene();
        }
        const bool consumed = ui.EndViewportHeader();
        return consumed;
    };
    sceneView->OnFocused = [this]() { SetPrefabDocumentFocused(true); };
    sceneView->RequestFocusOnNextDraw();
    m_panels.push_back(std::move(sceneView));
    m_activePrefabPath = normalized;
    SetPrefabDirty(false);
    RefreshPrefabDocumentTitle();
    m_prefabScene->SetSelectedObject(root);
    SetPrefabDocumentFocused(true);
    if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(root);
    if (m_primaryProperties) m_primaryProperties->SetSelectedObject(root);
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            "Opened prefab stage: " + normalized);
}

void EditorState::ProcessPendingPrefabStageOpen()
{
    if (m_pendingPrefabPath.empty())
        return;

    // Opening a prefab adds a Scene panel. Do it after the panel draw loop so
    // growing m_panels cannot invalidate the iterator currently drawing Assets.
    std::string path = std::move(m_pendingPrefabPath);
    m_pendingPrefabPath.clear();
    OpenPrefabStage(path);
}

void EditorState::ProcessPendingEditToolsOpen()
{
    if (!m_pendingEditToolsOpen) return;
    m_pendingEditToolsOpen = false;
    for (const auto& panel : m_panels)
        if (auto* meshTools = dynamic_cast<EditToolsPanel*>(panel.get()))
        {
            meshTools->SetOpen(true);
            return;
        }
    auto meshTools = std::make_unique<EditToolsPanel>();
    meshTools->DrawTools = [this](IEditorUi& ui) { DrawEditTools(ui); };
    meshTools->SetOpen(true);
    m_panels.push_back(std::move(meshTools));
}

void EditorState::QueueAssetDocumentOpen(const std::string& path,
    const std::string& editorType)
{
    if (path.empty())
        return;
    if (editorType == "Scene Editor")
    {
        QueueSceneAssetDocumentOpen(path);
        return;
    }
    if (editorType == "Object Editor")
    {
        QueueSceneAssetDocumentOpen(path);
        return;
    }
    if (editorType == "Mesh Editor")
    {
        QueueSceneAssetDocumentOpen(path);
        return;
    }
    if (editorType == "Model Editor")
    {
        QueueSceneAssetDocumentOpen(path);
        return;
    }
    const std::string identity = AssetPathIdentity(path);
    for (AssetDocumentView* document : m_assetDocuments)
        if (document && AssetPathIdentity(document->GetPath()) == identity)
        {
            document->SetOpen(true);
            SetActiveAssetDocument(document);
            return;
        }
    for (const auto& pending : m_pendingAssetDocuments)
        if (AssetPathIdentity(pending.first) == identity)
            return;
    m_pendingAssetDocuments.emplace_back(path, editorType);
}

void EditorState::QueueSceneAssetDocumentOpen(const std::string& path)
{
    if (path.empty())
        return;
    const std::string identity = AssetPathIdentity(path);
    // Scene files use the persistent Scene view; other scene-backed assets
    // keep separate document tabs.
    if (std::filesystem::path(identity).extension() != ".scene")
        for (const auto& document : m_sceneAssetDocuments)
            if (document && document->identity == identity)
            {
                document->view->SetOpen(true);
                SetActiveSceneAssetDocument(document.get());
                return;
            }
    for (const std::string& pending : m_pendingSceneAssetDocuments)
        if (AssetPathIdentity(pending) == identity)
            return;
    m_pendingSceneAssetDocuments.push_back(path);
}

void EditorState::ProcessPendingSceneAssetDocumentOpens()
{
    while (!m_pendingSceneAssetDocuments.empty())
    {
        std::string path = std::move(m_pendingSceneAssetDocuments.front());
        m_pendingSceneAssetDocuments.pop_front();
        const std::string normalized = NormalizeAssetPath(path);
        const std::string identity = AssetPathIdentity(normalized);
        std::string extension = std::filesystem::path(normalized).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension == ".scene")
        {
            if (m_currentScenePath.empty() ||
                AssetPathIdentity(m_currentScenePath) != identity)
                RequestSceneLoad(normalized);
            SetActiveSceneAssetDocument(nullptr);
            bool foundSceneView = false;
            for (auto& panel : m_panels)
                if (auto* sceneView = dynamic_cast<SceneView*>(panel.get());
                    sceneView && sceneView->GetScene() == m_scene.get())
                {
                    sceneView->SetOpen(true);
                    sceneView->RequestFocusOnNextDraw();
                    foundSceneView = true;
                    break;
                }
            if (!foundSceneView && m_viewFactory)
                if (auto sceneView = m_viewFactory->Create("Scene"))
                {
                    sceneView->OnFocused = [this]()
                    {
                        SetActiveSceneAssetDocument(nullptr);
                        SetPrefabDocumentFocused(false);
                    };
                    if (auto* createdSceneView = dynamic_cast<SceneView*>(
                        sceneView.get()))
                        createdSceneView->RequestFocusOnNextDraw();
                    m_panels.push_back(std::move(sceneView));
                    RefreshSceneDocumentTitle();
                }
            if (m_renderer) m_renderer->MarkDirty();
            continue;
        }
        auto existing = std::find_if(m_sceneAssetDocuments.begin(),
            m_sceneAssetDocuments.end(), [&](const auto& document)
            {
                return document && document->identity == identity;
            });
        if (existing != m_sceneAssetDocuments.end())
        {
            (*existing)->view->SetOpen(true);
            SetActiveSceneAssetDocument(existing->get());
            continue;
        }

        auto document = std::make_unique<SceneAssetDocument>();
        document->path = normalized;
        document->identity = identity;
        document->prefab = extension == ".prefab";
        document->meshStage = extension == ".mesh" || extension == ".obj";
        document->modelStage = ModelImporter::SupportsExtension(extension);
        if (document->meshStage)
        {
            std::filesystem::path nativePath(normalized);
            nativePath.replace_extension(".mesh");
            document->meshSavePath = nativePath.string();
        }
        document->scene = std::make_unique<Engine::Scene::Scene>();
        document->scene->SetEditorMode2D(m_scene && m_scene->IsEditorMode2D());
        document->scene->Init(m_renderer->GetGraphicsProvider());
        document->scene->SetRealtimeShadowSettings(m_projectSettings.realtimeShadows);
        document->scene->SetDistanceLightingSettings(m_projectSettings.distanceLighting);
        bool loaded = false;
        if (document->meshStage)
        {
            Engine::Core::Object* subject = document->scene->AddObject(
                std::filesystem::path(normalized).stem().string());
            document->mesh = subject
                ? subject->AddComponent<Engine::Components::Mesh>() : nullptr;
            if (document->mesh)
            {
                document->mesh->LoadFromFile(normalized);
                if (document->scene->GetGraphicsProvider())
                    document->mesh->CreateBuffer(document->scene->
                        GetGraphicsProvider()->GetBufferFactory());
                subject->AddComponent<Engine::Components::Material>();
                document->subject = subject;
                document->scene->SetSelectedObject(subject);
                loaded = true;
            }
        }
        else if (document->modelStage)
        {
            const std::string assetsDirectory = m_projectSettings.assetsDirectory.empty()
                ? std::string("Assets") : m_projectSettings.assetsDirectory;
            const Engine::Model::ModelImportResult imported =
                ModelImporter::Import(normalized, assetsDirectory);
            if (imported.success)
            {
                document->prefab = true;
                document->stageDataPath = NormalizeAssetPath(imported.prefabPath);
                Engine::Core::Object* root = Engine::Serialization::
                    SceneSerializer::InstantiatePrefab(*document->scene,
                        document->stageDataPath,
                        document->scene->GetGraphicsProvider());
                if (root)
                {
                    root->Prefab.reset();
                    document->subject = root;
                    document->scene->SetSelectedObject(root);
                    loaded = true;
                }
            }
            else if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Could not import model for Asset Stage: " + imported.message);
        }
        else if (document->prefab)
        {
            Engine::Core::Object* root = Engine::Serialization::SceneSerializer::
                InstantiatePrefab(*document->scene,
                    document->stageDataPath.empty() ? normalized : document->stageDataPath,
                    document->scene->GetGraphicsProvider());
            if (root)
            {
                root->Prefab.reset();
                document->subject = root;
                document->scene->SetSelectedObject(root);
                loaded = true;
            }
        }
        else
        {
            loaded = document->scene->Load(normalized);
        }
        // Prefab documents use the regular scene editing controls for now.
        // Keep the specialized object/skeleton stages for model assets only.
        if (loaded && document->prefab && document->modelStage)
        {
            RebuildSkeletonStageContext(*document);
            if (!document->skeletonStage)
                RebuildObjectStageContext(*document);
        }
        if (loaded && document->meshStage)
        {
            document->meshEdit.enabled = true;
            document->scene->SetEditorMeshEditPose(true);
            ApplyMeshStageVisibility(*document);
        }
        if (document->skeletonStage && !document->selectableObjects.empty())
        {
            if (document->selectableObjects.find(document->subject) ==
                document->selectableObjects.end())
                document->subject = *document->selectableObjects.begin();
            document->scene->SetSelectedObject(document->subject);
        }
        if (!loaded)
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Could not open scene document: " + normalized);
            continue;
        }
        document->savedSnapshot = CaptureSceneAssetDocumentSnapshot(*document);
        document->baseline = document->savedSnapshot;
        const std::string filename = std::filesystem::path(normalized).filename().string();
        auto view = m_viewFactory->CreateSceneView(document->scene.get(), filename,
            EditorPanelDockArea::MainDocument);
        if (!view)
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Could not create scene document viewport: " + normalized);
            continue;
        }
        SceneAssetDocument* raw = document.get();
        raw->view = view.get();
        raw->view->SetDocumentPath(normalized);
        view->OnMeshViewportInput = [this, raw](IEditorUi& ui,
            const EditorUiViewportInput& input, EditorTransformTool tool)
        { return HandleMeshViewport(ui, input, raw->scene.get(), raw->meshEdit, tool); };
        RefreshSceneAssetDocumentTitle(*raw);
        view->OnFocused = [this, raw]() { SetActiveSceneAssetDocument(raw); };
        view->OnObjectSelected = [this, raw](Engine::Core::Object* object)
        {
            if (raw->skeletonStage && object &&
                raw->selectableObjects.find(object) ==
                    raw->selectableObjects.end())
                return;
            if (raw->objectStage && object)
            {
                bool inFocusedGraph = false;
                for (Engine::Core::Object* current = object; current;
                    current = current->Parent)
                    if (current == raw->subject)
                    {
                        inFocusedGraph = true;
                        break;
                    }
                if (!inFocusedGraph || (!raw->showChildHierarchy &&
                    object != raw->subject))
                    return;
            }
            raw->propertiesProjection = nullptr;
            raw->scene->SetSelectedObject(object);
            if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
            if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
        };
        view->OnObjectCreated = [this, raw](Engine::Core::Object* object)
        {
            raw->scene->SetSelectedObject(object);
            const std::string current = CaptureSceneAssetDocumentSnapshot(*raw);
            if (raw->baseline != current)
            {
                if (m_historyLimit > 0)
                {
                    if (raw->undo.size() >= m_historyLimit) raw->undo.pop_front();
                    raw->undo.push_back(raw->baseline);
                }
                raw->redo.clear();
                raw->baseline = current;
                raw->dirty = raw->baseline != raw->savedSnapshot;
                RefreshSceneAssetDocumentTitle(*raw);
            }
            if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(object);
            if (m_primaryProperties) m_primaryProperties->SetSelectedObject(object);
        };
        view->OnDeleteSelectionRequested = [this]()
        {
            if (m_primaryHierarchy) m_primaryHierarchy->RequestDeleteSelectedObject();
        };
        view->OnGizmoInteraction = [this, raw](bool active)
        {
            if (m_activeSceneAssetDocument == raw)
            {
                m_sceneEditInProgress = m_sceneEditInProgress || active;
                if (m_renderer) m_renderer->MarkDirty();
            }
        };
        view->CanSelectObject = [raw](const Engine::Core::Object* object)
        {
            if (!object) return true;
            if (raw->skeletonStage)
                return raw->selectableObjects.find(
                    const_cast<Engine::Core::Object*>(object)) !=
                    raw->selectableObjects.end();
            if (raw->meshStage)
                return object == raw->subject;
            if (raw->objectStage)
            {
                if (!raw->showChildHierarchy && object != raw->subject)
                    return false;
                for (const Engine::Core::Object* current = object; current;
                    current = current->Parent)
                    if (current == raw->subject) return true;
                return false;
            }
            return true;
        };
        const auto drawAssetFocusPicker = [this, raw](IEditorUi& ui)
        {
            if (!raw->prefab)
                return;
            std::vector<Engine::Components::Mesh*> meshes;
            Engine::Components::Skeleton* skeleton = nullptr;
            for (const auto& object : raw->scene->GetObjects())
            {
                if (!object) continue;
                for (Engine::Core::Component* component : object->Components)
                {
                    if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(component);
                        mesh && !mesh->GetVertices().empty())
                        meshes.push_back(mesh);
                    if (auto* candidate = dynamic_cast<Engine::Components::Skeleton*>(component))
                        skeleton = candidate;
                }
            }
            std::vector<const char*> choices{ "Object" };
            const int objectMode = 0;
            int meshMode = -1;
            int skeletonMode = -1;
            if (!meshes.empty())
            {
                meshMode = static_cast<int>(choices.size());
                choices.push_back("Mesh");
            }
            if (skeleton)
            {
                skeletonMode = static_cast<int>(choices.size());
                choices.push_back("Skeleton");
            }
            int selectedMode = raw->meshStage ? meshMode
                : (raw->skeletonStage ? skeletonMode : objectMode);
            if (selectedMode < 0) selectedMode = objectMode;
            if (ui.Combo("Mode", &selectedMode, choices.data(),
                static_cast<int>(choices.size())))
            {
                if (raw->previewAnimation)
                    StopSkeletonAnimationPreview(*raw);
                RestoreSkeletonStageVisibility(*raw);
                raw->meshStage = false;
                raw->objectStage = false;
                raw->skeletonStage = false;
                raw->propertiesProjection = nullptr;
                raw->selectedVertex = 0;
                if (selectedMode == meshMode && meshMode >= 0)
                {
                    raw->mesh = meshes.front();
                    raw->subject = raw->mesh->Owner;
                    raw->meshStage = raw->subject != nullptr;
                    raw->meshSavePath = raw->mesh->GetFilePath();
                    if (raw->meshSavePath.empty())
                        raw->meshSavePath = (std::filesystem::path(raw->path).parent_path() /
                            (raw->subject->name + ".mesh")).string();
                    raw->scene->SetSelectedObject(raw->subject);
                    ApplyMeshStageVisibility(*raw);
                }
                else if (selectedMode == skeletonMode && skeletonMode >= 0)
                    RebuildSkeletonStageContext(*raw);
                else
                    RebuildObjectStageContext(*raw);
                raw->meshEdit.enabled = raw->meshStage;
                raw->scene->SetEditorMeshEditPose(raw->meshStage);
                if (raw->view)
                {
                    raw->view->AllowObjectCreation = false;
                    raw->view->AllowAssetDrops = false;
                    raw->view->AllowObjectTransform = !raw->meshStage;
                }
                SetActiveSceneAssetDocument(raw);
                m_pendingEditToolsOpen = true;
                if (m_renderer) m_renderer->MarkDirty();
            }
        };
        if (raw->prefab && raw->modelStage)
            raw->objectStageTools = [this, raw](IEditorUi& ui)
            {
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
            };
        if (raw->prefab && raw->modelStage && raw->skeleton)
        {
            view->AllowObjectCreation = false;
            view->AllowAssetDrops = false;
            raw->skeletonStageTools = [this, raw](IEditorUi& ui)
            {
                if (raw->objectStage)
                {
                    ui.ColoredLabel("Object Edit Stage",
                        { 0.35f, 0.75f, 1.f, 1.f });
                    if (ui.Checkbox("Include child hierarchy",
                        &raw->showChildHierarchy))
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
                    return;
                }
                if (!raw->skeletonStage) return;
                ui.ColoredLabel("Skeleton Edit Stage", { 0.35f, 0.75f, 1.f, 1.f });
                const std::string count = std::to_string(raw->selectableObjects.size());
                ui.ValueLabel("Editable joints", count.c_str());
                std::vector<Engine::Core::Object*> dependencyTargets;
                std::vector<std::string> dependencyLabels;
                const auto addDependencyTarget = [&](Engine::Core::Object* object,
                    const std::string& role)
                {
                    if (!object || std::find(dependencyTargets.begin(),
                        dependencyTargets.end(), object) != dependencyTargets.end())
                        return;
                    dependencyTargets.push_back(object);
                    dependencyLabels.push_back(role + ": " + object->name);
                };
                Engine::Components::Model* rigModel = raw->skeleton
                    ? raw->skeleton->ResolveModel() : nullptr;
                addDependencyTarget(rigModel ? rigModel->Owner : nullptr, "Model");
                for (Engine::Core::Object* ghost : raw->skeletonMeshObjects)
                    addDependencyTarget(ghost, "Skinned mesh");
                std::vector<const char*> dependencyNames;
                dependencyNames.push_back("Selected bone");
                for (const std::string& label : dependencyLabels)
                    dependencyNames.push_back(label.c_str());
                int dependencySelection = 0;
                for (size_t index = 0; index < dependencyTargets.size(); ++index)
                    if (dependencyTargets[index] == raw->propertiesProjection)
                        dependencySelection = static_cast<int>(index + 1);
                if (ui.Combo("Properties target", &dependencySelection,
                    dependencyNames.data(), static_cast<int>(dependencyNames.size())))
                {
                    raw->propertiesProjection = dependencySelection > 0
                        ? dependencyTargets[static_cast<size_t>(dependencySelection - 1)]
                        : nullptr;
                    if (m_primaryProperties)
                        m_primaryProperties->SetSelectedObject(
                            raw->propertiesProjection ? raw->propertiesProjection
                                : raw->scene->GetSelectedObject());
                }
                if (rigModel && rigModel->Owner &&
                    !rigModel->Owner->GetComponent<
                        Engine::Components::AnimationManager>() &&
                    ui.Button("Add Animation Manager Dependency"))
                {
                    rigModel->Owner->AddComponent<
                        Engine::Components::AnimationManager>();
                    raw->propertiesProjection = rigModel->Owner;
                    if (m_primaryProperties)
                        m_primaryProperties->SetSelectedObject(rigModel->Owner);
                    const std::string current =
                        CaptureSceneAssetDocumentSnapshot(*raw);
                    if (raw->baseline != current)
                    {
                        if (m_historyLimit > 0)
                        {
                            if (raw->undo.size() >= m_historyLimit)
                                raw->undo.pop_front();
                            raw->undo.push_back(raw->baseline);
                        }
                        raw->redo.clear();
                        raw->baseline = current;
                        raw->dirty = raw->baseline != raw->savedSnapshot;
                        RefreshSceneAssetDocumentTitle(*raw);
                    }
                }
                if (!raw->skeletonMeshObjects.empty() &&
                    ui.Checkbox("Show associated skinned mesh (ghost)",
                        &raw->showSkeletonMesh))
                {
                    ApplySkeletonStageVisibility(*raw);
                    if (m_renderer) m_renderer->MarkDirty();
                }
                ui.BeginDisabled(raw->previewAnimation);
                if (raw->skeleton && ui.Checkbox("Show bone overlays",
                    &raw->skeleton->showBones) && m_renderer)
                    m_renderer->MarkDirty();
                ui.EndDisabled();
                bool preview = raw->previewAnimation;
                if (ui.Checkbox("Preview animation", &preview))
                {
                    if (preview)
                    {
                        Engine::Components::AnimationManager* manager = nullptr;
                        if (raw->skeleton)
                            if (Engine::Components::Model* model =
                                raw->skeleton->ResolveModel())
                                manager = model->Owner
                                    ? model->Owner->GetComponent<
                                        Engine::Components::AnimationManager>()
                                    : nullptr;
                        if (!manager)
                            for (const auto& object : raw->scene->GetObjects())
                                if (object && (manager = object->GetComponent<
                                    Engine::Components::AnimationManager>()))
                                    break;
                        if (manager)
                        {
                            raw->previewRestoreSnapshot =
                                CaptureSceneAssetDocumentSnapshot(*raw);
                            raw->previewAnimationManager = manager;
                            raw->previewAnimation = true;
                            manager->playing = true;
                            raw->lastPreviewTick =
                                std::chrono::steady_clock::now();
                        }
                        else if (m_primaryConsole)
                            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                                "This skeleton has no AnimationManager to preview.");
                    }
                    else
                        StopSkeletonAnimationPreview(*raw);
                }
                if (raw->previewAnimation && raw->previewAnimationManager)
                {
                    const auto now = std::chrono::steady_clock::now();
                    const float delta = std::chrono::duration<float>(
                        now - raw->lastPreviewTick).count();
                    raw->lastPreviewTick = now;
                    raw->previewAnimationManager->Tick(delta);
                    if (m_renderer) m_renderer->MarkDirty();
                }
                Engine::Components::AnimationManager* manager =
                    raw->previewAnimationManager;
                if (!manager && raw->skeleton)
                    if (Engine::Components::Model* model =
                        raw->skeleton->ResolveModel())
                        manager = model->Owner ? model->Owner->GetComponent<
                            Engine::Components::AnimationManager>() : nullptr;
                if (manager)
                {
                    const auto clips = manager->GetAvailableClips();
                    std::vector<const char*> clipNames;
                    int selectedClip = 0;
                    for (size_t i = 0; i < clips.size(); ++i)
                    {
                        clipNames.push_back(clips[i]->clipName.c_str());
                        if (clips[i]->clipName == manager->clip)
                            selectedClip = static_cast<int>(i);
                    }
                    ui.BeginDisabled(!raw->previewAnimation);
                    if (!clipNames.empty() && ui.Combo("Animation clip",
                        &selectedClip, clipNames.data(),
                        static_cast<int>(clipNames.size())))
                        manager->Play(clips[static_cast<size_t>(selectedClip)]->clipName, 0.f);
                    ui.Checkbox("Playing", &manager->playing);
                    ui.Checkbox("Loop", &manager->looping);
                    ui.DragFloat("Playback speed", &manager->speed, 0.05f,
                        -10.f, 10.f);
                    float clipDuration = 0.f;
                    if (selectedClip >= 0 &&
                        static_cast<size_t>(selectedClip) < clips.size())
                        clipDuration = clips[static_cast<size_t>(selectedClip)]->duration;
                    if (clipDuration > 0.f)
                        ui.SliderFloat("Time", &manager->time, 0.f, clipDuration);
                    ui.EndDisabled();
                }
                ui.BeginDisabled(raw->previewAnimation);
                if (ui.Button("Apply Rest Pose"))
                {
                    Engine::Components::Model* model = raw->skeleton
                        ? raw->skeleton->ResolveModel() : nullptr;
                    Engine::Components::Transform* modelTransform = model && model->Owner
                        ? &model->Owner->transform : nullptr;
                    const auto& joints = raw->skeleton
                        ? raw->skeleton->ResolveJoints()
                        : std::vector<Engine::Core::Object*>{};
                    bool valid = raw->skeleton && modelTransform &&
                        !joints.empty() && joints.size() ==
                            raw->skeleton->jointNodes.size();
                    std::vector<glm::mat4> inverseBindMatrices;
                    if (valid)
                    {
                        const glm::mat4 modelWorld = modelTransform->GetWorldMatrix();
                        const float modelDeterminant = glm::determinant(modelWorld);
                        valid = std::isfinite(modelDeterminant) &&
                            std::abs(modelDeterminant) > 1e-8f;
                        const glm::mat4 inverseModel = valid
                            ? glm::inverse(modelWorld) : glm::mat4(1.f);
                        for (Engine::Core::Object* joint : joints)
                        {
                            if (!joint) { valid = false; break; }
                            const glm::mat4 jointInModel = inverseModel *
                                joint->transform.GetWorldMatrix();
                            const float determinant = glm::determinant(jointInModel);
                            if (!std::isfinite(determinant) ||
                                std::abs(determinant) <= 1e-8f)
                            {
                                valid = false;
                                break;
                            }
                            inverseBindMatrices.push_back(glm::inverse(jointInModel));
                        }
                    }
                    if (valid)
                    {
                        raw->skeleton->inverseBindMatrices =
                            std::move(inverseBindMatrices);
                        if (m_renderer) m_renderer->MarkDirty();
                    }
                    else if (m_primaryConsole)
                        m_primaryConsole->AddLog(ConsoleView::Level::Error,
                            "Cannot apply rest pose: a model or valid joint transform is missing.");
                }
                ui.EndDisabled();
                ui.DisabledLabel("Joint hierarchy is filtered to this skeleton.");
            };
        }
        if (raw->meshStage || raw->prefab)
        {
            view->AllowObjectCreation = false;
            view->AllowAssetDrops = false;
            view->AllowObjectTransform = !raw->meshStage;
            raw->meshStageTools = [this, raw](IEditorUi& ui)
            {
                if (!raw->meshStage) return;
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
                        SetActiveSceneAssetDocument(raw);
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
                ui.ColoredLabel("Mesh Edit Stage", { 0.35f, 0.75f, 1.f, 1.f });
                ui.ValueLabel("Save target", raw->meshSavePath.c_str());
                const std::string count = std::to_string(vertices.size());
                ui.ValueLabel("Triangle-list vertices", count.c_str());
                int selected = static_cast<int>(std::min<uint32_t>(
                    raw->selectedVertex, static_cast<uint32_t>(vertices.size() - 1)));
                if (vertices.size() > 1 && ui.SliderInt("Vertex", &selected, 0,
                    static_cast<int>(vertices.size() - 1)))
                    raw->selectedVertex = static_cast<uint32_t>(selected);
                auto& vertex = vertices[raw->selectedVertex];
                bool changed = ui.DragFloat3("Position", vertex.pos, 0.01f);
                changed |= ui.DragFloat("UV U", &vertex.uv[0], 0.005f);
                changed |= ui.DragFloat("UV V", &vertex.uv[1], 0.005f);
                changed |= ui.ColorEdit4("Vertex Color", vertex.color);
                std::vector<float> uvPairs;
                uvPairs.reserve(vertices.size() * 2);
                for (const auto& item : vertices)
                {
                    uvPairs.push_back(item.uv[0]);
                    uvPairs.push_back(item.uv[1]);
                }
                const auto& indices = raw->mesh->GetIndices();
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
                if (ui.Button("Save Mesh"))
                    SaveSceneAssetDocument(*raw);
            };
        }
        if (raw->prefab && raw->modelStage)
            view->OnDrawDocumentTools = [this, raw, drawAssetFocusPicker](IEditorUi& ui)
            {
                ui.PushId(raw);
                const std::string headerId = "##AssetHeader:" + raw->identity;
                if (!ui.BeginViewportHeader(headerId.c_str(), 380.f))
                {
                    ui.PopId();
                    return false;
                }
                const float contentWidth = ui.AvailableContentWidth();
                ui.SetNextItemWidth(contentWidth >= 280.f ? 100.f : 72.f);
                drawAssetFocusPicker(ui);
                ui.SameLine();
                const float toolsWidth = std::clamp(
                    ui.AvailableContentWidth() - 62.f, 88.f, 130.f);
                const char* toolsLabel = raw->meshStage ? "Mesh Tools"
                    : (raw->skeletonStage ? "Skeleton Tools" : "Object Tools");
                if (ui.BeginViewportHeaderDropdown("##AssetStageTools",
                    toolsLabel, toolsWidth, 320.f))
                {
                    if (raw->objectStage && raw->objectStageTools)
                        raw->objectStageTools(ui);
                    else if (raw->meshStage && raw->meshStageTools)
                        raw->meshStageTools(ui);
                    else if (raw->skeletonStage && raw->skeletonStageTools)
                        raw->skeletonStageTools(ui);
                    ui.EndViewportHeaderDropdown();
                }
                ui.SameLine();
                if (ui.AvailableContentWidth() >= 54.f && ui.Button("Save"))
                    SaveSceneAssetDocument(*raw);
                const bool consumedClick = ui.EndViewportHeader();
                ui.PopId();
                return consumedClick;
            };
        else if (raw->prefab)
            view->OnDrawDocumentTools = [this, raw](IEditorUi& ui)
            {
                ui.PushId(raw);
                const std::string headerId = "##PrefabHeader:" + raw->identity;
                if (!ui.BeginViewportHeader(headerId.c_str(), 230.f))
                {
                    ui.PopId();
                    return false;
                }
                int mode = raw->meshEdit.enabled ? 1 : 0;
                const ViewportModeControl controls = DrawViewportModeControl(ui, mode);
                if (controls.changed)
                    SetMeshEditMode(raw->scene.get(), raw->meshEdit, mode == 1);
                if (raw->meshEdit.enabled)
                    SyncMeshEditSelection(raw->scene.get(), raw->meshEdit);
                if (controls.showSave)
                {
                    ui.SameLine();
                    if (ui.Button("Save")) SaveSceneAssetDocument(*raw);
                }
                const bool consumedClick = ui.EndViewportHeader();
                ui.PopId();
                return consumedClick;
            };
        else if (!raw->meshStage)
            view->OnDrawDocumentTools = [this, raw](IEditorUi& ui)
            {
                ui.PushId(raw);
                if (!ui.BeginViewportHeader("##SceneAssetEditMode", 230.f))
                {
                    ui.PopId();
                    return false;
                }
                int mode = raw->meshEdit.enabled ? 1 : 0;
                const ViewportModeControl controls = DrawViewportModeControl(ui, mode);
                if (controls.changed)
                    SetMeshEditMode(raw->scene.get(), raw->meshEdit, mode == 1);
                if (raw->meshEdit.enabled)
                    SyncMeshEditSelection(raw->scene.get(), raw->meshEdit);
                if (controls.showSave)
                {
                    ui.SameLine();
                    if (ui.Button("Save")) SaveSceneAssetDocument(*raw);
                }
                const bool consumed = ui.EndViewportHeader();
                ui.PopId();
                return consumed;
            };
        else if (raw->meshStage)
            view->OnDrawDocumentTools = [this, raw](IEditorUi& ui)
            {
                ui.PushId(raw);
                const std::string headerId = "##MeshHeader:" + raw->identity;
                if (!ui.BeginViewportHeader(headerId.c_str(), 300.f))
                {
                    ui.PopId();
                    return false;
                }
                ui.Label("Mesh");
                ui.SameLine();
                const float toolsWidth = std::min(150.f,
                    std::max(100.f, ui.AvailableContentWidth() - 70.f));
                if (ui.BeginViewportHeaderDropdown("##MeshStageTools",
                    "Mesh Tools", toolsWidth, 320.f))
                {
                    if (raw->meshStageTools)
                        raw->meshStageTools(ui);
                    ui.EndViewportHeaderDropdown();
                }
                ui.SameLine();
                if (ui.AvailableContentWidth() >= 54.f && ui.Button("Save"))
                    SaveSceneAssetDocument(*raw);
                const bool consumedClick = ui.EndViewportHeader();
                ui.PopId();
                return consumedClick;
            };
        if (raw->objectStage)
        {
            view->AllowObjectCreation = false;
            view->AllowAssetDrops = false;
        }
        view->RequestFocusOnNextDraw();
        m_panels.push_back(std::move(view));
        m_sceneAssetDocuments.push_back(std::move(document));
        SetActiveSceneAssetDocument(raw);
    }
}

void EditorState::SetActiveSceneAssetDocument(SceneAssetDocument* document)
{
    m_activeSceneAssetDocument = document;
    m_activeAssetDocument = nullptr;
    m_prefabDocumentFocused = document && document->prefab;
    const auto scene = document ? document->scene.get() : m_scene.get();
    for (auto& panel : m_panels)
        if (auto* gameView = dynamic_cast<GameView*>(panel.get()))
            gameView->SetScene(scene);
    if (m_primaryHierarchy)
    {
        m_primaryHierarchy->Init(scene);
        m_primaryHierarchy->SetObjectFilter(document &&
            (document->skeletonStage || document->meshStage || document->objectStage)
            ? std::function<bool(const Engine::Core::Object*)>(
                [document](const Engine::Core::Object* object)
                {
                    return document->meshStage
                        ? object == document->subject
                        : (document->objectStage
                            ? (object == document->subject ||
                                (document->showChildHierarchy && [&]()
                                {
                                    for (const Engine::Core::Object* current = object;
                                        current; current = current->Parent)
                                        if (current == document->subject) return true;
                                    return false;
                                }()))
                            : document->selectableObjects.find(
                            const_cast<Engine::Core::Object*>(object)) !=
                            document->selectableObjects.end());
                })
            : std::function<bool(const Engine::Core::Object*)>{});
        m_primaryHierarchy->SetAllowDelete(!document ||
            (!document->meshStage && !document->skeletonStage &&
                !document->objectStage));
        m_primaryHierarchy->SetFilteredObjectContextActions(document &&
            document->objectStage && !document->modelStage,
            document ? document->subject : nullptr);
        m_primaryHierarchy->SetSkeletonContextActions(document &&
            document->skeletonStage,
            document ? document->subject : nullptr);
        Engine::Core::Object* selected = scene ? scene->GetSelectedObject() : nullptr;
        if (document && document->meshStage && selected != document->subject)
            selected = document->subject;
        if (document && document->objectStage && !document->showChildHierarchy &&
            selected != document->subject)
            selected = document->subject;
        if (document && document->skeletonStage && selected &&
            document->selectableObjects.find(selected) ==
                document->selectableObjects.end())
            selected = document->subject;
        m_primaryHierarchy->SetSelectedObject(selected);
    }
    if (m_primaryProperties)
    {
        m_primaryProperties->Init(scene);
        m_primaryProperties->SetAllowComponentStructureEdits(!document ||
            (!document->meshStage && !document->skeletonStage));
        m_primaryProperties->SetSelectedObject(document &&
            document->propertiesProjection
            ? document->propertiesProjection
            : (scene ? scene->GetSelectedObject() : nullptr));
        m_primaryProperties->SetSelectedAsset("");
    }
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::RefreshSceneAssetDocumentTitle(SceneAssetDocument& document)
{
    if (!document.view) return;
    std::string title = std::filesystem::path(document.path).filename().string();
    if (document.dirty) title += " *";
    document.view->SetTitle(title + "###SceneAssetDocument:" + document.identity);
}

bool EditorState::SaveSceneAssetDocument(SceneAssetDocument& document)
{
    if (document.meshEdit.dirty &&
        !SaveMeshEditSession(document.meshEdit)) return false;
    if (!SavePendingMeshEdits()) return false;
    bool saved = false;
    const std::string stageSavePath = document.stageDataPath.empty()
        ? document.path : document.stageDataPath;
    if (document.previewAnimation)
        StopSkeletonAnimationPreview(document);
    if (document.skeletonStage || document.objectStage)
        RestoreSkeletonStageVisibility(document);
    if (document.meshStage && document.mesh)
        saved = Engine::Components::Mesh::SaveNativeFile(
            document.meshSavePath, document.mesh->GetVertices(),
            document.mesh->GetIndices());
    else if (document.scene && document.prefab)
    {
        Engine::Core::Object* root = nullptr;
        for (const auto& object : document.scene->GetObjects())
            if (object && !object->Parent)
            {
                if (root) { root = nullptr; break; }
                root = object.get();
            }
        saved = root && Engine::Serialization::SceneSerializer::SavePrefab(
            *root, stageSavePath);
    }
    else if (document.scene)
        saved = document.scene->Save(document.path);
    if (document.skeletonStage)
        ApplySkeletonStageVisibility(document);
    else if (document.objectStage)
        ApplyObjectStageVisibility(document);
    if (!saved)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Failed to save scene document: " + document.path);
        return false;
    }
    document.savedSnapshot = CaptureSceneAssetDocumentSnapshot(document);
    document.baseline = document.savedSnapshot;
    document.dirty = false;
    RefreshSceneAssetDocumentTitle(document);
    if (!document.meshStage)
        AssetPreviewCache::CaptureScene(document.path, *document.scene,
            document.scene->GetGraphicsProvider());
    if (document.prefab && m_scene)
        Engine::Serialization::SceneSerializer::RefreshPrefabInstances(*m_scene,
            stageSavePath, m_scene->GetGraphicsProvider());
    if (document.prefab)
        for (const auto& other : m_sceneAssetDocuments)
            if (other && other.get() != &document && other->scene)
                Engine::Serialization::SceneSerializer::RefreshPrefabInstances(
                    *other->scene, stageSavePath,
                    other->scene->GetGraphicsProvider());
    if (document.meshStage && m_viewFactory &&
        m_viewFactory->OnAssetContentsChanged)
        m_viewFactory->OnAssetContentsChanged(document.meshSavePath);
    if (document.meshStage)
    {
        // Skeleton stages retain live component pointers, so the general scene
        // reload callback intentionally skips them. Refresh matching meshes in
        // place after the replacement file becomes visible.
        for (const auto& other : m_sceneAssetDocuments)
            if (other && other.get() != &document && other->skeletonStage &&
                other->scene)
                for (const auto& object : other->scene->GetObjects())
                    if (object)
                        for (Engine::Core::Component* component : object->Components)
                            if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(
                                    component);
                                mesh && AssetPathIdentity(mesh->GetFilePath()) ==
                                    AssetPathIdentity(document.meshSavePath))
                            {
                                mesh->LoadFromFile(mesh->GetFilePath());
                                if (auto* provider = other->scene->GetGraphicsProvider())
                                    mesh->CreateBuffer(provider->GetBufferFactory());
                            }
    }
    return true;
}

std::string EditorState::CaptureSceneAssetDocumentSnapshot(
    SceneAssetDocument& document)
{
    if (document.skeletonStage || document.objectStage)
    {
        RestoreSkeletonStageVisibility(document);
        const std::string snapshot = document.scene
            ? document.scene->SaveToString() : std::string{};
        if (document.skeletonStage)
            ApplySkeletonStageVisibility(document);
        else
            ApplyObjectStageVisibility(document);
        return snapshot;
    }
    if (!document.meshStage || !document.mesh)
        return document.scene ? document.scene->SaveToString() : std::string{};
    const auto& vertices = document.mesh->GetVertices();
    const auto& indices = document.mesh->GetIndices();
    const uint32_t counts[] = { static_cast<uint32_t>(vertices.size()),
        static_cast<uint32_t>(indices.size()) };
    std::string snapshot(reinterpret_cast<const char*>(counts), sizeof(counts));
    snapshot.append(reinterpret_cast<const char*>(vertices.data()),
        vertices.size() * sizeof(Engine::Components::Mesh::Vertex));
    snapshot.append(reinterpret_cast<const char*>(indices.data()),
        indices.size() * sizeof(uint32_t));
    return snapshot;
}

bool EditorState::RestoreSceneAssetDocumentSnapshot(
    SceneAssetDocument& document, const std::string& snapshot)
{
    if (!document.meshStage)
    {
        if (!document.scene || !document.scene->LoadFromString(snapshot))
            return false;
        if (document.skeletonStage)
            RebuildSkeletonStageContext(document);
        else if (document.objectStage)
            RebuildObjectStageContext(document);
        return true;
    }
    if (!document.mesh || snapshot.size() < 2 * sizeof(uint32_t))
        return false;
    uint32_t counts[2]{};
    std::memcpy(counts, snapshot.data(), sizeof(counts));
    const size_t vertexBytes = static_cast<size_t>(counts[0]) *
        sizeof(Engine::Components::Mesh::Vertex);
    const size_t indexBytes = static_cast<size_t>(counts[1]) * sizeof(uint32_t);
    if (counts[0] == 0 || vertexBytes > snapshot.size() - sizeof(counts) ||
        indexBytes != snapshot.size() - sizeof(counts) - vertexBytes)
        return false;
    std::vector<Engine::Components::Mesh::Vertex> vertices(counts[0]);
    std::vector<uint32_t> indices(counts[1]);
    std::memcpy(vertices.data(), snapshot.data() + sizeof(counts), vertexBytes);
    std::memcpy(indices.data(), snapshot.data() + sizeof(counts) + vertexBytes,
        indexBytes);
    return indices.empty()
        ? document.mesh->SetAuthoredVertices(std::move(vertices))
        : document.mesh->SetIndexedGeometry(std::move(vertices),
            std::move(indices));
}

void EditorState::ApplySkeletonStageVisibility(SceneAssetDocument& document)
{
    if (!document.scene || !document.skeletonStage)
        return;
    if (document.originalEnabledState.empty())
        for (const auto& object : document.scene->GetObjects())
            if (object)
                document.originalEnabledState.emplace(object.get(), object->enabled);
    for (const auto& [object, enabled] : document.originalEnabledState)
        if (object) object->enabled = enabled;

    std::unordered_set<Engine::Core::Object*> visible;
    const auto includeAncestors = [&visible](Engine::Core::Object* object)
    {
        for (Engine::Core::Object* current = object; current; current = current->Parent)
            visible.insert(current);
    };
    for (Engine::Core::Object* bone : document.selectableObjects)
        includeAncestors(bone);
    if (document.showSkeletonMesh)
        for (Engine::Core::Object* mesh : document.skeletonMeshObjects)
            includeAncestors(mesh);

    for (const auto& object : document.scene->GetObjects())
        if (object)
        {
            const auto original = document.originalEnabledState.find(object.get());
            object->enabled = original != document.originalEnabledState.end() &&
                original->second && visible.find(object.get()) != visible.end();
            for (Engine::Core::Component* component : object->Components)
            {
                if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(component))
                {
                    if (!document.originalMeshVisibility.count(mesh))
                        document.originalMeshVisibility.emplace(mesh,
                            mesh->IsEditorVisible());
                    const bool associatedGhost = document.showSkeletonMesh &&
                        document.skeletonMeshObjects.count(object.get()) != 0;
                    mesh->SetEditorVisible(associatedGhost);
                }
                if (auto* sprite = dynamic_cast<Engine::Components::Sprite*>(component))
                {
                    if (!document.originalSpriteVisibility.count(sprite))
                        document.originalSpriteVisibility.emplace(sprite,
                            sprite->IsEditorVisible());
                    sprite->SetEditorVisible(false);
                }
            }
        }

    if (document.ghostMaterialStates.empty())
        for (Engine::Core::Object* object : document.skeletonMeshObjects)
            if (object)
                for (Engine::Core::Component* component : object->Components)
                    if (auto* material = dynamic_cast<Engine::Components::Material*>(component))
                        document.ghostMaterialStates.push_back({ material,
                            material->baseColorAlpha, material->alphaMode,
                            material->castsShadows });
    for (auto& state : document.ghostMaterialStates)
        if (state.material)
        {
            state.material->baseColorAlpha = document.showSkeletonMesh
                ? 0.24f : state.alpha;
            state.material->alphaMode = document.showSkeletonMesh
                ? "Blend" : state.alphaMode;
            state.material->castsShadows = document.showSkeletonMesh
                ? false : state.castsShadows;
        }
}

void EditorState::RestoreSkeletonStageVisibility(SceneAssetDocument& document)
{
    for (const auto& [object, enabled] : document.originalEnabledState)
        if (object) object->enabled = enabled;
    for (auto& state : document.ghostMaterialStates)
        if (state.material)
        {
            state.material->baseColorAlpha = state.alpha;
            state.material->alphaMode = state.alphaMode;
            state.material->castsShadows = state.castsShadows;
        }
    for (const auto& [mesh, visible] : document.originalMeshVisibility)
        if (mesh) mesh->SetEditorVisible(visible);
    document.originalMeshVisibility.clear();
    for (const auto& [sprite, visible] : document.originalSpriteVisibility)
        if (sprite) sprite->SetEditorVisible(visible);
    document.originalSpriteVisibility.clear();
}

void EditorState::RebuildSkeletonStageContext(SceneAssetDocument& document)
{
    document.skeletonStage = false;
    document.skeleton = nullptr;
    document.selectableObjects.clear();
    document.skeletonMeshObjects.clear();
    document.originalEnabledState.clear();
    document.ghostMaterialStates.clear();
    if (!document.scene)
        return;
    for (const auto& object : document.scene->GetObjects())
        for (Engine::Core::Component* component : object->Components)
            if (auto* skeleton = dynamic_cast<Engine::Components::Skeleton*>(component))
            {
                document.skeletonStage = true;
                document.skeleton = skeleton;
                for (Engine::Components::AnimationBone* bone : skeleton->ResolveBones())
                    if (bone && bone->Owner)
                        document.selectableObjects.insert(bone->Owner);
                document.subject = skeleton->GetHierarchyRoot();
                break;
            }
    if (!document.skeletonStage)
        return;
    for (const auto& object : document.scene->GetObjects())
        for (Engine::Core::Component* component : object->Components)
            if (auto* skinned = dynamic_cast<Engine::Components::SkinnedMesh*>(component))
            {
                Engine::Components::Skeleton* bound =
                    skinned->skeletonReference.IsAssigned()
                    ? Engine::Core::ResolveComponentReference<
                        Engine::Components::Skeleton>(skinned->Owner,
                            skinned->skeletonReference)
                    : nullptr;
                if (bound == document.skeleton ||
                    (!bound && skinned->skinIndex >= 0 &&
                        static_cast<unsigned>(skinned->skinIndex) ==
                            document.skeleton->skinIndex))
                    document.skeletonMeshObjects.insert(object.get());
            }
    if (!document.selectableObjects.empty() &&
        document.selectableObjects.find(document.subject) ==
            document.selectableObjects.end())
        document.subject = *document.selectableObjects.begin();
    document.scene->SetSelectedObject(document.subject);
    ApplySkeletonStageVisibility(document);
}

void EditorState::RebuildObjectStageContext(SceneAssetDocument& document)
{
    document.objectStage = false;
    document.originalEnabledState.clear();
    document.ghostMaterialStates.clear();
    if (!document.scene || !document.prefab || document.skeletonStage)
        return;
    Engine::Core::Object* root = nullptr;
    for (const auto& object : document.scene->GetObjects())
        if (object && !object->Parent)
        {
            if (root) return;
            root = object.get();
        }
    if (!root) return;
    document.objectStage = true;
    document.subject = root;
    document.scene->SetSelectedObject(root);
    ApplyObjectStageVisibility(document);
}

void EditorState::ApplyObjectStageVisibility(SceneAssetDocument& document)
{
    if (!document.scene || !document.objectStage || !document.subject)
        return;
    if (document.originalEnabledState.empty())
        for (const auto& object : document.scene->GetObjects())
            if (object)
                document.originalEnabledState.emplace(object.get(), object->enabled);
    std::unordered_set<Engine::Core::Object*> visible;
    visible.insert(document.subject);
    if (document.showChildHierarchy)
    {
        std::vector<Engine::Core::Object*> pending(
            document.subject->Children.begin(), document.subject->Children.end());
        while (!pending.empty())
        {
            Engine::Core::Object* child = pending.back();
            pending.pop_back();
            if (!child || !visible.insert(child).second) continue;
            pending.insert(pending.end(), child->Children.begin(), child->Children.end());
        }
    }
    for (const auto& object : document.scene->GetObjects())
        if (object)
        {
            const auto original = document.originalEnabledState.find(object.get());
            object->enabled = original != document.originalEnabledState.end() &&
                original->second && visible.find(object.get()) != visible.end();
            for (Engine::Core::Component* component : object->Components)
            {
                if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(component))
                {
                    if (!document.originalMeshVisibility.count(mesh))
                        document.originalMeshVisibility.emplace(mesh,
                            mesh->IsEditorVisible());
                    mesh->SetEditorVisible(visible.count(object.get()) != 0);
                }
                if (auto* sprite = dynamic_cast<Engine::Components::Sprite*>(component))
                {
                    if (!document.originalSpriteVisibility.count(sprite))
                        document.originalSpriteVisibility.emplace(sprite,
                            sprite->IsEditorVisible());
                    sprite->SetEditorVisible(visible.count(object.get()) != 0);
                }
            }
        }
}

void EditorState::ApplyMeshStageVisibility(SceneAssetDocument& document)
{
    if (!document.scene || !document.meshStage || !document.mesh ||
        !document.subject)
        return;
    if (document.originalEnabledState.empty())
        for (const auto& object : document.scene->GetObjects())
            if (object)
                document.originalEnabledState.emplace(object.get(), object->enabled);
    std::unordered_set<Engine::Core::Object*> visible;
    for (Engine::Core::Object* current = document.subject; current;
        current = current->Parent)
        visible.insert(current);
    for (Engine::Core::Component* component : document.subject->Components)
        if (auto* skinned = dynamic_cast<Engine::Components::SkinnedMesh*>(component))
        {
            Engine::Components::Skeleton* bound =
                skinned->skeletonReference.IsAssigned()
                ? Engine::Core::ResolveComponentReference<
                    Engine::Components::Skeleton>(skinned->Owner,
                        skinned->skeletonReference)
                : nullptr;
            if (!bound)
                for (const auto& candidate : document.scene->GetObjects())
                    if (candidate)
                        if (auto* skeleton = candidate->GetComponent<
                            Engine::Components::Skeleton>(); skeleton &&
                            skeleton->skinIndex == skinned->skinIndex)
                        {
                            bound = skeleton;
                            break;
                        }
            if (!bound) continue;
            document.skeleton = bound;
            for (Engine::Components::AnimationBone* bone : bound->ResolveBones())
                if (bone && bone->Owner)
                    for (Engine::Core::Object* current = bone->Owner; current;
                        current = current->Parent)
                        visible.insert(current);
        }
    for (const auto& object : document.scene->GetObjects())
        if (object)
        {
            const auto original = document.originalEnabledState.find(object.get());
            object->enabled = original != document.originalEnabledState.end() &&
                original->second && visible.count(object.get()) != 0;
            for (Engine::Core::Component* component : object->Components)
            {
                if (auto* mesh = dynamic_cast<Engine::Components::Mesh*>(component))
                {
                    if (!document.originalMeshVisibility.count(mesh))
                        document.originalMeshVisibility.emplace(mesh,
                            mesh->IsEditorVisible());
                    mesh->SetEditorVisible(mesh == document.mesh);
                }
                if (auto* sprite = dynamic_cast<Engine::Components::Sprite*>(component))
                {
                    if (!document.originalSpriteVisibility.count(sprite))
                        document.originalSpriteVisibility.emplace(sprite,
                            sprite->IsEditorVisible());
                    sprite->SetEditorVisible(false);
                }
            }
        }
}

void EditorState::StopSkeletonAnimationPreview(SceneAssetDocument& document)
{
    if (!document.previewAnimation) return;
    document.previewAnimation = false;
    document.previewAnimationManager = nullptr;
    const std::string snapshot = std::move(document.previewRestoreSnapshot);
    document.previewRestoreSnapshot.clear();
    if (!snapshot.empty())
        RestoreSceneAssetDocumentSnapshot(document, snapshot);
    if (m_activeSceneAssetDocument == &document)
    {
        if (m_primaryHierarchy)
        {
            m_primaryHierarchy->Init(document.scene.get());
            m_primaryHierarchy->SetSelectedObject(document.subject);
        }
        if (m_primaryProperties)
        {
            m_primaryProperties->Init(document.scene.get());
            m_primaryProperties->SetSelectedObject(document.subject);
        }
    }
    if (m_renderer) m_renderer->MarkDirty();
}

void EditorState::HandleSceneAssetDocumentClosures()
{
    for (auto it = m_sceneAssetDocuments.begin(); it != m_sceneAssetDocuments.end();)
    {
        SceneAssetDocument* document = it->get();
        if (!document || document->view->IsOpen()) { ++it; continue; }
        if (document->dirty || document->meshEdit.dirty)
        {
            document->view->SetOpen(true);
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                    "Save the scene document before closing its editor.");
            ++it;
            continue;
        }
        if (m_activeSceneAssetDocument == document)
            SetActiveSceneAssetDocument(nullptr);
        for (auto panel = m_panels.begin(); panel != m_panels.end(); ++panel)
            if (panel->get() == document->view)
            {
                if ((*panel)->NeedsRender() && m_viewFactory)
                    if (auto* view = dynamic_cast<View*>(panel->get()))
                        m_viewFactory->FreeSrvSlot(view->GetSrvSlotIndex());
                if (m_viewFactory) m_viewFactory->NotifyPanelRemoved(panel->get());
                m_panels.erase(panel);
                break;
            }
        it = m_sceneAssetDocuments.erase(it);
    }
}

void EditorState::ProcessPendingAssetDocumentOpens()
{
    while (!m_pendingAssetDocuments.empty())
    {
        auto [path, editorType] = std::move(m_pendingAssetDocuments.front());
        m_pendingAssetDocuments.pop_front();
        const std::string identity = AssetPathIdentity(path);
        bool alreadyOpen = false;
        for (AssetDocumentView* document : m_assetDocuments)
            if (document && AssetPathIdentity(document->GetPath()) == identity)
            {
                document->SetOpen(true);
                SetActiveAssetDocument(document);
                alreadyOpen = true;
                break;
            }
        if (alreadyOpen)
            continue;

        auto document = std::make_unique<AssetDocumentView>(path,
            editorType, m_scene.get());
        AssetDocumentView* raw = document.get();
        raw->OnFocused = [this, raw]() { SetActiveAssetDocument(raw); };
        raw->OnChanged = [this, raw]()
        {
            if (m_viewFactory && m_viewFactory->OnAssetContentsChanged)
                m_viewFactory->OnAssetContentsChanged(raw->GetPath());
            if (m_renderer)
                m_renderer->MarkDirty();
        };
        raw->OnRenamed = [this](const std::string& oldPath,
            const std::string& newPath)
        {
            if (m_primaryAssets)
                m_primaryAssets->SetSelectedPath(newPath);
            if (m_viewFactory && m_viewFactory->OnAssetRenamed)
                m_viewFactory->OnAssetRenamed(oldPath, newPath);
        };
        m_assetDocuments.push_back(raw);
        m_panels.push_back(std::move(document));
        SetActiveAssetDocument(raw);
    }
}

void EditorState::SetActiveAssetDocument(AssetDocumentView* document)
{
    SetPrefabDocumentFocused(false);
    SetActiveSceneAssetDocument(nullptr);
    m_activeAssetDocument = document;
    if (m_renderer)
        m_renderer->MarkDirty();
}

void EditorState::RefreshSceneDocumentTitle()
{
    const std::string filename = m_currentScenePath.empty()
        ? "Untitled.scene"
        : std::filesystem::path(m_currentScenePath).filename().string();
    const std::string title = filename +
        (m_hasUnsavedChanges ? " *" : "") + "###SceneDocument:" +
        (m_currentScenePath.empty()
            ? std::string("untitled") : AssetPathIdentity(m_currentScenePath));
    for (const auto& panel : m_panels)
        if (auto* sceneView = dynamic_cast<SceneView*>(panel.get());
            sceneView && sceneView->GetScene() == m_scene.get())
        {
            sceneView->SetDocumentPath(m_currentScenePath);
            sceneView->SetTitle(title);
        }
}

void EditorState::HandleAssetDocumentClosures()
{
    for (auto it = m_assetDocuments.begin(); it != m_assetDocuments.end();)
    {
        AssetDocumentView* document = *it;
        if (!document || document->IsOpen())
        {
            ++it;
            continue;
        }
        if (document->IsDirty())
        {
            document->SetOpen(true);
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                    "Save the asset before closing its editor.");
            ++it;
            continue;
        }
        if (m_activeAssetDocument == document)
            m_activeAssetDocument = nullptr;
        it = m_assetDocuments.erase(it);
    }
}

void EditorState::ClosePrefabStage()
{
    if (m_activeSceneAssetDocument && m_activeSceneAssetDocument->prefab)
    {
        m_activeSceneAssetDocument->view->SetOpen(false);
        HandleSceneAssetDocumentClosures();
        return;
    }
    if (m_activePrefabPath.empty()) return;
    if (m_prefabHasUnsavedChanges)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Save the prefab before closing its stage.");
        return;
    }

    SetPrefabDocumentFocused(false);
    RemovePrefabPanels();
    m_prefabScene.reset();
    m_prefabMeshEdit = {};
    m_activePrefabPath.clear();
    SetPrefabDirty(false);
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            "Closed prefab stage.");
}

void EditorState::HandlePrefabPanelClosures()
{
    if (m_activePrefabPath.empty())
        return;
    const bool panelClosed =
        (m_prefabSceneView && !m_prefabSceneView->IsOpen());
    if (!panelClosed)
        return;

    if (m_prefabHasUnsavedChanges)
    {
        if (m_prefabSceneView) m_prefabSceneView->SetOpen(true);
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "Save the prefab before closing its editor.");
        return;
    }
    ClosePrefabStage();
}

void EditorState::RemovePrefabPanels()
{
    auto isPrefabPanel = [this](const std::unique_ptr<IEditorPanel>& panel)
    {
        return panel.get() == m_prefabSceneView;
    };
    for (auto it = m_panels.begin(); it != m_panels.end();)
    {
        if (!isPrefabPanel(*it))
        {
            ++it;
            continue;
        }
        if ((*it)->NeedsRender() && m_viewFactory)
            if (auto* view = dynamic_cast<View*>(it->get()))
                m_viewFactory->FreeSrvSlot(view->GetSrvSlotIndex());
        if (m_viewFactory)
            m_viewFactory->NotifyPanelRemoved(it->get());
        it = m_panels.erase(it);
    }
    m_prefabSceneView = nullptr;
}

void EditorState::CapturePlayModeScene()
{
    if (!m_scene || !m_playModeSceneSnapshot.empty())
        return;

    // Finish any editor interaction before establishing the immutable play
    // baseline. Runtime changes must never enter the undo history.
    TrackSceneChanges(true, false);
    CommitPendingHistoryEdit();
    m_playModeSceneSnapshot = m_scene->SaveToString();
    m_prePlayHasUnsavedChanges = m_hasUnsavedChanges;
    m_prePlayHadObjectSelection = false;
    m_prePlaySelectionPath.clear();
    for (const auto& panel : m_panels)
    {
        const auto* hierarchy = dynamic_cast<const HierarchyView*>(panel.get());
        if (!hierarchy)
            continue;
        Engine::Core::Object* selected = hierarchy->GetSelectedObject();
        m_prePlayHadObjectSelection = selected &&
            m_scene->TryGetObjectPath(selected, m_prePlaySelectionPath);
        break;
    }
    OutputDebugStringA("[Play] Captured editor scene state.\n");
}

void EditorState::RestorePlayModeScene()
{
    if (!m_scene || m_playModeSceneSnapshot.empty())
        return;

    // Scene replacement invalidates object pointers held by editor panels.
    for (auto& panel : m_panels)
        if (auto* hierarchy = dynamic_cast<HierarchyView*>(panel.get()))
            hierarchy->SetSelectedObject(nullptr);
    if (m_primaryProperties)
        m_primaryProperties->SetSelectedObject(nullptr);

    if (m_scene->LoadFromString(m_playModeSceneSnapshot))
    {
        m_hasUnsavedChanges = m_prePlayHasUnsavedChanges;
        RefreshSceneDocumentTitle();
        SelectObject(m_prePlayHadObjectSelection
            ? m_scene->FindObjectByPath(m_prePlaySelectionPath)
            : nullptr);
        m_historyBaseline = CaptureHistoryEntry();
        m_historyCapturedRevision = m_sceneEditRevision;
        m_historySelectionDirty = false;
        m_sceneEditInProgress = false;
        if (m_renderer)
            m_renderer->MarkDirty();
        OutputDebugStringA("[Play] Restored editor scene state.\n");
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info, "[Play] Restored pre-play scene state.");
    }
    else
    {
        OutputDebugStringA("[Play] ERROR: Failed to restore editor scene state.\n");
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error, "[Play] Failed to restore pre-play scene state.");
    }

    m_playModeSceneSnapshot.clear();
    m_prePlaySelectionPath.clear();
    m_prePlayHadObjectSelection = false;
}

void EditorState::RefreshSelectionAfterReload(const ::Engine::Scene::Scene::ObjectPath& selectedPath)
{
    SelectObject(m_scene ? m_scene->FindObjectByPath(selectedPath) : nullptr);
}

// ---------------------------------------------------------------------------
// EditorState::InitializePanels
// ---------------------------------------------------------------------------
void EditorState::InitializePanels()
{
    OutputDebugStringA("[EditorState::InitializePanels] Creating preferences view\n");
    m_preferences = std::make_unique<PreferencesView>();
    
    OutputDebugStringA("[EditorState::InitializePanels] Calling preferences->Init\n");
    m_preferences->Init(m_projectSettings, m_projectFilePath);
    m_preferences->SetSpatialDebugVisuals(
        m_scene && m_scene->settings.portalDebugVisuals);
    m_preferences->OnSettingsChanged = [this]() {
        m_projectSettings = m_preferences->GetSettings();
        if (m_scene)
        {
            m_scene->SetRealtimeShadowSettings(
                m_projectSettings.realtimeShadows);
            m_scene->SetDistanceLightingSettings(
                m_projectSettings.distanceLighting);
        }
        SetHistoryLimit(m_projectSettings.editorHistoryLimit);
        for (auto& panel : m_panels)
            if (auto* hierarchy = dynamic_cast<HierarchyView*>(panel.get()))
                hierarchy->SetDebugInteractionLogging(
                    m_projectSettings.debugHierarchyInteractions);
    };
    m_preferences->OnSpatialDebugVisualsChanged = [this](bool enabled) {
        if (!m_scene)
            return;
        m_scene->settings.portalDebugVisuals = enabled;
        MarkSceneEdited();
        if (m_renderer)
            m_renderer->MarkDirty();
    };
    
    OutputDebugStringA("[EditorState::InitializePanels] Checking view factory\n");
    if (m_viewFactory)
    {
        OutputDebugStringA("[EditorState::InitializePanels] Creating Scene view\n");
        auto scenePanel = m_viewFactory->Create("Scene");
        if (scenePanel)
        {
            if (auto* mainSceneView = dynamic_cast<SceneView*>(scenePanel.get()))
            {
                mainSceneView->OnMeshViewportInput = [this](IEditorUi& ui,
                    const EditorUiViewportInput& input, EditorTransformTool tool)
                { return HandleMeshViewport(ui, input, m_scene.get(), m_mainMeshEdit, tool); };
                mainSceneView->OnDrawDocumentTools = [this](IEditorUi& ui)
                {
                    if (!ui.BeginViewportHeader("##SceneEditMode", 230.f))
                        return false;
                    int mode = m_mainMeshEdit.enabled ? 1 : 0;
                    const ViewportModeControl controls = DrawViewportModeControl(ui, mode);
                    if (controls.changed)
                        SetMeshEditMode(m_scene.get(), m_mainMeshEdit, mode == 1);
                    if (m_mainMeshEdit.enabled)
                        SyncMeshEditSelection(m_scene.get(), m_mainMeshEdit);
                    if (controls.showSave)
                    {
                        ui.SameLine();
                        if (ui.Button("Save")) SaveScene();
                    }
                    const bool consumed = ui.EndViewportHeader();
                    return consumed;
                };
            }
            scenePanel->OnFocused = [this]()
            {
                SetActiveSceneAssetDocument(nullptr);
                SetPrefabDocumentFocused(false);
            };
            m_panels.push_back(std::move(scenePanel));
        }
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Scene panel is null\n");

        OutputDebugStringA("[EditorState::InitializePanels] Creating Game view\n");
        auto gamePanel = m_viewFactory->Create("Game");
        if (gamePanel)
        {
            gamePanel->OnFocused = []() {};
            m_panels.push_back(std::move(gamePanel));
        }
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Game panel is null\n");
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Hierarchy view\n");
        auto hierarchyPanel = m_viewFactory->Create("Hierarchy");
        if (hierarchyPanel)
        {
            hierarchyPanel->OnFocused = [this]() {};
            if (auto* hierarchy = dynamic_cast<HierarchyView*>(hierarchyPanel.get()))
            {
                m_primaryHierarchy = hierarchy;
                hierarchy->OnPrefabRequested = [this](const std::string& prefabPath)
                {
                    QueueSceneAssetDocumentOpen(prefabPath);
                };
            }
            m_panels.push_back(std::move(hierarchyPanel));
        }
        else OutputDebugStringA("[EditorState::InitializePanels] WARNING: Hierarchy panel is null\n");
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Properties view\n");
        auto properties = m_viewFactory->Create("Properties");
        if (properties)
        {
            OutputDebugStringA("[EditorState::InitializePanels] Storing properties pointer\n");
            m_primaryProperties = static_cast<PropertiesView*>(properties.get());
            properties->OnFocused = [this]() {};
            m_panels.push_back(std::move(properties));
        }
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Assets view\n");
        auto assets = m_viewFactory->Create("Assets");
        if (assets)
        {
            m_primaryAssets = static_cast<AssetsExplorerView*>(assets.get());
            m_panels.push_back(std::move(assets));
        }
        
        OutputDebugStringA("[EditorState::InitializePanels] Creating Console view\n");
        auto console = m_viewFactory->Create("Console");
        if (console)
        {
            OutputDebugStringA("[EditorState::InitializePanels] Storing console pointer\n");
            m_primaryConsole = static_cast<ConsoleView*>(console.get());
            m_panels.push_back(std::move(console));
        }

        OutputDebugStringA("[EditorState::InitializePanels] Creating Problems view\n");
        auto problems = m_viewFactory->Create("Problems");
        if (problems)
            m_panels.push_back(std::move(problems));

        OutputDebugStringA("[EditorState::InitializePanels] Creating Terminal view\n");
        auto terminal = m_viewFactory->Create("Terminal");
        if (terminal)
            m_panels.push_back(std::move(terminal));
    }
    m_pendingEditToolsOpen = true;
    RefreshSceneDocumentTitle();
    OutputDebugStringA("[EditorState::InitializePanels] Complete\n");
}

// ---------------------------------------------------------------------------
// EditorState::WireupCallbacks
// ---------------------------------------------------------------------------
void EditorState::WireupCallbacks()
{
    if (!m_viewFactory)
        return;
    m_viewFactory->OnMainDocumentFocused = [this]()
    {
        // Hierarchy and Properties are shared panels. Their focus alone must
        // not switch the active asset document.
    };

    // Wire up scene loading callback
    m_viewFactory->OnSceneRequested = [this](const std::string& scenePath) {
        QueueSceneAssetDocumentOpen(scenePath);
    };
    m_viewFactory->OnPrefabRequested = [this](const std::string& prefabPath) {
        QueueSceneAssetDocumentOpen(prefabPath);
    };
    m_viewFactory->OnAssetDocumentRequested = [this](const std::string& path,
        const std::string& editorType) {
        QueueAssetDocumentOpen(path, editorType);
    };

    m_viewFactory->OnAssetSelected = [this](const std::string& assetPath) {
        if (m_primaryHierarchy)
            m_primaryHierarchy->SetSelectedObject(nullptr);
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->SetSelectedObject(nullptr);
        if (m_primaryProperties)
        {
            m_primaryProperties->SetSelectedObject(nullptr);
            m_primaryProperties->SetSelectedAsset(assetPath);
        }
        if (m_primaryAssets)
            m_primaryAssets->SetSelectedPath(assetPath);
        MarkHistorySelectionChanged();
    };

    m_viewFactory->OnAssetRenamed = [this](const std::string& oldPath,
        const std::string& newPath) {
        if (m_primaryAssets)
            m_primaryAssets->SetSelectedPath(newPath);

        const std::string oldNormalized = NormalizeAssetPath(oldPath);
        const std::string newNormalized = NormalizeAssetPath(newPath);

        const auto remapPath = [&](const std::string& value)
        {
            const std::string normalized = NormalizeAssetPath(value);
            const std::string remapped = RemapPathPrefix(normalized,
                oldNormalized, newNormalized);
            if (remapped != normalized)
                return remapped;
            return normalized == oldNormalized ? newNormalized : value;
        };

        bool sceneChanged = false;
        if (m_scene)
        {
            std::function<void(Engine::Core::Object*)> updatePrefab =
                [&](Engine::Core::Object* object) {
                if (!object)
                    return;
                if (object->Prefab)
                {
                    const std::string remapped =
                        remapPath(object->Prefab->GetPath());
                    if (remapped != object->Prefab->GetPath())
                    {
                        object->SetPrefab(remapped);
                        sceneChanged = true;
                    }
                }
                for (Engine::Core::Object* child : object->Children)
                    updatePrefab(child);
            };
            for (const auto& object : m_scene->GetObjects())
                if (object && !object->Parent)
                    updatePrefab(object.get());
        }

        if (!m_currentScenePath.empty())
            m_currentScenePath = remapPath(m_currentScenePath);
        RefreshSceneDocumentTitle();
        if (!m_activePrefabPath.empty())
            m_activePrefabPath = remapPath(m_activePrefabPath);

        bool assetReferenceChanged = false;
        std::error_code error;
        std::filesystem::path assetsDirectory =
            m_projectSettings.assetsDirectory.empty()
            ? std::filesystem::path("Assets")
            : std::filesystem::path(m_projectSettings.assetsDirectory);
        assetsDirectory = std::filesystem::weakly_canonical(assetsDirectory,
            error);
        if (!error && std::filesystem::exists(assetsDirectory))
        {
            const std::vector<std::string> oldCandidates = {
                oldNormalized,
                std::filesystem::path(oldNormalized).make_preferred().string()
            };
            const std::vector<std::string> newCandidates = {
                newNormalized,
                std::filesystem::path(newNormalized).make_preferred().string()
            };

            for (const auto& entry :
                std::filesystem::recursive_directory_iterator(assetsDirectory,
                    error))
            {
                if (error)
                    break;
                if (!entry.is_regular_file())
                    continue;
                const std::string extension =
                    entry.path().extension().string();
                if (extension == ".meta")
                    continue;
                const std::string lowerExtension = [&]() {
                    std::string lower = extension;
                    std::transform(lower.begin(), lower.end(),
                        lower.begin(), [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        });
                    return lower;
                }();
                if (lowerExtension != ".scene" &&
                    lowerExtension != ".prefab" &&
                    lowerExtension != ".xml")
                    continue;

                std::ifstream input(entry.path(), std::ios::binary);
                if (!input)
                    continue;
                std::string contents((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
                input.close();

                bool changed = false;
                for (size_t index = 0; index < oldCandidates.size(); ++index)
                    changed = ReplaceAllInText(contents, oldCandidates[index],
                        newCandidates[index]) || changed;
                if (!changed)
                    continue;

                std::ofstream output(entry.path(),
                    std::ios::binary | std::ios::trunc);
                if (!output)
                    continue;
                output << contents;
                if (output.good())
                    assetReferenceChanged = true;
            }
        }

        if (sceneChanged || assetReferenceChanged)
        {
            m_hasUnsavedChanges = true;
            MarkSceneEdited();
        }
    };

    m_viewFactory->OnAssetContentsChanged = [this](const std::string&) {
        auto refresh = [this](Engine::Scene::Scene* scene)
        {
            if (!scene) return;
            const std::string snapshot = scene->SaveToString();
            ::Engine::Scene::Scene::ObjectPath selectedPath;
            const bool hadSelection = scene->GetSelectedObject() &&
                scene->TryGetObjectPath(scene->GetSelectedObject(), selectedPath);
            if (!scene->LoadFromString(snapshot) && m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Error,
                    "Could not refresh a scene after asset contents changed.");
            if (hadSelection)
                if (Engine::Core::Object* selected = scene->FindObjectByPath(selectedPath))
                    scene->SetSelectedObject(selected);
            if (scene == GetActiveDocumentScene())
            {
                Engine::Core::Object* selected = scene->GetSelectedObject();
                if (m_primaryHierarchy)
                    m_primaryHierarchy->SetSelectedObject(selected);
                if (m_primaryProperties)
                    m_primaryProperties->SetSelectedObject(selected);
            }
        };
        refresh(m_scene.get());
        refresh(m_prefabScene.get());
        for (const auto& document : m_sceneAssetDocuments)
            if (document && !document->meshStage && !document->skeletonStage)
                refresh(document->scene.get());
    };

    // Wire up selection changed callback (for hierarchy -> properties)
    m_viewFactory->OnSelectionChanged = [this](Engine::Core::Object* obj) {
        OutputDebugStringA(("[EditorState] Selection changed to: " + (obj ? obj->name : "nullptr") + "\n").c_str());
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->SetSelectedObject(obj);
        if (m_primaryAssets)
            m_primaryAssets->SetSelectedPath({});
        if (m_primaryProperties)
        {
            m_primaryProperties->SetSelectedObject(obj);
            OutputDebugStringA("[EditorState] Updated properties view\n");
        }
        else
        {
            OutputDebugStringA("[EditorState] WARNING: Properties view is null\n");
        }
        MarkHistorySelectionChanged();
    };
    m_viewFactory->OnSelectionSetChanged = [this](
        const std::vector<Engine::Core::Object*>& objects) {
        Engine::Core::Object* active = objects.empty() ? nullptr : objects.back();
        OutputDebugStringA(("[EditorState] Selection set changed: " +
            std::to_string(objects.size()) + " object(s)\n").c_str());
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->SetSelectedObject(active);
        if (m_primaryAssets)
            m_primaryAssets->SetSelectedPath({});
        if (m_primaryProperties)
            m_primaryProperties->SetSelectedObjects(objects);
        MarkHistorySelectionChanged();
    };

    // Scene viewport click-selection -> hierarchy/properties + render selection state
    m_viewFactory->OnObjectSelected = [this](Engine::Core::Object* obj) {
        if (m_primaryHierarchy)
            m_primaryHierarchy->SetSelectedObject(obj);
        if (m_primaryProperties)
            m_primaryProperties->SetSelectedObject(obj);
        if (m_primaryAssets)
            m_primaryAssets->SetSelectedPath({});
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->SetSelectedObject(obj);
        MarkHistorySelectionChanged();
    };
    m_viewFactory->OnObjectCreated = [this](Engine::Core::Object* obj) {
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
        SelectObject(obj);
    };
    m_viewFactory->OnDeleteSelectionRequested = [this]() {
        if (m_prefabDocumentFocused)
        {
            if (m_primaryHierarchy)
                m_primaryHierarchy->RequestDeleteSelectedObject();
            return;
        }
        if (m_primaryHierarchy)
            m_primaryHierarchy->RequestDeleteSelectedObject();
    };
    m_viewFactory->OnGizmoInteraction = [this](bool active) {
        ReportSceneEditInProgress(active);
        // Imported skeleton joints usually live below a linked-prefab root.
        // Invalidating that root on every mouse move makes the Properties view
        // serialize and diff the complete model hierarchy once per frame.  The
        // final transform is all the override cache needs, so defer its single
        // invalidation until the gizmo interaction ends.
        if (active)
        {
            m_mainSceneGizmoWasActive = true;
        }
        else if (m_mainSceneGizmoWasActive)
        {
            m_mainSceneGizmoWasActive = false;
            if (m_scene)
                if (Engine::Core::Object* selected = m_scene->GetSelectedObject();
                    selected && selected != selected->GetPrefabInstanceRoot())
                    selected->InvalidatePrefabOverrideCache();
        }
    };

    // Wire up focus (double-click) callback — frame the object in the scene camera
    m_viewFactory->OnFocusObject = [this](Engine::Core::Object* obj) {
        if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
            scene->FocusEditorCamera(obj);
    };
    m_viewFactory->OnHierarchyChanged = [this]() {
        if (m_activeSceneAssetDocument)
        {
            SceneAssetDocument& document = *m_activeSceneAssetDocument;
            if (document.objectStage || document.skeletonStage)
            {
                std::unordered_set<Engine::Core::Object*> liveObjects;
                for (const auto& object : document.scene->GetObjects())
                    if (object)
                    {
                        liveObjects.insert(object.get());
                        const bool inFocusGraph = document.objectStage && [&]()
                        {
                            for (Engine::Core::Object* current = object.get();
                                current; current = current->Parent)
                                if (current == document.subject) return true;
                            return false;
                        }();
                        const bool editableSubject =
                            document.selectableObjects.find(object.get()) !=
                                document.selectableObjects.end() || inFocusGraph;
                        if (!editableSubject) continue;
                        auto state = document.originalEnabledState.find(object.get());
                        if (state == document.originalEnabledState.end())
                            document.originalEnabledState.emplace(
                                object.get(), object->enabled);
                        else if (document.skeletonStage ||
                            (document.objectStage && document.showChildHierarchy) ||
                            object.get() == document.subject)
                            state->second = object->enabled;
                    }
                for (auto state = document.originalEnabledState.begin();
                    state != document.originalEnabledState.end();)
                    if (liveObjects.find(state->first) == liveObjects.end())
                        state = document.originalEnabledState.erase(state);
                    else
                        ++state;
                if (document.objectStage)
                    ApplyObjectStageVisibility(document);
            }
            m_renderer->MarkDirty();
        }
        else if (m_prefabDocumentFocused)
            SetPrefabDirty(true);
        else
        {
            m_hasUnsavedChanges = true;
            MarkSceneEdited();
        }
    };
    m_viewFactory->OnPropertiesChanged = [this]() {
        if (m_activeSceneAssetDocument)
            m_renderer->MarkDirty();
        else if (m_prefabDocumentFocused)
            SetPrefabDirty(true);
        else
        {
            m_hasUnsavedChanges = true;
            MarkSceneEdited();
        }
    };
    m_viewFactory->OnPropertiesAssetDropLog = [this](const std::string& message, bool error) {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(error ? ConsoleView::Level::Error : ConsoleView::Level::Info,
                message);
    };
    m_viewFactory->OnHierarchyInteraction = [this](const std::string& message) {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info, message);
    };

    m_viewFactory->OnAssetDropped = [this](const std::string& path) {
        SelectObject(InstantiateAsset(path));
    };
    m_viewFactory->OnAssetPreviewRequested = [this](const std::string& path) {
        m_assetPreviewActive = true;
        Engine::Core::Object* preview = InstantiateAsset(path, false);
        m_assetPreviewActive = preview != nullptr;
        return preview;
    };
    m_viewFactory->OnAssetPreviewCancelled = [this](Engine::Core::Object* object) {
        if (m_scene && object)
            m_scene->RemoveObject(object);
        m_assetPreviewActive = false;
    };
    m_viewFactory->OnAssetPreviewCommitted = [this](Engine::Core::Object* object,
        const std::string& path) {
        m_assetPreviewActive = false;
        if (!object)
            return;
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Placed prefab in scene: " + path);
        SelectObject(object);
    };

    m_viewFactory->OnPrefabCreated = [this](Engine::Core::Object*, const std::string& path) {
        m_hasUnsavedChanges = true;
        MarkSceneEdited();
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info, "Prefab created: " + path);
    };

    if (m_window)
        m_window->OnFilesDropped = [this](const std::vector<std::string>& paths) {
            Engine::Core::Object* lastObject = nullptr;
            for (const std::string& path : paths)
            {
                const std::string importedPath = ImportAssetFile(path);
                if (Engine::Core::Object* object = importedPath.empty()
                    ? nullptr : InstantiateAsset(importedPath))
                    lastObject = object;
            }
            if (lastObject)
                SelectObject(lastObject);
        };
}

void EditorState::ImportAsset()
{
    wchar_t source[MAX_PATH] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = m_window ? m_window->GetHWND() : nullptr;
    dialog.lpstrFile = source;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter =
        L"Supported Assets (*.obj;*.gltf;*.glb;*.fbx;*.prefab;*.spriteanim;*.spritesheet;*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2;*.wav;*.ogg;*.mp3;*.ttf;*.otf)\0"
        L"*.obj;*.gltf;*.glb;*.fbx;*.prefab;*.spriteanim;*.spritesheet;*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2;*.wav;*.ogg;*.mp3;*.ttf;*.otf\0"
        L"3D Models (*.obj;*.gltf;*.glb;*.fbx)\0*.obj;*.gltf;*.glb;*.fbx\0"
        L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2)\0*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga;*.hdr;*.exr;*.ktx2\0"
        L"Audio (*.wav;*.ogg;*.mp3)\0*.wav;*.ogg;*.mp3\0"
        L"Fonts (*.ttf;*.otf)\0*.ttf;*.otf\0"
        L"All Files (*.*)\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog))
        ImportAssetFile(std::filesystem::path(source).string());
}

std::string EditorState::ImportAssetFile(const std::string& path)
{
    namespace fs = std::filesystem;
    const fs::path source(path);
    std::string extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const fs::path assetsDirectory = m_projectSettings.assetsDirectory.empty()
        ? fs::path("Assets") : fs::path(m_projectSettings.assetsDirectory);

    try
    {
        fs::create_directories(assetsDirectory);
        if (ModelImporter::SupportsExtension(extension))
        {
            const Engine::Model::ModelImportResult imported =
                ModelImporter::Import(source.string(), assetsDirectory.string());
            if (!imported.success)
                throw std::runtime_error(imported.message);
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Info,
                    "Asset imported: " + imported.prefabPath);
            return imported.prefabPath;
        }

        std::error_code relativeError;
        const fs::path absoluteSource = fs::weakly_canonical(source, relativeError);
        const fs::path absoluteAssets = fs::weakly_canonical(assetsDirectory, relativeError);
        if (!relativeError)
        {
            const fs::path relative = absoluteSource.lexically_relative(absoluteAssets);
            if (!relative.empty() && *relative.begin() != "..")
            {
                Engine::Core::AssetRecord::Ensure(source, source,
                    { { "importer", std::string("native") } });
                return source.string();
            }
        }

        fs::path destination = assetsDirectory / source.filename();
        const std::string stem = destination.stem().string();
        const std::string suffix = destination.extension().string();
        for (unsigned index = 2; fs::exists(destination); ++index)
            destination = assetsDirectory /
                (stem + " " + std::to_string(index) + suffix);
        fs::copy_file(source, destination);
        Engine::Core::AssetRecord::Ensure(destination, source,
            { { "importer", std::string("copy") } });
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Asset imported: " + destination.string());
        return destination.string();
    }
    catch (const std::exception& error)
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Asset import failed: " + std::string(error.what()));
        return {};
    }
}

Engine::Core::Object* EditorState::InstantiateAsset(const std::string& path, bool recordChange)
{
    Engine::Scene::Scene* targetScene = GetActiveDocumentScene();
    if (!targetScene)
        return nullptr;

    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    Engine::Core::Object* object = nullptr;
    try
    {
        if (ModelImporter::SupportsExtension(extension))
        {
            const std::string assetsDirectory = m_projectSettings.assetsDirectory.empty()
                ? std::string("Assets")
                : m_projectSettings.assetsDirectory;
            const Engine::Model::ModelImportResult imported = ModelImporter::Import(path, assetsDirectory);
            if (!imported.success)
                throw std::runtime_error(imported.message);
            object = Engine::Serialization::SceneSerializer::InstantiatePrefab(
                *targetScene, imported.prefabPath, targetScene->GetGraphicsProvider());
        }
        else if (extension == ".prefab")
        {
            object = Engine::Serialization::SceneSerializer::InstantiatePrefab(
                *targetScene, path, targetScene->GetGraphicsProvider());
        }
        else if (extension == ".obj")
        {
            object = targetScene->AddObject(std::filesystem::path(path).stem().string());
            Engine::Components::Mesh* mesh = object->AddComponent<Engine::Components::Mesh>();
            mesh->LoadFromFile(path);
            if (targetScene->GetGraphicsProvider())
                mesh->CreateBuffer(targetScene->GetGraphicsProvider()->GetBufferFactory());
            object->AddComponent<Engine::Components::Material>();
        }
        else if (extension == ".spriteanim")
        {
            object = targetScene->AddObject(std::filesystem::path(path).stem().string());
            Engine::Components::SpriteAnimationManager* manager = object->AddComponent<Engine::Components::SpriteAnimationManager>();
            Engine::Components::Sprite* sprite = object->AddComponent<Engine::Components::Sprite>();
            sprite->SetAnimationManager(manager);
            if (!manager->LoadFromFile(path) ||
                !sprite->Prepare(targetScene->GetGraphicsProvider()))
                throw std::runtime_error("Could not load sprite animation");
        }
        else if (extension == ".scene" || extension == ".xml")
        {
            QueueSceneAssetDocumentOpen(path);
            return nullptr;
        }
        else
        {
            if (m_primaryConsole)
                m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                    "Unsupported scene drop: " + path);
            return nullptr;
        }
    }
    catch (const std::exception& error)
    {
        if (object)
            targetScene->RemoveObject(object);
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                "Could not instantiate asset '" + path + "': " + error.what());
        return nullptr;
    }

    if (object && recordChange)
    {
        if (m_activeSceneAssetDocument)
        {
            if (m_renderer) m_renderer->MarkDirty();
        }
        else if (m_prefabDocumentFocused)
            SetPrefabDirty(true);
        else
        {
            m_hasUnsavedChanges = true;
            MarkSceneEdited();
        }
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Info,
                "Added to scene: " + path);
    }
    return object;
}

Engine::Scene::Scene* EditorState::GetActiveDocumentScene() const
{
    if (m_activeSceneAssetDocument && m_activeSceneAssetDocument->scene)
        return m_activeSceneAssetDocument->scene.get();
    if (m_prefabDocumentFocused && m_prefabScene)
        return m_prefabScene.get();
    return m_scene.get();
}

void EditorState::SetPrefabDocumentFocused(bool focused)
{
    const bool nextFocused = focused && m_prefabScene && m_prefabSceneView;
    const bool contextChanged = m_prefabDocumentFocused != nextFocused ||
        m_activeAssetDocument != nullptr ||
        (nextFocused && m_activeSceneAssetDocument != nullptr);
    m_activeAssetDocument = nullptr;
    if (nextFocused)
        m_activeSceneAssetDocument = nullptr;
    m_prefabDocumentFocused = nextFocused;
    if (!contextChanged)
        return;
    Engine::Scene::Scene* activeScene = GetActiveDocumentScene();
    Engine::Core::Object* selected = activeScene
        ? activeScene->GetSelectedObject() : nullptr;

    if (m_primaryHierarchy)
    {
        m_primaryHierarchy->Init(activeScene);
        m_primaryHierarchy->SetSelectedObject(selected);
    }
    if (m_primaryProperties)
    {
        m_primaryProperties->Init(activeScene);
        m_primaryProperties->SetSelectedObject(selected);
        m_primaryProperties->SetSelectedAsset("");
    }
    if (m_renderer)
        m_renderer->MarkDirty();
}

void EditorState::SetPrefabDirty(bool dirty)
{
    if (m_prefabHasUnsavedChanges == dirty)
        return;
    m_prefabHasUnsavedChanges = dirty;
    RefreshPrefabDocumentTitle();
}

void EditorState::RefreshPrefabDocumentTitle()
{
    if (!m_prefabSceneView || m_activePrefabPath.empty())
        return;
    std::string title = std::filesystem::path(
        m_activePrefabPath).filename().string();
    if (m_prefabHasUnsavedChanges)
        title += " *";
    m_prefabSceneView->SetTitle(title + "###PrefabDocument:" +
        AssetPathIdentity(m_activePrefabPath));
}

void EditorState::SelectObject(Engine::Core::Object* object)
{
    if (m_primaryAssets)
        m_primaryAssets->SetSelectedPath({});
    if (m_primaryHierarchy)
        m_primaryHierarchy->SetSelectedObject(object);
    if (m_primaryProperties)
        m_primaryProperties->SetSelectedObject(object);
    if (Engine::Scene::Scene* scene = GetActiveDocumentScene())
        scene->SetSelectedObject(object);
    MarkHistorySelectionChanged();
}

EditorState::HistoryEntry EditorState::CaptureHistoryEntry() const
{
    HistoryEntry entry;
    if (!m_scene)
        return entry;
    entry.scene = m_scene->SaveToString();
    CaptureHistorySelection(entry);
    return entry;
}

void EditorState::CaptureHistorySelection(HistoryEntry& entry) const
{
    entry.hasSelection = false;
    entry.selectionPath.clear();
    if (!m_scene || m_prefabDocumentFocused || !m_primaryHierarchy)
        return;
    Engine::Core::Object* selected = m_primaryHierarchy->GetSelectedObject();
    entry.hasSelection = selected &&
        m_scene->TryGetObjectPath(selected, entry.selectionPath);
}

void EditorState::MarkSceneEdited()
{
    ++m_sceneEditRevision;
    if (m_renderer)
        m_renderer->MarkDirty();
}

void EditorState::TrackSceneChanges(bool allowHistory, bool editInProgress)
{
    if (m_activeSceneAssetDocument && m_activeSceneAssetDocument->scene)
    {
        if (m_activeSceneAssetDocument->previewAnimation)
            return;
        if (!allowHistory || editInProgress)
            return;
        SceneAssetDocument& document = *m_activeSceneAssetDocument;
        std::string current = CaptureSceneAssetDocumentSnapshot(document);
        if (current != document.baseline)
        {
            if (m_historyLimit > 0 && document.undo.size() >= m_historyLimit)
                document.undo.pop_front();
            if (m_historyLimit > 0)
                document.undo.push_back(document.baseline);
            document.redo.clear();
            document.baseline = std::move(current);
            document.dirty = document.baseline != document.savedSnapshot;
            RefreshSceneAssetDocumentTitle(document);
            if (m_renderer) m_renderer->MarkDirty();
        }
        return;
    }
    if (!m_scene || !allowHistory || m_assetPreviewActive)
        return;

    if (m_historySelectionDirty && !m_historyBaseline.scene.empty())
    {
        CaptureHistorySelection(m_historyBaseline);
        m_historySelectionDirty = false;
    }

    // Property drags and gizmos can update every rendered frame. Capturing the
    // complete scene here made large linked prefabs serialize and diff for
    // every intermediate mouse position. The existing baseline is already the
    // correct undo "before" state, so wait until the interaction ends and
    // capture its final value once.
    if (editInProgress)
        return;

    // Editor mutation callbacks advance the revision. If it has not changed,
    // the serialized scene must still match the existing history baseline, so
    // avoid rebuilding and comparing the complete scene on this idle frame.
    if (!m_historyBaseline.scene.empty() &&
        m_historyCapturedRevision == m_sceneEditRevision)
        return;

    HistoryEntry current = CaptureHistoryEntry();
    m_historyCapturedRevision = m_sceneEditRevision;
    m_historySelectionDirty = false;
    if (m_historyBaseline.scene.empty())
    {
        m_historyBaseline = std::move(current);
        if (m_savedSceneSnapshot.empty())
            m_savedSceneSnapshot = m_historyBaseline.scene;
        return;
    }

    if (current.scene != m_historyBaseline.scene)
    {
        if (m_historyLimit > 0 && !m_hasPendingHistoryEdit)
        {
            m_pendingHistoryBefore = m_historyBaseline;
            m_hasPendingHistoryEdit = true;
            m_redoHistory.clear();
        }
        m_historyBaseline = std::move(current);
        m_hasUnsavedChanges = m_historyBaseline.scene != m_savedSceneSnapshot;
        RefreshSceneDocumentTitle();
        if (m_hasPendingHistoryEdit && !editInProgress)
            CommitPendingHistoryEdit();
    }
    else
    {
        // Selection is editor state too, but changing it alone is not an undo action.
        m_historyBaseline.hasSelection = current.hasSelection;
        m_historyBaseline.selectionPath = std::move(current.selectionPath);
        if (m_hasPendingHistoryEdit && !editInProgress)
            CommitPendingHistoryEdit();
    }
}

void EditorState::CommitPendingHistoryEdit()
{
    if (!m_hasPendingHistoryEdit)
        return;
    if (m_historyLimit > 0 &&
        m_pendingHistoryBefore.scene != m_historyBaseline.scene)
        m_undoHistory.push_back(std::move(m_pendingHistoryBefore));
    m_pendingHistoryBefore = {};
    m_hasPendingHistoryEdit = false;
    TrimHistory();
}

void EditorState::Undo()
{
    if (MeshEditSession* edit = ActiveMeshEditSession();
        edit && edit->enabled && ApplyMeshHistory(false)) return;
    if (m_activeSceneAssetDocument)
    {
        SceneAssetDocument& document = *m_activeSceneAssetDocument;
        if (document.undo.empty()) return;
        document.redo.push_back(document.baseline);
        document.baseline = std::move(document.undo.back());
        document.undo.pop_back();
        RestoreSceneAssetDocumentSnapshot(document, document.baseline);
        document.dirty = document.baseline != document.savedSnapshot;
        RefreshSceneAssetDocumentTitle(document);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(nullptr);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(nullptr);
        if (m_renderer) m_renderer->MarkDirty();
        return;
    }
    if (m_activeAssetDocument)
    {
        if (!m_activeAssetDocument->Undo() && m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "No asset edit to undo.");
        return;
    }
    TrackSceneChanges(true, false);
    CommitPendingHistoryEdit();
    if (!m_scene || m_undoHistory.empty())
        return;
    HistoryEntry target = std::move(m_undoHistory.back());
    m_undoHistory.pop_back();
    // TrackSceneChanges has already made the baseline an exact snapshot of
    // the current scene. Re-serializing it here made every undo pay for a
    // second complete walk of large prefab/model hierarchies.
    m_redoHistory.push_back(std::move(m_historyBaseline));
    TrimHistory();
    ApplyHistoryEntry(std::move(target), "Undo");
}

void EditorState::Redo()
{
    if (MeshEditSession* edit = ActiveMeshEditSession();
        edit && edit->enabled && ApplyMeshHistory(true)) return;
    if (m_activeSceneAssetDocument)
    {
        SceneAssetDocument& document = *m_activeSceneAssetDocument;
        if (document.redo.empty()) return;
        if (m_historyLimit > 0)
            document.undo.push_back(document.baseline);
        document.baseline = std::move(document.redo.back());
        document.redo.pop_back();
        RestoreSceneAssetDocumentSnapshot(document, document.baseline);
        document.dirty = document.baseline != document.savedSnapshot;
        RefreshSceneAssetDocumentTitle(document);
        if (m_primaryHierarchy) m_primaryHierarchy->SetSelectedObject(nullptr);
        if (m_primaryProperties) m_primaryProperties->SetSelectedObject(nullptr);
        if (m_renderer) m_renderer->MarkDirty();
        return;
    }
    if (m_activeAssetDocument)
    {
        if (!m_activeAssetDocument->Redo() && m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Warning,
                "No asset edit to redo.");
        return;
    }
    TrackSceneChanges(true, false);
    CommitPendingHistoryEdit();
    if (!m_scene || m_redoHistory.empty())
        return;
    HistoryEntry target = std::move(m_redoHistory.back());
    m_redoHistory.pop_back();
    m_undoHistory.push_back(std::move(m_historyBaseline));
    TrimHistory();
    ApplyHistoryEntry(std::move(target), "Redo");
}

bool EditorState::CanUndo() const
{
    const MeshEditSession* edit = m_activeSceneAssetDocument
        ? &m_activeSceneAssetDocument->meshEdit
        : (m_prefabDocumentFocused && m_prefabScene
            ? &m_prefabMeshEdit : &m_mainMeshEdit);
    if (edit->enabled && !edit->undo.empty()) return true;
    return m_activeSceneAssetDocument ? !m_activeSceneAssetDocument->undo.empty()
        : m_activeAssetDocument ? m_activeAssetDocument->CanUndo()
        : (m_hasPendingHistoryEdit || !m_undoHistory.empty());
}

bool EditorState::CanRedo() const
{
    const MeshEditSession* edit = m_activeSceneAssetDocument
        ? &m_activeSceneAssetDocument->meshEdit
        : (m_prefabDocumentFocused && m_prefabScene
            ? &m_prefabMeshEdit : &m_mainMeshEdit);
    if (edit->enabled && !edit->redo.empty()) return true;
    return m_activeSceneAssetDocument ? !m_activeSceneAssetDocument->redo.empty()
        : m_activeAssetDocument ? m_activeAssetDocument->CanRedo()
        : !m_redoHistory.empty();
}

void EditorState::ApplyHistoryEntry(
    HistoryEntry entry, const char* operation)
{
    SelectObject(nullptr);
    if (!m_scene->LoadFromString(entry.scene))
    {
        if (m_primaryConsole)
            m_primaryConsole->AddLog(ConsoleView::Level::Error,
                std::string(operation) + " failed to restore the scene.");
        return;
    }

    SelectObject(entry.hasSelection
        ? m_scene->FindObjectByPath(entry.selectionPath) : nullptr);
    // The restored source is itself the canonical history snapshot. Keeping
    // it avoids serializing the newly rebuilt scene for a second time.
    m_historyBaseline = std::move(entry);
    m_historyCapturedRevision = m_sceneEditRevision;
    m_historySelectionDirty = false;
    m_hasUnsavedChanges = m_historyBaseline.scene != m_savedSceneSnapshot;
    RefreshSceneDocumentTitle();
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            std::string(operation) + " completed.");
}

void EditorState::ResetHistory(bool sceneIsSaved)
{
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_pendingHistoryBefore = {};
    m_hasPendingHistoryEdit = false;
    m_historyBaseline = CaptureHistoryEntry();
    m_historyCapturedRevision = m_sceneEditRevision;
    m_historySelectionDirty = false;
    if (sceneIsSaved)
        m_savedSceneSnapshot = m_historyBaseline.scene;
}

void EditorState::SetHistoryLimit(uint32_t limit)
{
    m_historyLimit = std::min(limit, 1000u);
    for (auto& document : m_sceneAssetDocuments)
        if (document)
        {
            while (document->undo.size() > m_historyLimit)
                document->undo.pop_front();
            while (document->redo.size() > m_historyLimit)
                document->redo.pop_front();
        }
    if (m_historyLimit == 0)
    {
        m_undoHistory.clear();
        m_redoHistory.clear();
        m_pendingHistoryBefore = {};
        m_hasPendingHistoryEdit = false;
    }
    else
        TrimHistory();
}

void EditorState::TrimHistory()
{
    while (m_undoHistory.size() > m_historyLimit)
        m_undoHistory.pop_front();
    while (m_redoHistory.size() > m_historyLimit)
        m_redoHistory.pop_front();
}

// ---------------------------------------------------------------------------
// EditorState::GetDeltaTime
// ---------------------------------------------------------------------------
float EditorState::GetDeltaTime() const
{
    return m_deltaTime;
}

// ---------------------------------------------------------------------------
// EditorState::UpdateDeltaTime
// ---------------------------------------------------------------------------
void EditorState::UpdateDeltaTime()
{
    LARGE_INTEGER currentCounter;
    QueryPerformanceCounter(&currentCounter);
    
    // Calculate elapsed time in seconds
    LONGLONG elapsedCounts = currentCounter.QuadPart - m_lastCounter.QuadPart;
    m_deltaTime = static_cast<float>(elapsedCounts) / static_cast<float>(m_perfFreq.QuadPart);
    
    // Update last counter for next frame
    m_lastCounter = currentCounter;
}
}
