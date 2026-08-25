#pragma once
#include "pch.h"
#include <functional>

namespace Engine::Scene { class Scene; }

// ---------------------------------------------------------------------------
// SceneManager — Scene lifecycle and transition management
//
// Provides a global singleton interface for scripts to request scene changes,
// load scenes in the background, and query the active scene.
//
// Usage in scripts:
//   SceneManager::LoadScene("Assets/Scenes/level1.scene");
//   SceneManager::SetOnSceneLoaded([](Scene* newScene) { /* react */ });
// ---------------------------------------------------------------------------
namespace Engine::Core
{
class SceneManager
{
public:
    // ---- Scene Access ----
    
    // Get the currently active scene
    static Engine::Scene::Scene* GetActiveScene() { return s_activeScene; }
    
    // ---- Scene Transitions ----
    
    // Immediately load a scene, unloading the current one
    // Returns true if successful
    static bool LoadScene(const std::string& path);

    // Queues a transition for a safe point after script/component updates.
    // UI listeners should use these methods instead of loading immediately.
    static void RequestSceneLoad(const std::string& path);
    static bool RequestDefaultSceneLoad();
    static bool ProcessPendingSceneLoad();
    static void CancelPendingSceneLoad() { s_pendingScenePath.clear(); }

    static void SetDefaultScenePath(const std::string& path)
    {
        s_defaultScenePath = path;
    }
    static const std::string& GetDefaultScenePath() { return s_defaultScenePath; }
    
    // Load a scene in the background without blocking the main thread
    // When load completes, the callback is invoked with the loaded scene
    // Note: Callbacks are invoked on the background thread - ensure thread safety
    //       or queue operations for the main thread as needed
    static void LoadSceneAsync(const std::string& path, 
                               std::function<void(Engine::Scene::Scene*)> onComplete);
    
    // ---- Callbacks ----
    
    // Called when a scene finishes loading (for both LoadScene and LoadSceneAsync)
    using SceneLoadedCallback = std::function<void(Engine::Scene::Scene*)>;
    static void SetOnSceneLoaded(SceneLoadedCallback callback) 
    { 
        s_onSceneLoaded = callback; 
    }
    
    // ---- Internal API (called from Main.cpp) ----
    
    // Set the active scene (called during startup or when loading a new scene)
    static void SetActiveScene(Engine::Scene::Scene* scene) { s_activeScene = scene; }

private:
    static Engine::Scene::Scene* s_activeScene;
    static SceneLoadedCallback s_onSceneLoaded;
    static std::string s_defaultScenePath;
    static std::string s_pendingScenePath;
};
}
