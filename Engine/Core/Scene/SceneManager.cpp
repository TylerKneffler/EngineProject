#include "pch.h"
#include "Core/SceneManager.h"
#include "Core/Scene/Scene.h"
#include "Core/Serialization/SceneSerializer.h"
#include "Core/Serialization/Json.h"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Engine::Core
{
namespace
{
class AsyncSceneReader
{
public:
    using Callback = std::function<void(Engine::Scene::Scene*)>;
    struct Request
    {
        std::string path;
        Callback callback;
        uint64_t generation = 0;
        uint64_t sceneGeneration = 0;
        uint64_t id = 0;
    };
    struct Result
    {
        Engine::Serialization::JsonValue document;
        Callback callback;
        uint64_t generation = 0;
        uint64_t sceneGeneration = 0;
        bool readSucceeded = false;
    };

    AsyncSceneReader() = default;
    ~AsyncSceneReader()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_one();
        if (m_worker.joinable()) m_worker.join();
    }

    void Submit(std::string path, Callback callback, uint64_t generation,
        uint64_t sceneGeneration)
    {
        {
            std::lock_guard lock(m_mutex);
            if (!m_worker.joinable())
                m_worker = std::thread([this] { Run(); });
            const uint64_t id = ++m_latestId;
            m_pending = Request{std::move(path), std::move(callback),
                generation, sceneGeneration, id};
            m_completed.reset();
        }
        m_wake.notify_one();
    }

    void Cancel()
    {
        std::unique_lock lock(m_mutex);
        ++m_latestId;
        m_pending.reset();
        m_completed.reset();
        m_idle.wait(lock, [this] { return !m_active; });
    }

    std::optional<Result> TakeCompleted()
    {
        std::lock_guard lock(m_mutex);
        auto result = std::move(m_completed);
        m_completed.reset();
        return result;
    }

private:
    void Run()
    {
        std::unique_lock lock(m_mutex);
        while (true)
        {
            m_wake.wait(lock, [this] { return m_stopping || m_pending.has_value(); });
            if (m_stopping) return;
            Request request = std::move(*m_pending);
            m_pending.reset();
            m_active = true;
            lock.unlock();
            {
                Result result;
                result.generation = request.generation;
                result.sceneGeneration = request.sceneGeneration;
                result.callback = std::move(request.callback);
                try
                {
                    result.document = Engine::Serialization::SceneSerializer::ReadSceneFile(request.path);
                    result.readSucceeded = result.document.IsObject();
                }
                catch (...) { result.document = {}; result.readSucceeded = false; }
                lock.lock();
                if (request.id == m_latestId && !m_stopping)
                    m_completed = std::move(result);
            }
            m_active = false;
            m_idle.notify_all();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::optional<Request> m_pending;
    std::optional<Result> m_completed;
    uint64_t m_latestId = 0;
    bool m_stopping = false;
    bool m_active = false;
    std::thread m_worker;
};

AsyncSceneReader& SceneReader()
{
    static AsyncSceneReader reader;
    return reader;
}
}

// Static member initialization
Engine::Scene::Scene* SceneManager::s_activeScene = nullptr;
SceneManager::SceneLoadedCallback SceneManager::s_onSceneLoaded = nullptr;
std::string SceneManager::s_defaultScenePath;
std::string SceneManager::s_pendingScenePath;
uint64_t SceneManager::s_sceneGeneration = 0;

void SceneManager::SetActiveScene(Engine::Scene::Scene* scene)
{
    if (s_activeScene)
        s_activeScene->AssertOwnerThread();
    if (scene)
        scene->AssertOwnerThread();
    SceneReader().Cancel();
    s_pendingScenePath.clear();
    s_activeScene = scene;
    ++s_sceneGeneration;
}

// ---------------------------------------------------------------------------
// SceneManager::LoadScene
// ---------------------------------------------------------------------------
bool SceneManager::LoadScene(const std::string& path)
{
    if (!s_activeScene)
        return false;
    s_activeScene->AssertOwnerThread();
    SceneReader().Cancel();

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
    {
        if (s_activeScene)
            s_activeScene->AssertOwnerThread();
        LoadSceneAsync(path, {});
    }
}

void SceneManager::CancelPendingSceneLoad()
{
    if (s_activeScene)
        s_activeScene->AssertOwnerThread();
    s_pendingScenePath.clear();
    SceneReader().Cancel();
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
    if (s_activeScene)
        s_activeScene->AssertOwnerThread();
    auto result = SceneReader().TakeCompleted();
    if (!result || result->generation != s_sceneGeneration || !s_activeScene ||
        result->sceneGeneration != s_activeScene->CaptureSceneGeneration())
        return false;
    Engine::Scene::Scene* loaded = nullptr;
    if (result->readSucceeded && s_activeScene->LoadFromValue(result->document))
    {
        s_activeScene->Start();
        loaded = s_activeScene;
        if (s_onSceneLoaded)
            s_onSceneLoaded(loaded);
    }
    if (result->callback)
        result->callback(loaded);
    return loaded != nullptr;
}

// ---------------------------------------------------------------------------
// SceneManager::LoadSceneAsync
// ---------------------------------------------------------------------------
void SceneManager::LoadSceneAsync(const std::string& path,
                                   std::function<void(Engine::Scene::Scene*)> onComplete)
{
    if (!s_activeScene || path.empty())
    {
        if (onComplete)
            onComplete(nullptr);
        return;
    }

    s_activeScene->AssertOwnerThread();
    s_pendingScenePath.clear();
    SceneReader().Submit(path, std::move(onComplete), s_sceneGeneration,
        s_activeScene->CaptureSceneGeneration());
}

}
