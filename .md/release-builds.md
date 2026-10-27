# Engine release builds

Each `v<version>` folder is a self-contained Windows x64 engine release. Run
`Editor.exe` from inside its version folder. The packaged editor and sandbox do
not require users to compile the engine first.

The matching `EngineProject-v<version>-windows-x64.zip` is the direct-download
artifact and contains that complete version folder.

The repository's `imgui.ini` is included so releases start with the maintained
editor docking layout. Users can still rearrange panels and save their own
layout normally.

Use the VS Code task **Engine: Package Versioned Release** to configure a
portable Release build and replace the matching version folder. Release folders
are intentionally tracked by Git so they can be downloaded directly.
