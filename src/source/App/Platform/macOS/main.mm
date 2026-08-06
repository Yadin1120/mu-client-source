// macOS entry point (the per-platform entry seam from issue #441; the APPLE
// branch in src/CMakeLists.txt selects this file).
//
// Same contract as the Linux entry (source/App/Platform/Linux/main.cpp): the
// bootstrap and game loop live in Winmain.cpp, whose WinMain compiles off
// Windows as a plain function, so this only forwards into it. The HINSTANCE and
// command-line parameters are Win32 artifacts the portable path does not read.
//
// Objective-C++ rather than plain C++ so the run sits inside an autorelease
// pool. SDL3 creates the NSApplication itself during SDL_Init, but the Cocoa
// objects it autoreleases while starting up have no pool of their own when
// main() is a bare C++ function, and AppKit logs about it on every launch.
#include "Core/Platform/WinCompat.h"

#import <Foundation/Foundation.h>

int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdLine, int nCmdShow);

int main(int /*argc*/, char* /*argv*/[])
{
    @autoreleasepool
    {
        return WinMain(nullptr, nullptr, nullptr, SW_SHOW);
    }
}
