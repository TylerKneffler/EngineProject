#include "pch.h"
#include "Core/SceneManager.h"
#include "Core/Scene/Scene.h"
#include <future>
#include <thread>

namespace Engine::Core
{

// Static member initialization
Engine::Scene::Scene* SceneManager::s_activeScene = nullptr;
SceneManager::SceneLoadedCallback SceneManager::s_onSceneLoaded = nullptr;
std::string SceneManager::s_defaultScenePath;
std::string SceneManager::s_pendingScenePath;

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

void SceneManager::RequestSceneLoad(const std::string& path)
{
    if (!path.empty())
        s_pendingScenePath = path;
}

bool SceneManager::RequestDefaultSceneLoad()
{
    if (s_defaultScenePath.empty())
        return false;
    RequestSceneLoad(s_defaultScenePath);
    return true;
}

bool SceneManager::ProcessPendingSceneLoad()
{
    if (s_pendingScenePath.empty())
        return false;

    std::string path = std::move(s_pendingScenePath);
    s_pendingScenePath.clear();
    if (!LoadScene(path))
        return false;

    // A transition creates a fresh component graph whose lifecycle must begin
    // before the next update/render frame.
    s_activeScene->Start();
    return true;
}

// ---------------------------------------------------------------------------
// SceneManager::LoadSceneAsync
// ---------------------------------------------------------------------------
void SceneManager::LoadSceneAsync(const std::string& path,
                                   std::function<void(Engine::Scene::Scene*)> onComplete)
{
    if (!s_activeScene)
    {
        if (onComplete)
            onComplete(nullptr);
        return;
    }

    // Launch async loading task using std::async
    // Note: Engine::Scene::Scene loading typically involves file I/O which is safe to do on background thread,
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

}
