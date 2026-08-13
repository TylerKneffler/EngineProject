#include "ProblemsView.h"
#include "Engine/Editor/UI/IEditorUi.h"

namespace Engine::Editor
{
void EditorProblemStore::Add(EditorProblemSeverity severity,
    const std::string& message)
{
    if (message.empty()) return;
    ++m_revision;
    m_problems.push_back({ severity, message });
    severity == EditorProblemSeverity::Error ? ++m_errorCount : ++m_warningCount;
    if (m_problems.size() <= kMaximumProblems) return;
    const EditorProblemSeverity removed = m_problems.front().severity;
    removed == EditorProblemSeverity::Error ? --m_errorCount : --m_warningCount;
    m_problems.erase(m_problems.begin());
}

void EditorProblemStore::Clear()
{
    m_problems.clear();
    m_warningCount = 0;
    m_errorCount = 0;
    ++m_revision;
}

void ProblemsView::RebuildText()
{
    m_textBlock.clear();
    if (!m_store) return;
    for (const EditorProblem& problem : m_store->GetProblems())
    {
        if (problem.severity == EditorProblemSeverity::Warning && !m_showWarnings)
            continue;
        if (problem.severity == EditorProblemSeverity::Error && !m_showErrors)
            continue;
        if (!m_textBlock.empty()) m_textBlock.push_back('\n');
        m_textBlock += problem.severity == EditorProblemSeverity::Error
            ? "[Error] " : "[Warning] ";
        m_textBlock += problem.message;
    }
    m_lastRevision = m_store->Revision();
    m_filtersChanged = false;
}

void ProblemsView::DrawPanel(IEditorUi& ui)
{
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }
    if (ui.Button("Clear") && m_store)
    {
        m_store->Clear();
        m_filtersChanged = true;
    }
    ui.SameLine();
    const std::string errors = "Errors: " +
        std::to_string(m_store ? m_store->ErrorCount() : 0);
    ui.ColoredLabel(errors.c_str(), { 1.f, .35f, .35f, 1.f });
    ui.SameLine();
    const std::string warnings = "Warnings: " +
        std::to_string(m_store ? m_store->WarningCount() : 0);
    ui.ColoredLabel(warnings.c_str(), { 1.f, .75f, .25f, 1.f });
    ui.SameLine();
    if (ui.Checkbox("Show errors", &m_showErrors)) m_filtersChanged = true;
    ui.SameLine();
    if (ui.Checkbox("Show warnings", &m_showWarnings)) m_filtersChanged = true;
    ui.Separator();
    if (m_filtersChanged || (m_store && m_lastRevision != m_store->Revision()))
        RebuildText();
    ui.ReadOnlyTextBlock("##problemsText", m_textBlock.c_str());
    ui.EndWindow();
}
}
