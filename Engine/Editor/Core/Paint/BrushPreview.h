#pragma once

#include "BrushPreset.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace Engine::Editor::Paint
{
inline void DrawBrushPreview(const BrushPreset& brush, float side = 72.f)
{
    ImGui::PushID(&brush);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##BrushPreview", { side, side });
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, { origin.x + side, origin.y + side },
        IM_COL32(48, 48, 48, 255));
    const float radius = side * .43f;
    const float hardness = std::clamp(brush.hardness, 0.f, 1.f);
    constexpr int cells = 32;
    const float cellSize = side / cells;
    for (int y = 0; y < cells; ++y)
    for (int x = 0; x < cells; ++x)
    {
        const float px = (x + .5f) * cellSize - side * .5f;
        const float py = (y + .5f) * cellSize - side * .5f;
        const float angle = brush.rotation * .01745329252f;
        const float rotatedX = px * std::cos(angle) + py * std::sin(angle);
        const float rotatedY = py * std::cos(angle) - px * std::sin(angle);
        const float distance = (brush.shape == 1
            ? std::max(std::abs(rotatedX), std::abs(rotatedY))
            : std::hypot(px, py)) / radius;
        if (distance > 1.f) continue;
        float strength = distance <= hardness ? 1.f :
            hardness >= 1.f ? 0.f :
            (1.f - distance) / (1.f - hardness);
        if (brush.falloff == 0) strength = distance <= hardness ? 1.f : 0.f;
        else if (brush.falloff == 2)
            strength = strength * strength * (3.f - 2.f * strength);
        const int alpha = static_cast<int>(std::lround(255.f * strength));
        draw->AddRectFilled({ origin.x + x * cellSize,
                origin.y + y * cellSize },
            { origin.x + (x + 1) * cellSize,
                origin.y + (y + 1) * cellSize },
            IM_COL32(245, 245, 245, alpha));
    }
    draw->AddRect(origin, { origin.x + side, origin.y + side },
        IM_COL32(130, 130, 130, 255));
    if (!brush.tipPath.empty())
        draw->AddText({ origin.x + 5.f, origin.y + side - 17.f },
            IM_COL32(255, 188, 80, 255), "TIP");
    ImGui::PopID();
}
}
