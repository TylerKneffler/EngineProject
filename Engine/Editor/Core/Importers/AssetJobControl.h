#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <array>
#include <filesystem>
#include <fstream>

namespace Engine::Editor
{
// Shared only between a worker and the editor thread. Workers publish a small
// scalar status; completed assets are handed to the editor through a future.
struct AssetJobControl
{
    std::atomic_bool cancelled{ false };
    std::atomic_bool committing{ false };
    std::atomic<float> progress{ 0.f };

    void SetProgress(float value)
    {
        progress.store(std::clamp(value, 0.f, 1.f),
            std::memory_order_release);
    }
    void Checkpoint() const
    {
        if (cancelled.load(std::memory_order_acquire))
            throw std::runtime_error("Asset job cancelled");
    }
};

// Importers can cooperate with cancellation without threading a parameter
// through every decoder and writer call. The scope is local to one worker.
inline thread_local AssetJobControl* currentAssetJob = nullptr;

class AssetJobScope
{
public:
    explicit AssetJobScope(AssetJobControl* job) : m_previous(currentAssetJob)
    { currentAssetJob = job; }
    ~AssetJobScope() { currentAssetJob = m_previous; }
    AssetJobScope(const AssetJobScope&) = delete;
    AssetJobScope& operator=(const AssetJobScope&) = delete;
private:
    AssetJobControl* m_previous;
};

inline void AssetJobCheckpoint(float progress = -1.f)
{
    if (!currentAssetJob) return;
    if (progress >= 0.f) currentAssetJob->SetProgress(progress);
    currentAssetJob->Checkpoint();
}

inline void AssetJobCopyFile(const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    namespace fs = std::filesystem;
    AssetJobCheckpoint();
    if (fs::exists(destination))
        throw std::runtime_error("Import destination exists: " +
            destination.string());
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!input || !output)
        throw std::runtime_error("Could not open asset copy: " +
            source.string());
    try
    {
        std::array<char, 256 * 1024> buffer;
        while (input)
        {
            AssetJobCheckpoint();
            input.read(buffer.data(), buffer.size());
            const auto bytes = input.gcount();
            if (bytes > 0) output.write(buffer.data(), bytes);
        }
        output.flush();
        if (!output || input.bad())
            throw std::runtime_error("Could not write asset copy: " +
                destination.string());
        AssetJobCheckpoint();
    }
    catch (...)
    {
        output.close();
        std::error_code ignored;
        fs::remove(destination, ignored);
        throw;
    }
}
}
