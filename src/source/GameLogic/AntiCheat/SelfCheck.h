// SelfCheck.h: client-side security self check, run once before the player
// reaches the login screen. Looks for a debugger attached to the game, for
// code injected into the game process, and for known cheat tools running on
// the machine.
//
// What this is and is not: everything here runs on the player's own computer,
// which is hostile ground by definition - a patched client simply would not
// report. It is therefore a *signal*, never proof, and the server treats it as
// such. The checks that cannot be lied to live on the server (see the anti
// cheat subsystem there); these add cheap breadth on top.
//
// Nothing here blocks the player. Findings are reported to the server, which
// scores them, and a game master decides.
//////////////////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <vector>

namespace GameLogic::AntiCheat
{
    enum class CheckStatus : unsigned char
    {
        Pending = 0,
        Running = 1,
        Passed = 2,
        Suspicious = 3,
        Skipped = 4,
    };

    enum class CheckId : unsigned char
    {
        Environment = 0,    // a debugger attached to the game
        Processes = 1,      // known cheat tools running on the machine
        Integrity = 2,      // modules loaded into the game from outside its folder
        Count = 3,
    };

    struct Check
    {
        CheckId eId;
        CheckStatus eStatus;

        // Short Hebrew label, shown in the security window.
        const wchar_t* lpszLabel;

        // What was found, for the report to the server. Empty when nothing was.
        std::wstring strFinding;
    };

    // The whole outcome of one run.
    struct Report
    {
        bool bCompleted;
        std::vector<Check> Checks;

        // A one-time secret handed over by the launcher. Empty when the game
        // was started without it, which is exactly what the server wants to know.
        std::wstring strLaunchToken;
    };

    // Runs every check once. Cheap enough for startup (a few milliseconds) and
    // deliberately not repeated per frame.
    void Run();

    // The result of the last Run(). Safe to call before Run(): every check
    // reads back as Pending.
    const Report& GetReport();

    // True once every check has finished, i.e. the security window may close.
    bool IsFinished();

    // Number of checks that found something.
    int GetSuspiciousCount();

    // The findings joined into one line for the server report, or an empty
    // string when everything passed.
    std::wstring BuildFindingSummary();
}
