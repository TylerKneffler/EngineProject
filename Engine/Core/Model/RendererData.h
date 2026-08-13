#pragma once

#include <string>

struct RendererOption
{
    std::string name;
    bool available = false;
    std::string unavailableReason;
};
