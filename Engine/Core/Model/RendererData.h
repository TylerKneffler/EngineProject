#pragma once

#include <string>

namespace Engine::Model
{
struct RendererOption
{
    std::string name;
    bool available = false;
    std::string unavailableReason;
};
}

