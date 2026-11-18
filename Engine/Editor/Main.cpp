#include "Engine/Editor/EditorApplication.h"
#include "Core/Startup/LaunchActivation.h"

int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE /*previousInstance*/,
    _In_opt_ LPWSTR /*commandLine*/,
    _In_ int /*showCommand*/)
{
    // A background launch (ENGINE_LAUNCH_BACKGROUND=1, no-activate show
    // state) starts unfocused; keep it at full speed rather than letting
    // Windows move it to efficiency cores (see DisablePowerThrottling).
    if (Engine::Core::IsBackgroundLaunch())
        Engine::Core::DisablePowerThrottling();
    return Engine::Editor::EditorApplication::Run(instance);
}
