#pragma once
#include "Core/Threading/JobScheduler.h"

namespace Engine::Rendering
{
// Rendering shares the engine CPU scheduler; existing callers remain compatible.
using RenderJobs=Engine::Threading::JobScheduler;
}
