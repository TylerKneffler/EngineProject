#pragma once
#include "pch.h"
#include "View/IEditorPanel.h"

namespace Engine::Editor
{
enum class EditorProblemSeverity { Warning, Error };

struct EditorProblem
{
    EditorProblemSeverity severity = EditorProblemSeverity::Warning;
    std::string message;
};

class EditorProblemStore
{
public:
    void Add(EditorProblemSeverity severity, const std::string& message);
    void Clear();
    const std::vector<EditorProblem>& GetProblems() const { return m_problems; }
    size_t WarningCount() const { return m_warningCount; }
    size_t ErrorCount() const { return m_errorCount; }
    uint64_t Revision() const { return m_revision; }

private:
    static constexpr size_t kMaximumProblems = 5000;
    std::vector<EditorProblem> m_problems;
    size_t m_warningCount = 0;
    size_t m_errorCount = 0;
    uint64_t m_revision = 0;
};

class ProblemsView : public IEditorPanel
{
public:
    explicit ProblemsView(std::shared_ptr<EditorProblemStore> store)
        : m_store(std::move(store)) {}

    void DrawPanel(IEditorUi& ui) override;

private:
    void RebuildText();
    std::shared_ptr<EditorProblemStore> m_store;
    std::string m_textBlock;
    uint64_t m_lastRevision = static_cast<uint64_t>(-1);
    bool m_showWarnings = true;
    bool m_showErrors = true;
    bool m_filtersChanged = true;
};
}
