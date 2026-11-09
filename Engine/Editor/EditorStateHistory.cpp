#include "pch.h"
#include "EditorState.h"
#include "Engine/Editor/Diagnostics/FrameActivity.h"
#include "Core/SceneManager.h"
#include "Core/Rendering/RenderJobs.h"
#include "Core/View/ViewFactory.h"
#include "Core/View/IEditorPanel.h"
#include "Core/View/View.h"
#include "Core/View/Views/ConsoleView.h"
#include "Core/View/Views/PropertiesView.h"
#include "Core/View/Views/HierarchyView.h"
#include "Core/View/Views/SceneView.h"
#include "Core/View/Views/GameView.h"
#include "Core/View/Views/AssetDocumentView.h"
#include "Core/Window.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include "Core/Compoonents/Obj/Mesh.h"
#include "Core/Compoonents/Materials/Texture.h"
#include "Core/Compoonents/Animation/Skeleton.h"
#include "Core/Compoonents/Transform.h"
#include "Core/MeshEditSnapshot.h"
#include "Core/SnapshotHistory.h"

#include "EditorStateSupport.h"

namespace Engine::Editor
{
using namespace StateSupport;

EditorState::HistoryEntry EditorState::CaptureHistoryEntry() const
{
    HistoryEntry entry;
    if (!m_scene)
        return entry;
    auto document = Engine::Serialization::SceneSerializer::CaptureSceneDocument(*m_scene);
    entry.scene = Engine::Rendering::RenderJobs::Get().Submit(
        [document = std::move(document)] {
            return Engine::Serialization::SceneSerializer::EncodeSceneSnapshot(document);
        }).get();
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
    if (m_activeAnimationDocument) return;
    if (m_activeSceneAssetDocument && m_activeSceneAssetDocument->scene)
    {
        if (m_activeSceneAssetDocument->previewAnimation)
            return;
        if (!allowHistory || editInProgress)
            return;
        SceneAssetDocument& document = *m_activeSceneAssetDocument;
        std::string current = CaptureSceneAssetDocumentSnapshot(document);
        if (HistoryDiffers(current, document.baseline))
        {
            if (m_historyLimit > 0 && document.undo.size() >= m_historyLimit)
                document.undo.pop_front();
            if (m_historyLimit > 0)
                document.undo.push_back(document.baseline);
            if (document.skeletonEdit.enabled)
            {
                auto& skeleton = document.skeletonEdit;
                RecordSkeletonHistory(skeleton,
                    skeleton.pendingBindingMeshHistory
                        ? SkeletonEditSession::HistoryKind::Binding
                        : SkeletonEditSession::HistoryKind::Scene);
                skeleton.pendingBindingMeshHistory = false;
            }
            document.redo.clear();
            document.baseline = std::move(current);
            document.dirty = HistoryDiffers(document.baseline, document.savedSnapshot);
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

    if (m_historyCapture.valid())
    {
        if (m_historyCapture.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready) return;
        HistoryCapture result = m_historyCapture.get();
        // A newer owner mutation supersedes this captured document. Its
        // worker may finish later, but it must never publish out of order.
        // The first capture is the undo baseline even if an edit arrived
        // before encoding finished; publish it before capturing the edit.
        if (m_historyBaseline.scene.empty() &&
            result.savedRevision == m_savedSceneRevision)
        {
            m_historyBaseline = std::move(result.entry);
            m_historyCapturedRevision = result.revision;
            if (m_savedSceneSnapshot.empty())
                m_savedSceneSnapshot = m_historyBaseline.scene;
        }
        else if (result.revision == m_sceneEditRevision &&
            result.savedRevision == m_savedSceneRevision)
        {
            m_historyCapturedRevision = result.revision;
            m_historySelectionDirty = false;
            if (result.changed)
            {
                if (m_historyLimit > 0 && !m_hasPendingHistoryEdit)
                {
                    m_pendingHistoryBefore = m_historyBaseline;
                    m_hasPendingHistoryEdit = true;
                    m_redoHistory.clear();
                }
                m_historyBaseline = std::move(result.entry);
                m_hasUnsavedChanges = result.dirty;
                RefreshSceneDocumentTitle();
                if (m_hasPendingHistoryEdit) CommitPendingHistoryEdit();
            }
            else
            {
                m_historyBaseline.hasSelection = result.entry.hasSelection;
                m_historyBaseline.selectionPath = std::move(result.entry.selectionPath);
                if (m_hasPendingHistoryEdit) CommitPendingHistoryEdit();
            }
        }
    }
    if (!m_historyBaseline.scene.empty() &&
        m_historyCapturedRevision == m_sceneEditRevision) return;
    HistoryEntry entry;
    CaptureHistorySelection(entry);
    auto document = Engine::Serialization::SceneSerializer::CaptureSceneDocument(*m_scene);
    const uint64_t revision = m_sceneEditRevision;
    const uint64_t savedRevision = m_savedSceneRevision;
    const std::string baseline = m_historyBaseline.scene;
    const std::string saved = m_savedSceneSnapshot;
    m_historyCapture = Engine::Rendering::RenderJobs::Get().Submit(
        [document = std::move(document), entry = std::move(entry),
         revision, savedRevision, baseline, saved]() mutable {
            entry.scene = Engine::Serialization::SceneSerializer::EncodeSceneSnapshot(document);
            const bool changed = entry.scene != baseline;
            const bool dirty = entry.scene != saved;
            return HistoryCapture{std::move(entry), revision, savedRevision,
                changed, dirty};
        });
}

void EditorState::FlushHistoryCapture()
{
    // Commands such as Undo are ordering barriers. Ordinary frames only poll.
    while (m_historyCapture.valid())
    {
        m_historyCapture.wait();
        if (m_activeSceneAssetDocument || m_activeAnimationDocument)
        { m_historyCapture.get(); break; }
        TrackSceneChanges(true, false);
    }
}

void EditorState::CommitPendingHistoryEdit()
{
    if (!m_hasPendingHistoryEdit)
        return;
    if (m_historyLimit > 0 &&
        HistoryDiffers(m_pendingHistoryBefore.scene, m_historyBaseline.scene))
    {
        m_undoHistory.push_back(std::move(m_pendingHistoryBefore));
        if (m_mainSkeletonEdit.enabled)
        {
            RecordSkeletonHistory(m_mainSkeletonEdit,
                m_mainSkeletonEdit.pendingBindingMeshHistory
                    ? SkeletonEditSession::HistoryKind::Binding
                    : SkeletonEditSession::HistoryKind::Scene);
            m_mainSkeletonEdit.pendingBindingMeshHistory = false;
        }
    }
    m_pendingHistoryBefore = {};
    m_hasPendingHistoryEdit = false;
    TrimHistory();
}

void EditorState::Undo()
{
    if (m_activeAnimationDocument)
    { StepAnimationHistory(*m_activeAnimationDocument, false); return; }
    MeshEditSession* paint = m_activeSceneAssetDocument
        ? &m_activeSceneAssetDocument->meshEdit
        : (m_prefabDocumentFocused && m_prefabScene
            ? &m_prefabMeshEdit : &m_mainMeshEdit);
    if (paint->enabled && paint->vertexPaintMode &&
        paint->paintTarget == 1 &&
        StepTexturePaintHistory(*paint, false)) return;
    if (ApplySkeletonHistory(false)) return;
    if (ApplyMeshHistory(false)) return;
    if (m_activeSceneAssetDocument)
    {
        SceneAssetDocument& document = *m_activeSceneAssetDocument;
        if (!StepSnapshotHistory(false, document.undo, document.redo,
            document.baseline, [&](const std::string& snapshot)
            { return RestoreSceneAssetDocumentSnapshot(document, snapshot); }))
            return;
        document.dirty = HistoryDiffers(document.baseline, document.savedSnapshot);
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
    FlushHistoryCapture();
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
    if (m_activeAnimationDocument)
    { StepAnimationHistory(*m_activeAnimationDocument, true); return; }
    MeshEditSession* paint = m_activeSceneAssetDocument
        ? &m_activeSceneAssetDocument->meshEdit
        : (m_prefabDocumentFocused && m_prefabScene
            ? &m_prefabMeshEdit : &m_mainMeshEdit);
    if (paint->enabled && paint->vertexPaintMode &&
        paint->paintTarget == 1 &&
        StepTexturePaintHistory(*paint, true)) return;
    if (ApplySkeletonHistory(true)) return;
    if (ApplyMeshHistory(true)) return;
    if (m_activeSceneAssetDocument)
    {
        SceneAssetDocument& document = *m_activeSceneAssetDocument;
        if (!StepSnapshotHistory(true, document.undo, document.redo,
            document.baseline, [&](const std::string& snapshot)
            { return RestoreSceneAssetDocumentSnapshot(document, snapshot); }))
            return;
        document.dirty = HistoryDiffers(document.baseline, document.savedSnapshot);
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
    FlushHistoryCapture();
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
    if (m_activeAnimationDocument)
        return !m_activeAnimationDocument->undo.empty();
    const MeshEditSession* edit = m_activeSceneAssetDocument
        ? &m_activeSceneAssetDocument->meshEdit
        : (m_prefabDocumentFocused && m_prefabScene
            ? &m_prefabMeshEdit : &m_mainMeshEdit);
    const SkeletonEditSession* skeleton = m_activeSceneAssetDocument
        ? &m_activeSceneAssetDocument->skeletonEdit
        : (m_prefabDocumentFocused && m_prefabScene
            ? &m_prefabSkeletonEdit : &m_mainSkeletonEdit);
    if (skeleton->enabled && !skeleton->undoOrder.empty()) return true;
    if (edit->enabled && edit->vertexPaintMode && edit->paintTarget == 1)
        return !edit->textureUndo.empty();
    if ((edit->enabled || (skeleton->enabled && skeleton->submode == 1)) &&
        !edit->undo.empty()) return true;
    return m_activeSceneAssetDocument ? !m_activeSceneAssetDocument->undo.empty()
        : m_activeAssetDocument ? m_activeAssetDocument->CanUndo()
        : (m_hasPendingHistoryEdit || !m_undoHistory.empty() ||
            m_historyCapture.valid());
}

bool EditorState::CanRedo() const
{
    if (m_activeAnimationDocument)
        return !m_activeAnimationDocument->redo.empty();
    const MeshEditSession* edit = m_activeSceneAssetDocument
        ? &m_activeSceneAssetDocument->meshEdit
        : (m_prefabDocumentFocused && m_prefabScene
            ? &m_prefabMeshEdit : &m_mainMeshEdit);
    const SkeletonEditSession* skeleton = m_activeSceneAssetDocument
        ? &m_activeSceneAssetDocument->skeletonEdit
        : (m_prefabDocumentFocused && m_prefabScene
            ? &m_prefabSkeletonEdit : &m_mainSkeletonEdit);
    if (skeleton->enabled && !skeleton->redoOrder.empty()) return true;
    if (edit->enabled && edit->vertexPaintMode && edit->paintTarget == 1)
        return !edit->textureRedo.empty();
    if ((edit->enabled || (skeleton->enabled && skeleton->submode == 1)) &&
        !edit->redo.empty()) return true;
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
    m_mainSkeletonEdit.skeleton = nullptr;
    m_mainSkeletonEdit.bindingMesh = nullptr;
    m_mainSkeletonEdit.mesh = nullptr;
    m_mainSkeletonEdit.adjacency.reset();
    m_mainSkeletonEdit.observedBindTransforms.clear();
    m_scene->SetEditorWeightPaint(nullptr, -1);
    m_scene->SetEditorMeshEditPose(m_mainMeshEdit.enabled ||
        m_mainSkeletonEdit.enabled);

    SelectObject(entry.hasSelection
        ? m_scene->FindObjectByPath(entry.selectionPath) : nullptr);
    // The restored source is itself the canonical history snapshot. Keeping
    // it avoids serializing the newly rebuilt scene for a second time.
    m_historyBaseline = std::move(entry);
    m_historyCapturedRevision = m_sceneEditRevision;
    m_historySelectionDirty = false;
    m_hasUnsavedChanges = HistoryDiffers(m_historyBaseline.scene, m_savedSceneSnapshot);
    RefreshSceneDocumentTitle();
    if (m_primaryConsole)
        m_primaryConsole->AddLog(ConsoleView::Level::Info,
            std::string(operation) + " completed.");
}

void EditorState::ResetHistory(bool sceneIsSaved)
{
    if (m_historyCapture.valid()) m_historyCapture = {};
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_pendingHistoryBefore = {};
    m_hasPendingHistoryEdit = false;
    m_historyBaseline = CaptureHistoryEntry();
    m_historyCapturedRevision = m_sceneEditRevision;
    m_historySelectionDirty = false;
    if (sceneIsSaved)
    {
        m_savedSceneSnapshot = m_historyBaseline.scene;
        m_savedSceneRevision = m_sceneEditRevision;
    }
}

void EditorState::SetHistoryLimit(uint32_t limit)
{
    m_historyLimit = std::min(limit, 1000u);
    const auto trimSkeleton = [&](SkeletonEditSession& session)
    {
        while (session.undoOrder.size() > m_historyLimit)
            session.undoOrder.pop_front();
        while (session.redoOrder.size() > m_historyLimit)
            session.redoOrder.pop_front();
    };
    trimSkeleton(m_mainSkeletonEdit);
    trimSkeleton(m_prefabSkeletonEdit);
    for (auto& document : m_sceneAssetDocuments)
        if (document)
        {
            trimSkeleton(document->skeletonEdit);
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

}
