#include "pch.h"
#include "Core/SceneManager.h"
#include "Core/Scene/Scene.h"
#include <future>
#include <thread>

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
    if (!s_activeScene)
    {
        if (onComplete)
            onComplete(nullptr);
        return;
    }

    // Launch async loading task using std::async
    // Note: Scene loading typically involves file I/O which is safe to do on background thread,
    // but graphics resource creation must happen on main thread
    std::thread([path, onComplete, activeScene = s_activeScene]()
    {
        // Perform scene loading on background thread
        bool success = activeScene->Load(path);
        
        // Schedule callbacks to run on main thread (or invoke directly if safe)
        // For now, invoke callbacks directly - proper implementation would queue for main thread
        if (success)
        {
            if (s_onSceneLoaded)
                s_onSceneLoaded(activeScene);
            if (onComplete)
                onComplete(activeScene);
        }
        else
        {
            if (onComplete)
                onComplete(nullptr);
        }
    }).detach(); // Detach thread to run independently
}
