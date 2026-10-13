#pragma once
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace Engine::Model::MeshPackAsset
{
// Opens an ordinary asset, or its entry in an adjacent mesh pack if extracted
// files are absent. The returned stream is positioned at the entry payload.
std::unique_ptr<std::ifstream> Open(const std::filesystem::path& asset);
bool PackRig(const std::filesystem::path& rig, std::string& error);
bool ExtractRig(const std::filesystem::path& rig, std::string& error);
}
