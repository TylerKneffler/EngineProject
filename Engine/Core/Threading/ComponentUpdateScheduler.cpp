#include "Core/Threading/ComponentUpdateScheduler.h"
#include "Core/Threading/JobScheduler.h"
#include <algorithm>
#include <stdexcept>
#include <typeindex>

namespace Engine::Threading
{
namespace
{
bool Intersects(const std::vector<std::string>& first,
    const std::vector<std::string>& second)
{
    for (const auto& name : first)
        if (std::find(second.begin(), second.end(), name) != second.end())
            return true;
    return false;
}

bool Conflicts(const Core::Component::UpdateAccess& first,
    const Core::Component::UpdateAccess& second)
{
    return Intersects(first.writes, second.reads) ||
        Intersects(first.writes, second.writes) ||
        Intersects(first.reads, second.writes);
}
}

void ComponentUpdateScheduler::Run(const std::vector<Node>& nodes)
{
    const size_t count = nodes.size();
    if (count == 0) return;
    for (const auto& node : nodes)
        if (!node.component)
            throw std::invalid_argument("Component update node is null");
    std::vector<std::vector<size_t>> dependencies(count);
    for (size_t current = 0; current < count; ++current)
    {
        for (size_t other = 0; other < count; ++other)
        {
            if (current == other) continue;
            // Conflicting access keeps the former serial order. Explicit
            // prerequisites can also point to a later node in the segment.
            const bool accessConflict = other < current &&
                Conflicts(nodes[other].access, nodes[current].access);
            const auto& after = nodes[current].access.after;
            const bool prerequisite = std::find(after.begin(), after.end(),
                std::type_index(typeid(*nodes[other].component))) != after.end();
            if (accessConflict || prerequisite)
                dependencies[current].push_back(other);
        }
    }

    // Validate the whole graph before dispatching any updates.
    std::vector<size_t> order;
    std::vector<unsigned char> state(count, 0);
    const auto visit = [&](const auto& self, size_t index) -> void
    {
        if (state[index] == 2) return;
        if (state[index] == 1)
            throw std::logic_error("Component update dependency cycle");
        state[index] = 1;
        for (size_t dependency : dependencies[index]) self(self, dependency);
        state[index] = 2;
        order.push_back(index);
    };
    for (size_t index = 0; index < count; ++index) visit(visit, index);

    auto& scheduler = JobScheduler::Get();
    std::vector<JobScheduler::Handle> handles(count);
    std::exception_ptr failure;
    try
    {
        for (size_t index : order)
        {
            std::vector<JobScheduler::Handle> prior;
            prior.reserve(dependencies[index].size());
            for (size_t dependency : dependencies[index])
                prior.push_back(handles[dependency]);
            handles[index] = scheduler.Schedule(
                [component = nodes[index].component] { component->Update(); },
                prior, false);
        }
    }
    catch (...) { failure = std::current_exception(); }
    // All captured component pointers must be released before the scene can
    // mutate again, even if scheduling or one update failed.
    for (const auto& handle : handles)
        try { scheduler.Wait(handle); }
        catch (...) { if (!failure) failure = std::current_exception(); }
    if (failure) std::rethrow_exception(failure);
    for (const auto& node : nodes)
        node.component->PublishUpdateOnOwner();
}
}
