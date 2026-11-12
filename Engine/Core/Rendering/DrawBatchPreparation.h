#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Engine::Graphics { class IGraphicsTexture; }
namespace Engine::Rendering
{
// Identities only: preparation never dereferences graphics or component objects.
using DrawBatchKey=std::tuple<const void*,const void*,const void*,
    std::array<const Graphics::IGraphicsTexture*,30>,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,bool,int,
    std::vector<std::pair<uint32_t,uint32_t>>,std::array<const void*,3>,const void*>;
class DrawBatchPreparation
{
public:
    struct Input { DrawBatchKey key; uint32_t slot=0,index=0; };
    struct Group { std::vector<uint32_t> members; };
    struct Result { std::vector<Group> groups; bool reused=false; uint64_t reusedKeys=0; double buildMs=0; };
    // Single owner, or one worker while the owner does independent work. Join
    // before reusing/destroying the cache or dereferencing result draw indexes.
    Result Prepare(std::vector<Input> input,bool allowReuse=true);
    std::size_t CachedSlotCount() const noexcept { return m_slots.size(); }
private:
    struct Compatibility { DrawBatchKey key; uint32_t references=0; };
    std::unordered_map<uint32_t,uint32_t> m_slots;
    std::map<DrawBatchKey,uint32_t> m_lookup;
    std::vector<Compatibility> m_compatibility;
    std::vector<uint32_t> m_free;
    // Store identities and draw indices, never pointers to live draw data.
    std::vector<std::pair<uint32_t,uint32_t>> m_lastOrder;
    std::vector<Group> m_lastGroups;
    bool m_hasLastGroups=false;
};
}
