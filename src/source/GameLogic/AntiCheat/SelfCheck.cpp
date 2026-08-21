// SelfCheck.cpp: the client-side security self check. See SelfCheck.h for what
// this is and, more importantly, what it is not.
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "SelfCheck.h"

#ifdef _WIN32
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <string>

#include "I18N/All.h"

// Defined in App/Platform/Windows/Winmain.cpp, the same way WSclient.cpp reaches it.
BOOL Util_CheckOption(std::wstring lpszCommandLine, wchar_t cOption, std::wstring& lpszString);

namespace GameLogic::AntiCheat
{
    namespace
    {
        // The command line switch through which the launcher hands over its
        // one-time secret: Main.exe connect /u<ip> /p<port> /t<token>.
        constexpr wchar_t LAUNCH_TOKEN_OPTION = L't';

        constexpr size_t MAX_TOKEN_LENGTH = 64;

        // Executable names of tools whose only realistic use next to a game
        // client is memory editing or debugging. Deliberately short: a wide net
        // here means accusing honest players, and the cost of a false accusation
        // is higher than the cost of missing a cheater.
        //
        // Not listed on purpose: packet sniffers, process explorers and other
        // dual-use utilities. Plenty of people run those for ordinary reasons.
        const wchar_t* const KNOWN_CHEAT_TOOLS[] =
        {
            L"cheatengine-x86_64.exe",
            L"cheatengine-i386.exe",
            L"cheatengine.exe",
            L"artmoney.exe",
            L"ollydbg.exe",
            L"x64dbg.exe",
            L"x32dbg.exe",
            L"xenos.exe",
            L"xenos64.exe",
            L"extremeinjector.exe",
            L"winject.exe",
            L"mhs.exe",
        };

        Report& MutableReport()
        {
            static Report s_report{};
            return s_report;
        }

        std::wstring ToLower(const std::wstring& text)
        {
            std::wstring lowered = text;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                [](wchar_t c) { return static_cast<wchar_t>(towlower(static_cast<wint_t>(c))); });
            return lowered;
        }

        void SetResult(Check& check, CheckStatus eStatus, std::wstring strFinding = L"")
        {
            check.eStatus = eStatus;
            check.strFinding = std::move(strFinding);
        }

#ifdef _WIN32
        // A debugger attached to the game is how memory edits are usually made.
        void CheckEnvironment(Check& check)
        {
            if (::IsDebuggerPresent())
            {
                SetResult(check, CheckStatus::Suspicious, I18N::Game::ADebuggerIsAttachedToTheGame);
                return;
            }

            BOOL bRemoteDebugger = FALSE;
            if (::CheckRemoteDebuggerPresent(::GetCurrentProcess(), &bRemoteDebugger) && bRemoteDebugger)
            {
                SetResult(check, CheckStatus::Suspicious, I18N::Game::AnExternalDebuggerIsAttachedToTheGame);
                return;
            }

            SetResult(check, CheckStatus::Passed);
        }

        void CheckProcesses(Check& check)
        {
            HANDLE hSnapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (hSnapshot == INVALID_HANDLE_VALUE)
            {
                SetResult(check, CheckStatus::Skipped);
                return;
            }

            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            std::wstring strFound;
            if (::Process32FirstW(hSnapshot, &entry))
            {
                do
                {
                    const std::wstring strName = ToLower(entry.szExeFile);
                    for (const wchar_t* const lpszTool : KNOWN_CHEAT_TOOLS)
                    {
                        if (strName != lpszTool)
                        {
                            continue;
                        }

                        if (!strFound.empty())
                        {
                            strFound += L", ";
                        }

                        strFound += entry.szExeFile;
                        break;
                    }
                }
                while (::Process32NextW(hSnapshot, &entry));
            }

            ::CloseHandle(hSnapshot);

            if (strFound.empty())
            {
                SetResult(check, CheckStatus::Passed);
                return;
            }

            SetResult(check, CheckStatus::Suspicious, std::wstring(I18N::Game::ProgramsRunning) + strFound);
        }

        bool IsPathUnder(const std::wstring& strPath, const std::wstring& strFolder)
        {
            if (strFolder.empty() || strPath.size() < strFolder.size())
            {
                return false;
            }

            return ToLower(strPath).compare(0, strFolder.size(), ToLower(strFolder)) == 0;
        }

        std::wstring GetFolderOfCurrentProcess()
        {
            wchar_t szPath[MAX_PATH]{};
            if (::GetModuleFileNameW(nullptr, szPath, MAX_PATH) == 0)
            {
                return L"";
            }

            std::wstring strPath = szPath;
            const size_t lastSeparator = strPath.find_last_of(L"\\/");
            return lastSeparator == std::wstring::npos ? L"" : strPath.substr(0, lastSeparator);
        }

        // Looks for code loaded into the game from somewhere it has no business
        // being. Overlays from chat and driver software legitimately inject into
        // games, so a module merely living outside the game folder proves nothing;
        // only a module running from a temporary folder, or one with no file on
        // disk at all, is reported.
        void CheckIntegrity(Check& check)
        {
            HANDLE hSnapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
            if (hSnapshot == INVALID_HANDLE_VALUE)
            {
                SetResult(check, CheckStatus::Skipped);
                return;
            }

            wchar_t szTempFolder[MAX_PATH]{};
            const DWORD dwTempLength = ::GetTempPathW(MAX_PATH, szTempFolder);
            const std::wstring strTempFolder = dwTempLength > 0 ? std::wstring(szTempFolder) : L"";

            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            std::wstring strFound;
            if (::Module32FirstW(hSnapshot, &entry))
            {
                do
                {
                    const std::wstring strPath = entry.szExePath;
                    const bool bFromTemp = !strTempFolder.empty() && IsPathUnder(strPath, strTempFolder);
                    if (!strPath.empty() && !bFromTemp)
                    {
                        continue;
                    }

                    if (!strFound.empty())
                    {
                        strFound += L", ";
                    }

                    strFound += entry.szModule;
                }
                while (::Module32NextW(hSnapshot, &entry));
            }

            ::CloseHandle(hSnapshot);

            if (strFound.empty())
            {
                SetResult(check, CheckStatus::Passed);
                return;
            }

            SetResult(check, CheckStatus::Suspicious, std::wstring(I18N::Game::CodeLoadedIntoTheGameFromATemporaryFolder) + strFound);
        }
#endif

        void ReadLaunchToken(Report& report)
        {
#ifdef _WIN32
            std::wstring strToken;
            if (Util_CheckOption(::GetCommandLineW(), LAUNCH_TOKEN_OPTION, strToken)
                && strToken.size() <= MAX_TOKEN_LENGTH)
            {
                report.strLaunchToken = strToken;
            }
#else
            (void)report;
#endif
        }
    }

    void Run()
    {
        Report& report = MutableReport();
        report.Checks.clear();
        report.Checks.push_back({ CheckId::Environment, CheckStatus::Pending, I18N::Game::SystemEnvironment, L"" });
        report.Checks.push_back({ CheckId::Processes, CheckStatus::Pending, I18N::Game::RunningPrograms, L"" });
        report.Checks.push_back({ CheckId::Integrity, CheckStatus::Pending, I18N::Game::GameIntegrity, L"" });

        ReadLaunchToken(report);

#ifdef _WIN32
        CheckEnvironment(report.Checks[static_cast<size_t>(CheckId::Environment)]);
        CheckProcesses(report.Checks[static_cast<size_t>(CheckId::Processes)]);
        CheckIntegrity(report.Checks[static_cast<size_t>(CheckId::Integrity)]);
#else
        for (Check& check : report.Checks)
        {
            SetResult(check, CheckStatus::Skipped);
        }
#endif

        report.bCompleted = true;
    }

    const Report& GetReport()
    {
        return MutableReport();
    }

    bool IsFinished()
    {
        return MutableReport().bCompleted;
    }

    int GetSuspiciousCount()
    {
        int iCount = 0;
        for (const Check& check : MutableReport().Checks)
        {
            if (check.eStatus == CheckStatus::Suspicious)
            {
                iCount++;
            }
        }

        return iCount;
    }

    std::wstring BuildFindingSummary()
    {
        std::wstring strSummary;
        for (const Check& check : MutableReport().Checks)
        {
            if (check.strFinding.empty())
            {
                continue;
            }

            if (!strSummary.empty())
            {
                strSummary += L" · ";
            }

            strSummary += check.strFinding;
        }

        return strSummary;
    }
}
