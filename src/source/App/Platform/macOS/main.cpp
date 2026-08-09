// macOS entry point (the per-platform entry seam from issue #441; the APPLE
// branch in src/CMakeLists.txt selects this file).
//
// Same contract as the Linux entry (source/App/Platform/Linux/main.cpp): the
// bootstrap and game loop live in Winmain.cpp, whose WinMain compiles off
// Windows as a plain function, so this only forwards into it.
//
// Plain C++, deliberately not Objective-C++: an earlier main.mm imported
// Foundation, whose <objc/objc.h> typedefs BOOL as `bool` and collides with
// the engine-wide `typedef int BOOL` in WinCompat.h. Nothing here needs Cocoa
// directly - SDL3 owns the NSApplication lifecycle and its own autorelease
// pools once SDL_Init runs.
#include "Core/Platform/WinCompat.h"

int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdLine, int nCmdShow);

int main(int /*argc*/, char* /*argv*/[])
{
    return WinMain(nullptr, nullptr, nullptr, SW_SHOW);
}
