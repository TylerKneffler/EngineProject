#include "pch.h"
#include "PostProcess.h"

namespace Engine::Graphics
{
namespace { thread_local PostProcessSettings g_settings{}; }

void SetPostProcessSettings(const PostProcessSettings& settings)
{
    g_settings = settings;
}

PostProcessSettings GetPostProcessSettings()
{
    return g_settings;
}
}
