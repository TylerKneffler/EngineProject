#include "Editor/Core/View/Views/AssetLocationIndex.h"
#include "Core/AssetRecord.h"
#include <Windows.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <crtdbg.h>

namespace fs = std::filesystem;
using Engine::Editor::AssetLocationIndex;

int main()
{
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    const fs::path root = fs::temp_directory_path() /
        ("AssetLocationIndexTests-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(root / "A");
    fs::create_directories(root / "B");
    const fs::path asset = root / "A" / "texture.png";
    std::ofstream(asset).put('x');
    Engine::Core::AssetRecord::Ensure(asset, asset);
    std::ofstream(root / "A" / "scene.scene").put('x');

    AssetLocationIndex index;
    index.Open(root);
    fs::rename(asset, root / "B" / "texture.png");
    auto moves = index.Detect();
    assert(moves.size() == 1);
    assert(moves[0].oldPath == "A/texture.png");
    assert(moves[0].newPath == "B/texture.png");
    index.Accept();

    AssetLocationIndex reopened;
    reopened.Open(root);
    fs::rename(root / "A", root / "C");
    moves = reopened.Detect();
    assert(moves.size() == 1 && moves[0].directory);
    assert(moves[0].oldPath == "A" && moves[0].newPath == "C");
    reopened.Accept();

    fs::rename(root / "C", root / "D");
    reopened.RecordKnownMove(root / "C", root / "D");
    moves = reopened.Detect();
    for (const auto& move : moves)
        std::cerr << "unexpected: " << move.oldPath << " -> "
            << move.newPath << '\n';
    if (!moves.empty()) return 1;

    // Copy + delete gives the file a new filesystem identity, as a move to
    // another volume would. Its sidecar keeps the stable asset identity.
    const fs::path crossVolumeSource = root / "B" / "source.asset";
    const fs::path crossVolumeTarget = root / "D" / "renamed.asset";
    std::ofstream(crossVolumeSource).put('y');
    Engine::Core::AssetRecord::Ensure(crossVolumeSource, crossVolumeSource);
    reopened.Detect();
    fs::copy_file(crossVolumeSource, crossVolumeTarget);
    fs::copy_file(Engine::Core::AssetRecord::SidecarPath(crossVolumeSource),
        Engine::Core::AssetRecord::SidecarPath(crossVolumeTarget));
    fs::remove(crossVolumeSource);
    fs::remove(Engine::Core::AssetRecord::SidecarPath(crossVolumeSource));
    moves = reopened.Detect();
    assert(moves.size() == 1);
    assert(moves[0].oldPath == "B/source.asset");
    assert(moves[0].newPath == "D/renamed.asset");
    reopened.Accept();

    const fs::path unpairedSource = root / "B" / "unpaired.asset";
    const fs::path unpairedTarget = root / "D" / "unpaired.asset";
    std::ofstream(unpairedSource).put('z');
    reopened.Detect();
    fs::copy_file(unpairedSource, unpairedTarget);
    fs::remove(unpairedSource);
    assert(reopened.Detect().empty());
    fs::remove_all(root);
}
