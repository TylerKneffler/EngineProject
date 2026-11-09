#include "pch.h"
#include "EditorState.h"
#include "Core/SceneManager.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/IEditorPanel.h"
#include "Core/View/View.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/View/Views/SceneView.h"
#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Camera/Camera.h"
#include "Core/Compoonents/Materials/Material.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Transform.h"
#include "Core/Graphics/IGraphicsProvider.h"
#include "Core/MeshEditGeometry.h"
#include "Core/MeshEditSnapshot.h"
#include "Core/SnapshotHistory.h"
#include "Engine/Editor/Core/Paint/PaintInput.h"
#include "Engine/Editor/Core/Paint/ImagePaint.h"
#include "Engine/Editor/Core/Paint/BrushTipMask.h"
#include "Engine/Editor/Core/Paint/MeshTexturePaint.h"
#include "Engine/Editor/Core/Paint/PngPaintWriter.h"
#include <glm/gtc/matrix_inverse.hpp>

#include "EditorStateSupport.h"

namespace Engine::Editor
{
using namespace StateSupport;

namespace
{
    std::shared_ptr<Engine::Components::Texture> PaintTextureForSlot(
        const Engine::Components::Material& material, int slot,
        int& uvSet)
    {
        switch (slot)
        {
        case 0: uvSet = material.baseColorUvSet;
            return material.baseColorTexture;
        case 1: uvSet = material.metallicRoughnessUvSet;
            return material.metallicRoughnessTexture;
        case 2: uvSet = material.normalUvSet;
            return material.normalTexture;
        case 3: uvSet = material.occlusionUvSet;
            return material.occlusionTexture;
        case 4: uvSet = material.emissiveUvSet;
            return material.emissiveTexture;
        case 5: uvSet = material.heightUvSet;
            return material.heightTexture;
        default: uvSet = 0; return nullptr;
        }
    }

}

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
        FinishVertexPaintStroke(session);
        FinishTexturePaintStroke(session);
        if ((session.dirty || !session.textureUndo.empty()) &&
            !session.savePath.empty())
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
        session.texturePaintPath.clear();
        session.texturePaintPixels.clear();
        session.textureUndo.clear();
        session.textureRedo.clear();
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
    if (!enabled)
    {
        FinishVertexPaintStroke(session);
        FinishTexturePaintStroke(session);
    }
    session.enabled = enabled;
    if (scene)
    {
        scene->SetEditorSelectedMesh(nullptr);
        scene->SetEditorMeshEditPose(enabled);
    }
    for (const auto& panel : m_panels)
        if (auto* view = dynamic_cast<SceneView*>(panel.get());
            view && view->GetScene() == scene)
        {
            view->AllowObjectTransform = !enabled;
            view->SetEditMode(enabled ? SceneEditMode::Mesh :
                SceneEditMode::Object);
        }
    if (enabled) SyncMeshEditSelection(scene, session);
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

void EditorState::FinishVertexPaintStroke(MeshEditSession& session)
{
    if (!session.vertexPainting) return;
    session.vertexPainting = false;
    if (!session.vertexStrokeChanged || !session.activeMesh) return;
    if (m_historyLimit > 0 && session.undo.size() >= m_historyLimit)
        session.undo.pop_front();
    if (m_historyLimit > 0)
        session.undo.push_back(std::move(session.vertexStrokeBefore));
    session.redo.clear();
    session.baseline = CaptureMeshSnapshot(*session.activeMesh);
    session.dirty = HistoryDiffers(session.baseline, session.savedSnapshot);
    if (!session.savePath.empty())
    {
        MeshEditSession cached = session;
        cached.activeMesh = nullptr;
        m_meshEditCache[AssetPathIdentity(session.savePath)] =
            std::move(cached);
    }
    session.vertexStrokeChanged = false;
}

void EditorState::FinishTexturePaintStroke(MeshEditSession& session)
{
    if (!session.texturePainting) return;
    session.texturePainting = false;
    if (!session.textureStrokeChanged) return;
    std::string error;
    if (!Paint::SavePng(session.texturePaintPath,
            session.texturePaintWidth, session.texturePaintHeight,
            session.texturePaintPixels, error))
    {
        session.toolError = error;
        session.texturePaintPixels = session.textureStrokeBefore;
    }
    else
    {
        Paint::CommitTexturePaintHistory(session.textureUndo,
            session.textureRedo, std::move(session.textureStrokeBefore),
            m_historyLimit);
        session.toolError.clear();
    }
    Engine::Components::Texture::Invalidate(session.texturePaintPath);
    session.textureStrokeChanged = false;
    if (m_renderer) m_renderer->MarkDirty();
}

bool EditorState::StepTexturePaintHistory(MeshEditSession& session, bool redo)
{
    FinishTexturePaintStroke(session);
    auto& source = redo ? session.textureRedo : session.textureUndo;
    if (source.empty() || session.texturePaintPath.empty()) return false;
    std::vector<uint8_t> next = source.back();
    std::string error;
    if (!Paint::SavePng(session.texturePaintPath,
            session.texturePaintWidth, session.texturePaintHeight,
            next, error))
    { session.toolError = error; return false; }
    Paint::StepTexturePaintPixels(session.texturePaintPixels,
        session.textureUndo, session.textureRedo, redo);
    Engine::Components::Texture::Invalidate(session.texturePaintPath);
    if (m_renderer) m_renderer->MarkDirty();
    return true;
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
    if ((!session.vertexPaintMode || session.vertexPaintTool == 0 ||
            session.paintTarget != 0) && session.vertexPainting)
        FinishVertexPaintStroke(session);
    if ((!session.vertexPaintMode || session.vertexPaintTool == 0 ||
            session.paintTarget != 1) && session.texturePainting)
        FinishTexturePaintStroke(session);
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
    struct Point { EditorUiVec2 screen{}; float depth = 0.f;
        float inverseW = 0.f; bool valid = false; };
    std::vector<Point> points(vertices.size());
    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto& p = vertices[i].pos;
        glm::vec4 clip = transform * glm::vec4(p[0], p[1], p[2], 1.f);
        if (clip.w <= .0001f) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.z < 0.f || ndc.z > 1.f) continue;
        points[i] = { { (ndc.x * .5f + .5f) * input.available.x,
            (.5f - ndc.y * .5f) * input.available.y }, ndc.z,
            1.f / clip.w, true };
    }
    const auto mirroredPaintCursor = [&](float x, float y)
        -> std::optional<EditorUiVec2>
    {
        float nearest = 2.f;
        std::optional<glm::vec3> local;
        for (uint32_t face = 0; face < faceCount; ++face)
        {
            const uint32_t ia = indices[face * 3], ib = indices[face * 3 + 1],
                ic = indices[face * 3 + 2];
            const auto& a = points[ia]; const auto& b = points[ib];
            const auto& c = points[ic];
            if (!a.valid || !b.valid || !c.valid) continue;
            const float denominator = (b.screen.y - c.screen.y) *
                (a.screen.x - c.screen.x) + (c.screen.x - b.screen.x) *
                (a.screen.y - c.screen.y);
            if (std::abs(denominator) < 1e-6f) continue;
            const float wa = ((b.screen.y - c.screen.y) * (x - c.screen.x) +
                (c.screen.x - b.screen.x) * (y - c.screen.y)) / denominator;
            const float wb = ((c.screen.y - a.screen.y) * (x - c.screen.x) +
                (a.screen.x - c.screen.x) * (y - c.screen.y)) / denominator;
            const float wc = 1.f - wa - wb;
            if (wa < 0.f || wb < 0.f || wc < 0.f) continue;
            const float depth = wa * a.depth + wb * b.depth + wc * c.depth;
            if (depth >= nearest) continue;
            const float pa = wa * a.inverseW, pb = wb * b.inverseW,
                pc = wc * c.inverseW;
            const float sum = pa + pb + pc;
            if (sum <= 0.f) continue;
            const auto& va = vertices[ia].pos;
            const auto& vb = vertices[ib].pos;
            const auto& vc = vertices[ic].pos;
            local = glm::vec3((pa * va[0] + pb * vb[0] + pc * vc[0]) / sum,
                (pa * va[1] + pb * vb[1] + pc * vc[1]) / sum,
                (pa * va[2] + pb * vb[2] + pc * vc[2]) / sum);
            nearest = depth;
        }
        if (!local) return std::nullopt;
        local->x = -local->x;
        const glm::vec4 clip = transform * glm::vec4(*local, 1.f);
        if (clip.w <= .0001f) return std::nullopt;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return EditorUiVec2{ (ndc.x * .5f + .5f) * input.available.x,
            (.5f - ndc.y * .5f) * input.available.y };
    };
    if (session.vertexPaintMode && session.paintTarget == 1 &&
        session.vertexPaintTool != 0)
    {
        int uvSet = 0;
        auto* material = mesh->Owner->GetComponent<Engine::Components::Material>();
        auto texture = material ? PaintTextureForSlot(*material,
            session.textureSlot, uvSet) : nullptr;
        if (!texture || (uvSet != 0 && uvSet != 1) ||
            std::filesystem::path(texture->GetFilePath()).extension() != ".png" ||
            !texture->Load())
        {
            session.toolError = "Texture paint needs a PNG in the selected material slot with UV set 0 or 1.";
            return true;
        }
        const std::string path = texture->GetFilePath();
        if (session.texturePaintPath != path)
        {
            FinishTexturePaintStroke(session);
            session.texturePaintPath = path;
            session.texturePaintWidth = texture->GetWidth();
            session.texturePaintHeight = texture->GetHeight();
            const size_t bytes = static_cast<size_t>(session.texturePaintWidth) *
                session.texturePaintHeight * 4;
            if (texture->GetPixels().size() < bytes)
            { session.toolError = "The selected PNG has incomplete RGBA pixels."; return true; }
            session.texturePaintPixels.assign(texture->GetPixels().begin(),
                texture->GetPixels().begin() + bytes);
            session.textureUndo.clear();
            session.textureRedo.clear();
        }
        const auto cursor = input.mousePosInViewport;
        const float radius = std::max(1.f, session.vertexBrush.size * .5f *
            Paint::PressureScale(session.vertexBrush.pressureSize));
        ui.DrawViewportCircle(cursor, session.vertexPaintTool == 3 ? 5.f :
            radius, { 1.f, .85f, .25f, .95f }, false);
        if (input.leftReleased || !input.leftDown)
        { FinishTexturePaintStroke(session); return true; }
        if (!input.hovered || input.rightDown || input.middleDown) return true;
        if (session.vertexBrush.shape != 0 ||
            session.vertexBrush.rotation != 0.f ||
            session.vertexBrush.scatter != 0.f ||
            session.vertexBrush.falloff < 0 ||
            session.vertexBrush.falloff > 2)
        {
            session.toolError = "This brush uses shape, rotation, or scatter settings unsupported by mesh texture paint.";
            return true;
        }
        std::shared_ptr<Engine::Components::Texture> brushTip;
        bool tipUsesLuminance = false;
        if (!session.vertexBrush.tipPath.empty() &&
            session.vertexPaintTool != 3)
        {
            brushTip = Engine::Components::Texture::Acquire(
                session.vertexBrush.tipPath, false);
            if (!brushTip || !brushTip->Load() || !brushTip->GetWidth() ||
                !brushTip->GetHeight() || brushTip->GetPixels().size() <
                    static_cast<size_t>(brushTip->GetWidth()) *
                        brushTip->GetHeight() * 4)
            {
                session.toolError = "Could not load the selected brush image tip.";
                return true;
            }
            tipUsesLuminance = true;
            const auto& tipPixels = brushTip->GetPixels();
            const size_t tipBytes = static_cast<size_t>(brushTip->GetWidth()) *
                brushTip->GetHeight() * 4;
            for (size_t alpha = 3; alpha < tipBytes; alpha += 4)
                if (tipPixels[alpha] != 255)
                { tipUsesLuminance = false; break; }
        }
        if (!session.texturePainting && !input.leftClicked) return true;
        if (session.vertexPaintTool == 3 && !input.leftClicked) return true;
        const float spacing = std::max(1.f,
            session.vertexBrush.size * session.vertexBrush.spacing);
        if (session.texturePainting && session.vertexPaintTool != 3 &&
            std::hypot(cursor.x - session.lastVertexPaintPosition.x,
                cursor.y - session.lastVertexPaintPosition.y) < spacing)
            return true;
        if (!session.texturePainting)
        {
            session.texturePainting = true;
            session.textureStrokeChanged = false;
            session.textureStrokeBefore = session.texturePaintPixels;
            session.lastVertexPaintPosition = { -10000.f, -10000.f };
        }
        std::vector<Paint::ProjectedPaintVertex> projected;
        projected.reserve(points.size());
        for (const Point& point : points)
            projected.push_back({ point.screen.x, point.screen.y,
                point.depth, point.inverseW, point.valid });
        const uint8_t color[4]{
            static_cast<uint8_t>(std::lround(std::clamp(session.vertexPaintColor[0], 0.f, 1.f) * 255.f)),
            static_cast<uint8_t>(std::lround(std::clamp(session.vertexPaintColor[1], 0.f, 1.f) * 255.f)),
            static_cast<uint8_t>(std::lround(std::clamp(session.vertexPaintColor[2], 0.f, 1.f) * 255.f)),
            static_cast<uint8_t>(std::lround(std::clamp(session.vertexPaintColor[3], 0.f, 1.f) * 255.f)) };
        const float opacity = std::clamp(session.vertexBrush.strength *
            session.vertexBrush.opacity * session.vertexBrush.flow *
            Paint::PressureScale(session.vertexBrush.pressureStrength), 0.f, 1.f);
        bool changed = false;
        if (session.vertexPaintTool == 3)
        {
            if (session.textureAlphaLock || session.paintMirrorX)
            { session.toolError = "Texture bucket does not support Alpha Lock or Mirror X."; return true; }
            if (auto texel = Paint::PickMeshTextureTexel(
                    session.texturePaintWidth, session.texturePaintHeight,
                    vertices, indices, projected, cursor.x, cursor.y, uvSet))
                changed = Paint::Fill(session.texturePaintPixels,
                    session.texturePaintWidth, session.texturePaintHeight,
                    static_cast<uint32_t>(*texel % session.texturePaintWidth),
                    static_cast<uint32_t>(*texel / session.texturePaintWidth),
                    session.vertexBucketTolerance, color);
        }
        else
        {
            const auto last = session.lastVertexPaintPosition;
            const float distance = last.x < 0.f ? 0.f :
                std::hypot(cursor.x - last.x, cursor.y - last.y);
            const int steps = last.x < 0.f ? 1 :
                std::max(1, static_cast<int>(std::ceil(distance / spacing)));
            for (int step = 1; step <= steps; ++step)
            {
                const float t = static_cast<float>(step) / steps;
                const float x = last.x < 0.f ? cursor.x :
                    last.x + (cursor.x - last.x) * t;
                const float y = last.y < 0.f ? cursor.y :
                    last.y + (cursor.y - last.y) * t;
                changed |= Paint::StampMeshTexture(session.texturePaintPixels,
                    session.texturePaintWidth, session.texturePaintHeight,
                    vertices, indices, projected, x, y, radius,
                    session.vertexBrush.hardness, opacity, color,
                    session.vertexPaintTool == 2, uvSet,
                    session.vertexBrush.falloff,
                    brushTip ? &brushTip->GetPixels() : nullptr,
                    brushTip ? brushTip->GetWidth() : 0,
                    brushTip ? brushTip->GetHeight() : 0,
                    tipUsesLuminance, session.textureAlphaLock);
                if (session.paintMirrorX)
                    if (auto mirror = mirroredPaintCursor(x, y))
                        changed |= Paint::StampMeshTexture(
                            session.texturePaintPixels,
                            session.texturePaintWidth, session.texturePaintHeight,
                            vertices, indices, projected, mirror->x, mirror->y,
                            radius, session.vertexBrush.hardness, opacity, color,
                            session.vertexPaintTool == 2, uvSet,
                            session.vertexBrush.falloff,
                            brushTip ? &brushTip->GetPixels() : nullptr,
                            brushTip ? brushTip->GetWidth() : 0,
                            brushTip ? brushTip->GetHeight() : 0,
                            tipUsesLuminance, session.textureAlphaLock);
            }
        }
        session.lastVertexPaintPosition = cursor;
        if (changed)
        {
            session.textureStrokeChanged = true;
            if (texture->SetPreviewPixels(session.texturePaintPixels))
                texture->Prepare(scene->GetGraphicsProvider());
            session.toolError.clear();
            if (m_renderer) m_renderer->MarkDirty();
        }
        if (session.vertexPaintTool == 3) FinishTexturePaintStroke(session);
        return true;
    }
    if (session.vertexPaintMode && session.paintTarget == 0 &&
        session.vertexPaintTool != 0)
    {
        const auto cursor = input.mousePosInViewport;
        const float radius = std::max(2.f, session.vertexBrush.size * .5f *
            Paint::PressureScale(session.vertexBrush.pressureSize));
        if (session.vertexPaintTool != 0)
            ui.DrawViewportCircle(cursor, session.vertexPaintTool == 3 ? 5.f :
                radius, { 1.f, .85f, .25f, .95f }, false);
        if (session.vertexPaintTool == 0 || input.leftReleased ||
            !input.leftDown)
        {
            FinishVertexPaintStroke(session);
            return session.vertexPaintTool != 0;
        }
        if (!input.hovered || input.rightDown || input.middleDown)
            return true;
        if (session.vertexPaintTool != 3 &&
            (session.vertexBrush.shape != 0 ||
            session.vertexBrush.rotation != 0.f ||
            session.vertexBrush.scatter != 0.f ||
            session.vertexBrush.falloff < 0 ||
            session.vertexBrush.falloff > 2))
        {
            session.toolError = "This preset uses shape, rotation, or scatter settings unsupported by vertex paint.";
            return true;
        }
        std::shared_ptr<Engine::Components::Texture> vertexTip;
        bool tipUsesLuminance = false;
        if (!session.vertexBrush.tipPath.empty() &&
            session.vertexPaintTool != 3)
        {
            vertexTip = Engine::Components::Texture::Acquire(
                session.vertexBrush.tipPath, false);
            if (!vertexTip || !vertexTip->Load() || !vertexTip->GetWidth() ||
                !vertexTip->GetHeight() || vertexTip->GetPixels().size() <
                    static_cast<size_t>(vertexTip->GetWidth()) *
                        vertexTip->GetHeight() * 4)
            {
                session.toolError = "Could not load the selected brush image tip.";
                return true;
            }
            tipUsesLuminance = Paint::TipUsesLuminance(
                vertexTip->GetPixels(), vertexTip->GetWidth(),
                vertexTip->GetHeight());
        }
        float hitDepth = 2.f;
        uint32_t hitFace = UINT32_MAX;
        for (uint32_t face = 0; face < faceCount; ++face)
        {
            const auto& a = points[indices[face * 3]];
            const auto& b = points[indices[face * 3 + 1]];
            const auto& c = points[indices[face * 3 + 2]];
            if (!a.valid || !b.valid || !c.valid) continue;
            const float denominator = (b.screen.y - c.screen.y) *
                (a.screen.x - c.screen.x) + (c.screen.x - b.screen.x) *
                (a.screen.y - c.screen.y);
            if (std::abs(denominator) < 1e-5f) continue;
            const float u = ((b.screen.y - c.screen.y) *
                (cursor.x - c.screen.x) + (c.screen.x - b.screen.x) *
                (cursor.y - c.screen.y)) / denominator;
            const float v = ((c.screen.y - a.screen.y) *
                (cursor.x - c.screen.x) + (a.screen.x - c.screen.x) *
                (cursor.y - c.screen.y)) / denominator;
            if (u < 0.f || v < 0.f || u + v > 1.f) continue;
            const float depth = u * a.depth + v * b.depth +
                (1.f - u - v) * c.depth;
            if (depth < hitDepth) { hitDepth = depth; hitFace = face; }
        }
        if (hitFace == UINT32_MAX ||
            (!session.vertexPainting && !input.leftClicked)) return true;
        if (session.vertexPaintTool == 3 && !input.leftClicked) return true;
        const float spacing = std::max(1.f,
            session.vertexBrush.size * session.vertexBrush.spacing);
        if (session.vertexPainting && session.vertexPaintTool != 3 &&
            std::hypot(cursor.x - session.lastVertexPaintPosition.x,
                cursor.y - session.lastVertexPaintPosition.y) < spacing)
            return true;
        if (!session.vertexPainting)
        {
            session.vertexPainting = true;
            session.vertexStrokeChanged = false;
            session.vertexStrokeBefore = CaptureMeshSnapshot(*mesh);
        }
        session.lastVertexPaintPosition = cursor;
        auto painted = vertices;
        bool changed = false;
        const float opacity = std::clamp(session.vertexBrush.strength *
            session.vertexBrush.opacity * session.vertexBrush.flow *
            Paint::PressureScale(session.vertexBrush.pressureStrength),
            0.f, 1.f);
        const float hardness = std::clamp(session.vertexBrush.hardness, 0.f, 1.f);
        if (session.vertexPaintTool == 3)
        {
            if (session.paintMirrorX)
            { session.toolError = "Vertex bucket does not support Mirror X."; return true; }
            uint32_t seed = indices[hitFace * 3];
            float nearest = INFINITY;
            for (int corner = 0; corner < 3; ++corner)
            {
                const uint32_t id = indices[hitFace * 3 + corner];
                const float distance = std::hypot(
                    points[id].screen.x - cursor.x,
                    points[id].screen.y - cursor.y);
                if (distance < nearest) { nearest = distance; seed = id; }
            }
            std::vector<std::vector<uint32_t>> neighbors(painted.size());
            for (size_t face = 0; face + 2 < indices.size(); face += 3)
                for (int corner = 0; corner < 3; ++corner)
                {
                    const uint32_t a = indices[face + corner];
                    const uint32_t b = indices[face + (corner + 1) % 3];
                    neighbors[a].push_back(b);
                    neighbors[b].push_back(a);
                }
            const auto initial = painted[seed];
            std::vector<uint8_t> visited(painted.size(), 0);
            std::vector<uint32_t> queue{ seed };
            visited[seed] = 1;
            for (size_t head = 0; head < queue.size(); ++head)
            {
                const uint32_t id = queue[head];
                if (session.vertexSelectionMask && session.selectionMode == 0 &&
                    std::find(session.selectedElements.begin(),
                        session.selectedElements.end(), id) ==
                        session.selectedElements.end()) continue;
                bool matching = true;
                for (int channel = 0; channel < 4; ++channel)
                    matching &= std::abs(vertices[id].color[channel] -
                        initial.color[channel]) <=
                        session.vertexBucketTolerance;
                if (!matching) continue;
                for (int channel = 0; channel < 4; ++channel)
                {
                    float& value = painted[id].color[channel];
                    const float result = value +
                        (session.vertexPaintColor[channel] - value) * opacity;
                    changed |= std::abs(result - value) > 1e-6f;
                    value = result;
                }
                for (uint32_t neighbor : neighbors[id])
                    if (!visited[neighbor])
                    { visited[neighbor] = 1; queue.push_back(neighbor); }
            }
        }
        else
        {
          const auto mirror = session.paintMirrorX ?
              mirroredPaintCursor(cursor.x, cursor.y) :
              std::optional<EditorUiVec2>{};
          for (size_t i = 0; i < painted.size(); ++i)
          {
            if (session.vertexSelectionMask && session.selectionMode == 0 &&
                std::find(session.selectedElements.begin(),
                    session.selectedElements.end(), static_cast<uint32_t>(i)) ==
                    session.selectedElements.end()) continue;
            if (!points[i].valid || points[i].depth > hitDepth + .025f)
                continue;
            const float primaryDistance = std::hypot(
                points[i].screen.x - cursor.x,
                points[i].screen.y - cursor.y) / radius;
            const float mirrorDistance = mirror ? std::hypot(
                points[i].screen.x - mirror->x,
                points[i].screen.y - mirror->y) / radius : INFINITY;
            const bool useMirror = mirrorDistance < primaryDistance;
            const float distance = std::min(primaryDistance, mirrorDistance);
            if (distance > 1.f) continue;
            float falloff = distance <= hardness ? 1.f :
                hardness >= 1.f ? 0.f :
                (1.f - distance) / (1.f - hardness);
            if (session.vertexBrush.falloff == 0)
                falloff = distance <= hardness ? 1.f : 0.f;
            else if (session.vertexBrush.falloff == 2)
                falloff = falloff * falloff * (3.f - 2.f * falloff);
            const float tipAmount = vertexTip ? Paint::SampleTipMask(
                vertexTip->GetPixels(), vertexTip->GetWidth(),
                vertexTip->GetHeight(),
                (points[i].screen.x - (useMirror ? mirror->x : cursor.x)) / radius,
                (points[i].screen.y - (useMirror ? mirror->y : cursor.y)) / radius,
                tipUsesLuminance) : 1.f;
            const float amount = opacity * falloff * tipAmount;
            for (int channel = 0; channel < 4; ++channel)
            {
                const float target = session.vertexPaintTool == 2 ? 1.f :
                    session.vertexPaintColor[channel];
                float& value = painted[i].color[channel];
                const float result = std::clamp(value +
                    (target - value) * amount, 0.f, 1.f);
                changed |= std::abs(result - value) > 1e-6f;
                value = result;
            }
          }
        }
        if (changed && mesh->SetIndexedGeometry(std::move(painted),
            std::vector<uint32_t>(indices)))
        {
            session.vertexStrokeChanged = true;
            session.toolError.clear();
            if (m_renderer) m_renderer->MarkDirty();
        }
        if (session.vertexPaintTool == 3) FinishVertexPaintStroke(session);
        return true;
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
                        session.dirty = HistoryDiffers(session.baseline, session.savedSnapshot);
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
    if (session) FinishVertexPaintStroke(*session);
    Engine::Scene::Scene* scene = GetActiveDocumentScene();
    SkeletonEditSession* skeleton = ActiveSkeletonEditSession();
    const bool paint = skeleton && skeleton->enabled;
    if (!session || (!session->enabled && !paint) || !scene) return false;
    if (session->enabled) SyncMeshEditSelection(scene, *session);
    else if (paint && !session->activeMesh)
        session->activeMesh = skeleton->mesh;
    if (!session->activeMesh ||
        !StepMeshSnapshotHistory(*session->activeMesh, redo,
            session->undo, session->redo, session->baseline)) return false;
    session->dirty = HistoryDiffers(session->baseline, session->savedSnapshot);
    if (m_activeSceneAssetDocument)
        RefreshSceneAssetDocumentTitle(*m_activeSceneAssetDocument);
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
    FinishVertexPaintStroke(session);
    FinishTexturePaintStroke(session);
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

}
