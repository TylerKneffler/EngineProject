#pragma once
#include "pch.h"
#include "View/IEditorPanel.h"

namespace Engine::Editor
{
// Interactive PowerShell session embedded in the editor. Output is drained
// without blocking the render loop and commands are sent over redirected stdin.
class TerminalView : public IEditorPanel
{
public:
    TerminalView() = default;
    ~TerminalView() override;

    void Init(std::string workingDirectory);
    void DrawPanel(IEditorUi& ui) override;

private:
    bool StartShell();
    void StopShell();
    void RestartShell();
    void DrainOutput();
    void SubmitCommand();
    void AppendOutput(const char* bytes, size_t size);

    std::string m_workingDirectory;
    std::string m_output;
    std::array<char, 1024> m_command{};
    HANDLE m_process = nullptr;
    HANDLE m_stdinWrite = nullptr;
    HANDLE m_stdoutRead = nullptr;
    bool m_scrollToBottom = false;
};
}
