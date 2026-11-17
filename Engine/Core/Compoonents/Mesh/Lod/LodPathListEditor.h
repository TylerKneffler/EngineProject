#pragma once

#include "Engine/Editor/UI/IEditorUi.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace Engine::Components
{
// Inspector rows for a per-level asset list (Mesh.lodMeshes,
// Material.lodMaterials). Row i edits LOD i + 1; an empty row is allowed and
// keeps its meaning to the owning component.
inline bool EditLodPathList(::Engine::Editor::IEditorUi& ui, const char* header,
    std::vector<std::string>& paths, size_t maximum, const char* emptyHint)
{
    bool changed = false;
    if (!ui.PropertyGroupHeader(header, false)) return false;
    ui.PushId(header);
    if (paths.empty()) ui.DisabledLabel("No additional levels");
    else ui.DisabledLabel(emptyHint);
    for (size_t index = 0; index < paths.size(); ++index)
    {
        ui.PushId((std::string("lod") + std::to_string(index + 1)).c_str());
        char buffer[1024]{};
        std::memcpy(buffer, paths[index].data(),
            std::min(paths[index].size(), sizeof(buffer) - 1));
        const std::string label = "LOD " + std::to_string(index + 1);
        if (ui.InputText(label.c_str(), buffer, sizeof(buffer)))
        {
            paths[index] = buffer;
            changed = true;
        }
        ui.PopId();
    }
    if (paths.size() < maximum && ui.Button("Add level"))
    {
        paths.emplace_back();
        changed = true;
    }
    if (!paths.empty())
    {
        ui.SameLine();
        if (ui.Button("Remove last level"))
        {
            paths.pop_back();
            changed = true;
        }
    }
    ui.PopId();
    return changed;
}
}
