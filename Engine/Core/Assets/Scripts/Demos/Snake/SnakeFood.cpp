#include "Scripts/Demos/Snake/SnakeFood.h"

#include "Core/Object.h"
#include "Core/Serialization/SceneSerializer.h"
#include <algorithm>
#include <cmath>

SnakeFood::SnakeFood()
{
    SetTypeName(COMPONENT_TYPE_NAME(SnakeFood));
    RegisterField("minimumX", minimumX);
    RegisterField("maximumX", maximumX);
    RegisterField("minimumY", minimumY);
    RegisterField("maximumY", maximumY);
    RegisterField("cellSize", cellSize);
}

namespace
{
struct SnakeFoodRegistration
{
    SnakeFoodRegistration()
    {
        Engine::Serialization::RegisterComponentType<SnakeFood>("SnakeFood");
    }
};
SnakeFoodRegistration g_registration;
}

void SnakeFood::Start()
{
    if (Owner)
        Owner->enabled = true;
}

bool SnakeFood::Respawn(const std::vector<glm::ivec2>& blockedCells)
{
    if (!Owner)
        return false;
    const int width = std::max(1, maximumX - minimumX + 1);
    const int height = std::max(1, maximumY - minimumY + 1);
    const int cellCount = width * height;
    m_randomState = m_randomState * 1664525u + 1013904223u;
    const int start = static_cast<int>(m_randomState %
        static_cast<uint32_t>(cellCount));
    for (int offset = 0; offset < cellCount; ++offset)
    {
        const int index = (start + offset) % cellCount;
        const glm::ivec2 cell(minimumX + index % width,
            minimumY + index / width);
        if (IsBlocked(cell, blockedCells))
            continue;
        Owner->transform.position.x = static_cast<float>(cell.x) * cellSize;
        Owner->transform.position.y = static_cast<float>(cell.y) * cellSize;
        Owner->transform.scale.x = cellSize * 0.72f;
        Owner->transform.scale.y = cellSize * 0.72f;
        Owner->enabled = true;
        return true;
    }
    Owner->enabled = false;
    return false;
}

glm::ivec2 SnakeFood::GetGridPosition() const
{
    if (!Owner)
        return glm::ivec2(0);
    const float safeCellSize = std::max(0.01f, cellSize);
    return glm::ivec2(
        static_cast<int>(std::round(Owner->transform.position.x / safeCellSize)),
        static_cast<int>(std::round(Owner->transform.position.y / safeCellSize)));
}

bool SnakeFood::IsBlocked(const glm::ivec2& cell,
    const std::vector<glm::ivec2>& blockedCells) const
{
    return std::find(blockedCells.begin(), blockedCells.end(), cell) !=
        blockedCells.end();
}
