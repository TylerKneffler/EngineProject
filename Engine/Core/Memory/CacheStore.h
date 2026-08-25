#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace Engine::Memory
{
enum class CacheLifetime
{
    ShortTerm,
    LongTerm
};

struct CacheStats
{
    size_t shortTermEntries = 0;
    size_t longTermEntries = 0;
};

// Process-wide typed storage for reusable resources and disposable derived
// data. Long-term entries live until explicitly invalidated; short-term entries
// expire after they stop being accessed and can also be cleared by domain.
class CacheStore final
{
public:
    using Duration = std::chrono::steady_clock::duration;

    static CacheStore& Get();
    static std::string PathKey(const std::string& path,
        const std::string& variant = {});

    template<typename T>
    std::shared_ptr<T> Find(const std::string& domain,
        const std::string& key)
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(m_mutex);
        MaybePruneLocked(now);
        auto found = m_entries.find(CombinedKey(domain, key));
        if (found == m_entries.end() || found->second.type != std::type_index(typeid(T)))
            return nullptr;
        if (IsExpired(found->second, now))
        {
            m_entries.erase(found);
            return nullptr;
        }
        found->second.lastAccess = now;
        return std::static_pointer_cast<T>(found->second.value);
    }

    template<typename T, typename Factory>
    std::shared_ptr<T> GetOrCreate(CacheLifetime lifetime,
        const std::string& domain, const std::string& key, Factory&& factory,
        Duration shortTermRetention = std::chrono::seconds(60))
    {
        if (std::shared_ptr<T> existing = Find<T>(domain, key))
            return existing;

        std::shared_ptr<T> created = std::forward<Factory>(factory)();
        if (!created)
            return nullptr;

        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::string combined = CombinedKey(domain, key);
        auto found = m_entries.find(combined);
        if (found != m_entries.end() && !IsExpired(found->second, now) &&
            found->second.type == std::type_index(typeid(T)))
        {
            found->second.lastAccess = now;
            return std::static_pointer_cast<T>(found->second.value);
        }

        Entry entry;
        entry.value = created;
        entry.type = std::type_index(typeid(T));
        entry.lifetime = lifetime;
        entry.lastAccess = now;
        entry.retention = shortTermRetention;
        m_entries[combined] = std::move(entry);
        return created;
    }

    bool Erase(const std::string& domain, const std::string& key);
    size_t ClearDomain(const std::string& domain);
    size_t Clear(CacheLifetime lifetime);
    size_t PruneShortTerm();
    CacheStats GetStats() const;

private:
    struct Entry
    {
        std::shared_ptr<void> value;
        std::type_index type { typeid(void) };
        CacheLifetime lifetime = CacheLifetime::ShortTerm;
        std::chrono::steady_clock::time_point lastAccess{};
        Duration retention = std::chrono::seconds(60);
    };

    static std::string CombinedKey(const std::string& domain,
        const std::string& key);
    static bool IsExpired(const Entry& entry,
        std::chrono::steady_clock::time_point now);
    void MaybePruneLocked(std::chrono::steady_clock::time_point now);
    size_t PruneShortTermLocked(std::chrono::steady_clock::time_point now);

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, Entry> m_entries;
    size_t m_operationsUntilPrune = 256;
};
}
