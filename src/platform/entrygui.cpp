//-----------------------------------------------------------------------------
// Our main() function for the graphical interface.
//
// Copyright 2018 <whitequark@whitequark.org>
//-----------------------------------------------------------------------------
#include "solvespace.h"
#if defined(HAVE_MCP)
#   include "mcp/mcp.h"
#endif
#if defined(WIN32)
#   include <windows.h>
#endif

using namespace SolveSpace;

int main(int argc, char** argv) {
    std::vector<std::string> args = Platform::InitGui(argc, argv);
#if defined(HAVE_MCP)
    uint16_t mcpPort = Mcp::PortFromArgs(&args);
#endif

    Platform::Open3DConnexion();
    SS.Init();
#if defined(HAVE_MCP)
    if(mcpPort != 0) {
        Mcp::StartServer(mcpPort);
    }
#endif

    if(args.size() >= 2) {
        if(args.size() > 2) {
            dbp("Only the first file passed on command line will be opened.");
        }

        SS.Load(Platform::Path::From(args.back()));
    }

    Platform::RunGui();

#if defined(HAVE_MCP)
    Mcp::StopServer();
#endif

    Platform::Close3DConnexion();
    SS.Clear();
    SK.Clear();
    Platform::ClearGui();

    return 0;
}

#if defined(WIN32)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, INT nCmdShow) {
    return main(0, NULL);
}
#endif
