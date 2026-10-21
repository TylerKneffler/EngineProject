#include "Engine/Editor/EditorApplication.h"

int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE /*previousInstance*/,
    _In_ LPWSTR /*commandLine*/,
    _In_ int /*showCommand*/)
{
    return Engine::Editor::EditorApplication::Run(instance);
}
