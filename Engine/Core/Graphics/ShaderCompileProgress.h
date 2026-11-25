#pragma once

#include <string>

namespace Engine::Graphics
{
// Shader compiles in progress, for loading screens (StartupSplash). The
// compilers report each compile that missed their cache (Begin/End, any
// thread); readers take a Snapshot. Each finished compile's duration is
// remembered per shader entry (the cache directory's compile_times.txt), so
// the next cold compile of that entry has an expected length to show.
class ShaderCompileProgress
{
public:
    struct Snapshot
    {
        int active = 0;             // compiles running now
        int finished = 0;           // compiles finished in this process
        std::string label;          // the longest-running active compile ("Object.hlsl PSMain")
        double elapsedSeconds = 0;  // how long that one has run
        double expectedSeconds = 0; // its last known duration (0: unknown)
        double idleSeconds = 0;     // since the last compile ended (with none active)
    };

    // Where durations are remembered (the shader cache directory); empty
    // keeps them in memory only. Set once by the compiler, before compiling.
    static void SetStoreDirectory(const std::string& directory);
    // Returns a ticket for End.
    static int Begin(const std::string& filePath, const char* entryPoint, const char* target);
    static void End(int ticket, bool succeeded);
    static Snapshot Get();
};
}
