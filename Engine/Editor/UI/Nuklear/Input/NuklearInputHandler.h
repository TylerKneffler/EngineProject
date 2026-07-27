#pragma once

// Nuklear native-input feature.

#include <cstdint>

struct nk_context;

// Translates native Win32 keyboard and pointer messages into Nuklear input.
class NuklearInputHandler
{
public:
    bool HandleMessage(nk_context& context, void* nativeWindow,
        uint32_t message, uintptr_t wParam, intptr_t lParam) const;
};
