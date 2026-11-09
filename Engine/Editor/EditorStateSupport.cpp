#include "pch.h"
#include "EditorStateSupport.h"
#include "Core/Rendering/RenderJobs.h"

namespace Engine::Editor::StateSupport
{
    bool HistoryDiffers(const std::string& left, const std::string& right)
    {
        if (left.size() != right.size()) return true;
        if (left.size() < 64 * 1024) return left != right;
        return Engine::Rendering::RenderJobs::Get().Submit(
            [&left, &right] { return left != right; }).get();
    }

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

}
