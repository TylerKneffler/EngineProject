#pragma once

#include "Core/Serialization/Json.h"
#include "Core/Serialization/SceneSerializer.h"
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace Engine::Scene { class Scene; }

namespace Engine::Runtime
{
// Reads scene documents on one bounded worker thread. Only the latest request
// is kept; replaced or canceled requests never complete. Results are taken on
// the scene owner thread (Game::ProcessPendingSceneLoad).
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
}
