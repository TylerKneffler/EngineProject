#include "VideoExporter/VideoExportApplication.h"

int wmain()
{
    return Engine::Video::VideoExportApplication::Run(GetModuleHandleW(nullptr));
}
