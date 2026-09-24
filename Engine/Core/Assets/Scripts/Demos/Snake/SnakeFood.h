#pragma once

#include "Core/Script.h"
#include "Core/PropertyMacros.h"
#include <glm/glm.hpp>
#include <vector>

class SnakeFood final : public Engine::Core::Script
{
public:
    SnakeFood();

    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Board")
    int minimumX = -7;
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Board")
    int maximumX = 7;
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Board")
    int minimumY = -3;
    PROPERTY(Inspector, EditAnywhere, Category = "Snake | Board")
    int maximumY = 3;

    void Start() override;
    bool Respawn(const std::vector<glm::ivec2>& blockedCells);
    glm::ivec2 GetGridPosition() const;

private:
    bool IsBlocked(const glm::ivec2& cell,
        const std::vector<glm::ivec2>& blockedCells) const;

    uint32_t m_randomState = 0x534E414Bu;
};
