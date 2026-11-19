#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Engine::Editor
{
// ---------------------------------------------------------------------------
// AssetGeneratorRunner
//
// Runs the repository's Python asset generators (tools/generate_*.py and
// tools/cook_*.py: conifers, boulders, forest clusters, sky sprites, foliage
// texture cooking) from the editor. One generator runs at a time, as a
// background process without a console window; its output is collected for
// the Console. When it exits, the files it wrote under the engine's Assets
// folder are listed so the editor can reload cached textures and refresh the
// open scenes (the asset browser picks up new files through its own watch).
//
// Usage:
//   runner.Start(runner.Generators()[0], &error);
//   ... every frame: if (runner.Poll(result)) { log result, reload assets }
// ---------------------------------------------------------------------------
class AssetGeneratorRunner
{
public:
    struct Generator
    {
        std::filesystem::path script;   // absolute path of the .py file
        std::string label;              // "Generate conifers"
        std::string summary;            // first line of the script's docstring
    };
    struct Result
    {
        std::string label;
        int exitCode = -1;
        double seconds = 0.0;
        std::vector<std::string> output;               // stdout + stderr lines
        std::vector<std::filesystem::path> changedFiles; // under the Assets folder
    };

    AssetGeneratorRunner();
    ~AssetGeneratorRunner();
    AssetGeneratorRunner(const AssetGeneratorRunner&) = delete;
    AssetGeneratorRunner& operator=(const AssetGeneratorRunner&) = delete;

    // Generators found under <engine root>/tools, sorted by label; scanned
    // on first use and by Rescan().
    const std::vector<Generator>& Generators();
    void Rescan();
    const std::filesystem::path& EngineRoot() const { return m_engineRoot; }
    const std::filesystem::path& AssetsRoot() const { return m_assetsRoot; }

    bool Start(const Generator& generator, std::string* error = nullptr);
    bool IsRunning() const { return static_cast<bool>(m_run); }
    std::string RunningLabel() const;
    double RunningSeconds() const;
    // Lines printed by the running generator since the last call.
    std::vector<std::string> TakeNewOutput();
    // True once per finished run, with its result.
    bool Poll(Result& result);

private:
    struct Run;
    static std::string LabelFor(const std::filesystem::path& script);
    static std::string SummaryOf(const std::filesystem::path& script);
    std::filesystem::path m_engineRoot;
    std::filesystem::path m_assetsRoot;
    std::vector<Generator> m_generators;
    bool m_scanned = false;
    std::unique_ptr<Run> m_run;
};
}
