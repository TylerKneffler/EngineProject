#include "Core/Model/MeshPackAsset.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main()
{
    namespace fs = std::filesystem;
    namespace pack = Engine::Model::MeshPackAsset;
    const auto root = fs::temp_directory_path() / "engine_mesh_pack_round_trip";
    fs::remove_all(root);
    fs::create_directories(root / "Meshes");
    const auto rig = root / "Sample.rig";
    const auto mesh = root / "Meshes" / "Body.mesh";
    const auto morph = root / "Meshes" / "Body.morph";
    { std::ofstream(rig) << "rig"; }
    { std::ofstream out(mesh, std::ios::binary); out.write("mesh\0payload", 12); }
    { std::ofstream(morph, std::ios::binary) << "morph-payload"; }
    std::string error;
    if (!pack::PackRig(rig, error) || fs::exists(mesh) || fs::exists(morph))
    { std::cerr << "Pack failed: " << error; return 1; }
    auto packedMesh = pack::Open(mesh);
    auto packedMorph = pack::Open(morph);
    if (!packedMesh || !packedMorph)
    { std::cerr << "Packed entries are missing"; return 2; }
    char meshBytes[12]{}, morphBytes[13]{};
    packedMesh->read(meshBytes, 12);
    packedMorph->read(morphBytes, 13);
    if (std::string(meshBytes, 12) != std::string("mesh\0payload", 12) ||
        std::string(morphBytes, 13) != "morph-payload")
    { std::cerr << "Packed read failed"; return 2; }
    packedMesh.reset();
    packedMorph.reset();
    if (!pack::ExtractRig(rig, error) || !fs::exists(mesh) ||
        !fs::exists(morph) || fs::exists(root / "Sample.meshpack"))
    { std::cerr << "Extract failed: " << error; return 3; }
    std::ifstream restoredMesh(mesh, std::ios::binary);
    std::ifstream restoredMorph(morph, std::ios::binary);
    if (std::string(std::istreambuf_iterator<char>(restoredMesh), {}) !=
            std::string("mesh\0payload", 12) ||
        std::string(std::istreambuf_iterator<char>(restoredMorph), {}) !=
            "morph-payload")
    { std::cerr << "Extracted bytes changed"; return 4; }
    restoredMesh.close();
    restoredMorph.close();
    fs::remove_all(root);
    return 0;
}
