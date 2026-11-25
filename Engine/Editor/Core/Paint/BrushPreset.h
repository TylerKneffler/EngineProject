#pragma once
// The brush asset (.brush) lives in the engine core (Core/Brushes), where the
// terrain tools and scripts read it too; the painting tools keep their names.
#include "Core/Brushes/BrushPreset.h"

namespace Engine::Editor::Paint
{
using Engine::Brushes::BrushPreset;
using Engine::Brushes::SaveBrushPreset;
using Engine::Brushes::LoadBrushPreset;
}
