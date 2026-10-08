#pragma once

#include <deque>
#include <string>

namespace Engine::Editor
{
template<typename Restore>
bool StepSnapshotHistory(bool redo, std::deque<std::string>& undo,
    std::deque<std::string>& redoEntries, std::string& baseline,
    Restore&& restore)
{
    auto& source = redo ? redoEntries : undo;
    auto& destination = redo ? undo : redoEntries;
    if (source.empty()) return false;
    const std::string target = source.back();
    if (!restore(target)) return false;
    source.pop_back();
    destination.push_back(baseline);
    baseline = target;
    return true;
}
}
