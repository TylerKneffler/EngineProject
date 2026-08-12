#include "TerminalView.h"
#include "Engine/Editor/UI/IEditorUi.h"
#include <algorithm>

namespace
{
constexpr size_t kMaximumTerminalText = 1024 * 1024;
}

TerminalView::~TerminalView()
{
    StopShell();
}

void TerminalView::Init(std::string workingDirectory)
{
    m_workingDirectory = std::move(workingDirectory);
    StartShell();
}

bool TerminalView::StartShell()
{
    if (m_process)
        return true;

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE stdoutWrite = nullptr;
    HANDLE stdinRead = nullptr;
    if (!CreatePipe(&m_stdoutRead, &stdoutWrite, &security, 0))
        return false;
    if (!SetHandleInformation(m_stdoutRead, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stdinRead, &m_stdinWrite, &security, 0) ||
        !SetHandleInformation(m_stdinWrite, HANDLE_FLAG_INHERIT, 0))
    {
        if (m_stdoutRead) CloseHandle(m_stdoutRead);
        if (stdoutWrite) CloseHandle(stdoutWrite);
        if (stdinRead) CloseHandle(stdinRead);
        if (m_stdinWrite) CloseHandle(m_stdinWrite);
        m_stdoutRead = nullptr;
        m_stdinWrite = nullptr;
        return false;
    }

    std::wstring command = L"powershell.exe -NoLogo -NoProfile -Command -";
    std::vector<wchar_t> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdinRead;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = stdoutWrite;
    PROCESS_INFORMATION process{};

    std::wstring workingDirectory;
    if (!m_workingDirectory.empty())
        workingDirectory = std::filesystem::path(m_workingDirectory).wstring();
    const BOOL started = CreateProcessW(nullptr, commandBuffer.data(), nullptr,
        nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        &startup, &process);

    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    if (!started)
    {
        CloseHandle(m_stdoutRead);
        CloseHandle(m_stdinWrite);
        m_stdoutRead = nullptr;
        m_stdinWrite = nullptr;
        const char message[] = "Unable to start PowerShell.\n";
        AppendOutput(message, sizeof(message) - 1);
        return false;
    }

    CloseHandle(process.hThread);
    m_process = process.hProcess;
    const std::string banner = "PowerShell - " +
        (m_workingDirectory.empty() ? std::filesystem::current_path().string() : m_workingDirectory) + "\n";
    AppendOutput(banner.data(), banner.size());
    return true;
}

void TerminalView::StopShell()
{
    if (m_stdinWrite)
    {
        const char exitCommand[] = "exit\r\n";
        DWORD written = 0;
        WriteFile(m_stdinWrite, exitCommand, sizeof(exitCommand) - 1, &written, nullptr);
        CloseHandle(m_stdinWrite);
        m_stdinWrite = nullptr;
    }
    if (m_process)
    {
        if (WaitForSingleObject(m_process, 250) == WAIT_TIMEOUT)
            TerminateProcess(m_process, 0);
        CloseHandle(m_process);
        m_process = nullptr;
    }
    if (m_stdoutRead)
    {
        CloseHandle(m_stdoutRead);
        m_stdoutRead = nullptr;
    }
}

void TerminalView::RestartShell()
{
    StopShell();
    m_output.clear();
    StartShell();
}

void TerminalView::AppendOutput(const char* bytes, size_t size)
{
    m_output.append(bytes, size);
    if (m_output.size() > kMaximumTerminalText)
    {
        const size_t remove = m_output.size() - kMaximumTerminalText;
        const size_t line = m_output.find('\n', remove);
        m_output.erase(0, line == std::string::npos ? remove : line + 1);
    }
    m_scrollToBottom = true;
}

void TerminalView::DrainOutput()
{
    if (!m_stdoutRead)
        return;

    DWORD available = 0;
    while (PeekNamedPipe(m_stdoutRead, nullptr, 0, nullptr, &available, nullptr) && available)
    {
        char buffer[2048];
        const DWORD requested = std::min<DWORD>(available, sizeof(buffer));
        DWORD read = 0;
        if (!ReadFile(m_stdoutRead, buffer, requested, &read, nullptr) || !read)
            break;
        AppendOutput(buffer, read);
    }

    if (m_process && WaitForSingleObject(m_process, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(m_process);
        m_process = nullptr;
        if (m_stdinWrite) { CloseHandle(m_stdinWrite); m_stdinWrite = nullptr; }
        const char message[] = "\n[PowerShell exited. Click Restart to open a new session.]\n";
        AppendOutput(message, sizeof(message) - 1);
    }
}

void TerminalView::SubmitCommand()
{
    if (!m_command[0])
        return;
    if (!m_process && !StartShell())
        return;

    const std::string input(m_command.data());
    const std::string shown = "PS> " + input + "\n";
    AppendOutput(shown.data(), shown.size());
    const std::string wire = input + "\r\n";
    DWORD written = 0;
    if (!WriteFile(m_stdinWrite, wire.data(), static_cast<DWORD>(wire.size()),
        &written, nullptr))
    {
        const char message[] = "[Failed to send command.]\n";
        AppendOutput(message, sizeof(message) - 1);
    }
    m_command.fill('\0');
}

void TerminalView::DrawPanel(IEditorUi& ui)
{
    DrainOutput();
    if (!ui.BeginWindow(m_title.c_str(), &m_open))
    {
        ui.EndWindow();
        return;
    }
    if (ui.Button("Clear"))
        m_output.clear();
    ui.SameLine();
    if (ui.Button("Restart"))
        RestartShell();
    ui.SameLine();
    ui.DisabledLabel(m_process ? "PowerShell" : "PowerShell stopped");
    ui.Separator();
    const bool scroll = m_scrollToBottom;
    ui.ReadOnlyTextBlock("##terminalOutput", m_output.c_str(), scroll, 30.f);
    if (scroll) m_scrollToBottom = false;
    if (ui.InputTextSubmit("##terminalCommand", m_command.data(), m_command.size()))
        SubmitCommand();
    ui.EndWindow();
}
