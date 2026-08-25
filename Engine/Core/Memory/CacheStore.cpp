#include "Core/Memory/CacheStore.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace Engine::Memory
{
CacheStore& CacheStore::Get()
{
    static CacheStore store;
    return store;
}

std::string CacheStore::PathKey(const std::string& path,
    const std::string& variant)
{
    std::error_code error;
    std::filesystem::path identity = std::filesystem::absolute(
        std::filesystem::path(path).lexically_normal(), error);
    if (error)
        identity = std::filesystem::path(path).lexically_normal();
    std::string key = identity.generic_string();
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
#endif
    return variant.empty() ? key : key + '|' + variant;
}

std::string CacheStore::CombinedKey(const std::string& domain,
    const std::string& key)
{
    return domain + '\x1f' + key;
}

bool CacheStore::IsExpired(const Entry& entry,
    std::chrono::steady_clock::time_point now)
{
    return entry.lifetime == CacheLifetime::ShortTerm &&
        entry.retention != Duration::max() &&
        now - entry.lastAccess >= entry.retention;
}

void CacheStore::MaybePruneLocked(std::chrono::steady_clock::time_point now)
{
    if (--m_operationsUntilPrune == 0)
    {
        PruneShortTermLocked(now);
        m_operationsUntilPrune = 256;
    }
}

size_t CacheStore::PruneShortTermLocked(
    std::chrono::steady_clock::time_point now)
{
    size_t removed = 0;
    for (auto entry = m_entries.begin(); entry != m_entries.end();)
    {
        if (IsExpired(entry->second, now))
        {
            entry = m_entries.erase(entry);
            ++removed;
        }
        else
            ++entry;
    }
    return removed;
}

bool CacheStore::Erase(const std::string& domain, const std::string& key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.erase(CombinedKey(domain, key)) != 0;
}

size_t CacheStore::ClearDomain(const std::string& domain)
{
    const std::string prefix = domain + '\x1f';
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t removed = 0;
    for (auto entry = m_entries.begin(); entry != m_entries.end();)
    {
        if (entry->first.compare(0, prefix.size(), prefix) == 0)
        {
            entry = m_entries.erase(entry);
            ++removed;
        }
        else
            ++entry;
    }
    return removed;
}

size_t CacheStore::Clear(CacheLifetime lifetime)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t removed = 0;
    for (auto entry = m_entries.begin(); entry != m_entries.end();)
    {
        if (entry->second.lifetime == lifetime)
        {
            entry = m_entries.erase(entry);
            ++removed;
        }
        else
            ++entry;
    }
    return removed;
}

size_t CacheStore::PruneShortTerm()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return PruneShortTermLocked(std::chrono::steady_clock::now());
}

CacheStats CacheStore::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    CacheStats stats;
    for (const auto& entry : m_entries)
    {
        if (entry.second.lifetime == CacheLifetime::ShortTerm)
            ++stats.shortTermEntries;
        else
            ++stats.longTermEntries;
    }
    return stats;
}
}
