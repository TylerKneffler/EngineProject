#pragma once
#include <atomic>
#include <cstdint>

namespace Engine::Editor
{
// Editor actions can run inside input callbacks or UI drawing. Keep their tags
// until the frame recorder consumes them after submission.
class FrameActivity
{
public:
    enum : uint32_t { Import = 1, Save = 2, Bake = 4, Load = 8, PlayStart = 16, PlayStop = 32 };
    static void Mark(uint32_t activity) { Pending().fetch_or(activity, std::memory_order_relaxed); }
    static uint32_t Consume() { return Pending().exchange(0, std::memory_order_relaxed); }
private:
    static std::atomic<uint32_t>& Pending()
    {
        static std::atomic<uint32_t> pending{0};
        return pending;
    }
};
}
