#include "pch.h"
#include "Engine/Editor/Tools/AssetGeneratorRunner.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#ifndef ENGINE_ROOT_PATH
#define ENGINE_ROOT_PATH "."
#endif

namespace fs = std::filesystem;

namespace Engine::Editor
{
struct AssetGeneratorRunner::Run
{
    std::string label;
    std::chrono::steady_clock::time_point started;
    fs::file_time_type startedOnDisk;
    fs::path assetsRoot;
    HANDLE process = nullptr;
    HANDLE output = nullptr;
    std::thread worker;
    std::mutex mutex;
    std::vector<std::string> fresh;
    std::vector<std::string> lines;
    std::string partial;
    std::vector<fs::path> changed;
    int exitCode = -1;
    std::atomic<bool> done{ false };

    void Append(const char* data, size_t size)
    {
        std::lock_guard lock(mutex);
        for (size_t i = 0; i < size; ++i)
        {
            const char c = data[i];
            if (c == '\r') continue;
            if (c != '\n') { partial.push_back(c); continue; }
            fresh.push_back(partial);
            lines.push_back(std::move(partial));
            partial.clear();
        }
    }

    // Reads the process output until it exits, then lists the files it
    // changed under the Assets folder (off the UI thread: a full scan).
    void Work()
    {
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(output, buffer, sizeof(buffer), &read, nullptr) && read)
            Append(buffer, read);
        WaitForSingleObject(process, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        exitCode = static_cast<int>(code);
        {
            std::lock_guard lock(mutex);
            if (!partial.empty()) { fresh.push_back(partial); lines.push_back(partial); partial.clear(); }
        }
        std::error_code error;
        // Two seconds of slack for file systems with coarse timestamps.
        const auto since = startedOnDisk - std::chrono::seconds(2);
        for (fs::recursive_directory_iterator it(assetsRoot,
                 fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error))
        {
            if (!it->is_regular_file(error)) continue;
            const auto written = it->last_write_time(error);
            if (!error && written >= since) changed.push_back(it->path());
        }
        done = true;
    }
};

AssetGeneratorRunner::AssetGeneratorRunner()
{
    std::error_code error;
    m_engineRoot = fs::weakly_canonical(fs::absolute(fs::path(ENGINE_ROOT_PATH)), error);
    if (error) m_engineRoot = fs::absolute(fs::path(ENGINE_ROOT_PATH));
    m_assetsRoot = m_engineRoot / "Engine" / "Core" / "Assets";
}

AssetGeneratorRunner::~AssetGeneratorRunner()
{
    if (!m_run) return;
    if (!m_run->done && m_run->process) TerminateProcess(m_run->process, 1);
    if (m_run->worker.joinable()) m_run->worker.join();
    if (m_run->process) CloseHandle(m_run->process);
    if (m_run->output) CloseHandle(m_run->output);
}

std::string AssetGeneratorRunner::LabelFor(const fs::path& script)
{
    // generate_forest_clusters -> "Generate forest clusters"
    std::string label = script.stem().string();
    std::replace(label.begin(), label.end(), '_', ' ');
    if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    return label;
}

std::string AssetGeneratorRunner::SummaryOf(const fs::path& script)
{
    std::ifstream file(script);
    std::string line;
    bool inDocstring = false;
    while (std::getline(file, line))
    {
        const auto quote = line.find("\"\"\"");
        if (!inDocstring)
        {
            if (quote == std::string::npos) continue;
            inDocstring = true;
            line = line.substr(quote + 3);
        }
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        line = line.substr(first);
        const auto end = line.find("\"\"\"");
        return end == std::string::npos ? line : line.substr(0, end);
    }
    return {};
}

void AssetGeneratorRunner::Rescan()
{
    m_scanned = true;
    m_generators.clear();
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(m_engineRoot / "tools", error))
    {
        const auto& path = entry.path();
        const std::string name = path.filename().string();
        if (path.extension() != ".py") continue;
        if (name.rfind("generate_", 0) != 0 && name.rfind("cook_", 0) != 0) continue;
        m_generators.push_back({ path, LabelFor(path), SummaryOf(path) });
    }
    std::sort(m_generators.begin(), m_generators.end(),
        [](const Generator& a, const Generator& b) { return a.label < b.label; });
}

const std::vector<AssetGeneratorRunner::Generator>& AssetGeneratorRunner::Generators()
{
    if (!m_scanned) Rescan();
    return m_generators;
}

bool AssetGeneratorRunner::Start(const Generator& generator, std::string* error)
{
    if (m_run)
    {
        if (error) *error = "An asset generator is already running.";
        return false;
    }
    SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &security, 0))
    {
        if (error) *error = "Could not create the generator output pipe.";
        return false;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = m_engineRoot.wstring();
    // "python" on PATH, else the Windows "py" launcher. -u: unbuffered output.
    BOOL started = FALSE;
    for (const wchar_t* interpreter : { L"python", L"py -3" })
    {
        std::wstring command = std::wstring(interpreter) + L" -u \"" + generator.script.wstring() + L"\"";
        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(L'\0');
        started = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
            workingDirectory.c_str(), &startup, &process);
        if (started) break;
    }
    CloseHandle(writeEnd);
    if (!started)
    {
        CloseHandle(readEnd);
        if (error) *error = "Could not start Python (python or py on PATH; numpy required).";
        return false;
    }
    CloseHandle(process.hThread);
    m_run = std::make_unique<Run>();
    m_run->label = generator.label;
    m_run->started = std::chrono::steady_clock::now();
    m_run->startedOnDisk = fs::file_time_type::clock::now();
    m_run->assetsRoot = m_assetsRoot;
    m_run->process = process.hProcess;
    m_run->output = readEnd;
    m_run->worker = std::thread([run = m_run.get()] { run->Work(); });
    return true;
}

std::string AssetGeneratorRunner::RunningLabel() const
{
    return m_run ? m_run->label : std::string{};
}

double AssetGeneratorRunner::RunningSeconds() const
{
    return m_run ? std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_run->started).count() : 0.0;
}

std::vector<std::string> AssetGeneratorRunner::TakeNewOutput()
{
    if (!m_run) return {};
    std::lock_guard lock(m_run->mutex);
    return std::exchange(m_run->fresh, {});
}

bool AssetGeneratorRunner::Poll(Result& result)
{
    if (!m_run || !m_run->done) return false;
    if (m_run->worker.joinable()) m_run->worker.join();
    result.label = m_run->label;
    result.exitCode = m_run->exitCode;
    result.seconds = RunningSeconds();
    result.output = std::move(m_run->lines);
    result.changedFiles = std::move(m_run->changed);
    CloseHandle(m_run->process);
    CloseHandle(m_run->output);
    m_run.reset();
    return true;
}
}
