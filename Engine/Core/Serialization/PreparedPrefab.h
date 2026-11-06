#pragma once
#include "Core/Serialization/Json.h"
#include "Core/Component.h"
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <string>

namespace Engine::Serialization
{
// CPU-only, single-consumer placement payload. No live scene or GPU resources
// cross the worker boundary. Script components are constructed on publication.
struct PreparedPrefab
{
    std::string path;
    JsonValue document;
    std::map<std::string, JsonValue> dependencies;
    std::map<std::string, std::deque<std::unique_ptr<Engine::Core::Component>>> components;

    static std::unique_ptr<PreparedPrefab> Load(const std::string& path,
        const std::shared_ptr<std::atomic_bool>& canceled);
    static std::string PathKey(const std::string& path);
    static Engine::Core::Component* TakeComponent(const JsonValue& value);
    static const JsonValue* FindDocument(const std::string& path);
    // Transfer transient worker documents into the owner's prefab cache rather
    // than copying their complete hierarchy during publication.
    static JsonValue CopyOrConsumeDocument(const JsonValue& value);
    class Publication
    {
        PreparedPrefab* previous;
    public:
        explicit Publication(PreparedPrefab& prepared);
        ~Publication();
    };
private:
    inline static thread_local PreparedPrefab* current = nullptr;
};
}
