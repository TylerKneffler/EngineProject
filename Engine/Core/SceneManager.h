#pragma once
#include "pch.h"
#include <functional>

class Scene;

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
class SceneManager
{
public:
    // ---- Scene Access ----
    
    // Get the currently active scene
    static Scene* GetActiveScene() { return s_activeScene; }
    
    // ---- Scene Transitions ----
    
    // Immediately load a scene, unloading the current one
    // Returns true if successful
    static bool LoadScene(const std::string& path);
    
    // Load a scene in the background without changing the active scene
    // When load completes, the callback is invoked with the loaded scene
    // TODO: implement async with thread pool
    static void LoadSceneAsync(const std::string& path, 
                               std::function<void(Scene*)> onComplete);
    
    // ---- Callbacks ----
    
    // Called when a scene finishes loading (for both LoadScene and LoadSceneAsync)
    using SceneLoadedCallback = std::function<void(Scene*)>;
    static void SetOnSceneLoaded(SceneLoadedCallback callback) 
    { 
        s_onSceneLoaded = callback; 
    }
    
    // ---- Internal API (called from Main.cpp) ----
    
    // Set the active scene (called during startup or when loading a new scene)
    static void SetActiveScene(Scene* scene) { s_activeScene = scene; }

private:
    static Scene* s_activeScene;
    static SceneLoadedCallback s_onSceneLoaded;
};
