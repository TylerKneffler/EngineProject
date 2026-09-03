#pragma once

namespace Engine::Server
{
// Deliberately independent from the renderer and window systems. Networking
// and player-session ownership will be added here without changing scene
// simulation or its diagnostic protocol.
class ServerApplication
{
public:
    static int Run(int argc, char** argv);
};
}
