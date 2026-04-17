#include "pch.h"
#include "Core/SceneManager.h"
#include "Core/Scene/Scene.h"

// Static member initialization
Scene* SceneManager::s_activeScene = nullptr;
SceneManager::SceneLoadedCallback SceneManager::s_onSceneLoaded = nullptr;

// ---------------------------------------------------------------------------
// SceneManager::LoadScene
// ---------------------------------------------------------------------------
bool SceneManager::LoadScene(const std::string& path)
{
    if (!s_activeScene)
        return false;

    // Clear and load the new scene
    if (!s_activeScene->Load(path))
        return false;

    // Invoke callback if set
    if (s_onSceneLoaded)
        s_onSceneLoaded(s_activeScene);

    return true;
}

// ---------------------------------------------------------------------------
// SceneManager::LoadSceneAsync
// ---------------------------------------------------------------------------
void SceneManager::LoadSceneAsync(const std::string& path,
                                   std::function<void(Scene*)> onComplete)
{
    // TODO: Implement with background thread pool.
    // For now, just do it synchronously and invoke the callback.
    if (!s_activeScene)
        return;

    if (s_activeScene->Load(path))
    {
        if (s_onSceneLoaded)
            s_onSceneLoaded(s_activeScene);
        if (onComplete)
            onComplete(s_activeScene);
    }
}
