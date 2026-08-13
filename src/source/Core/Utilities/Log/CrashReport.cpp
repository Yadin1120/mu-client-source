// CrashReport.cpp — ראה CrashReport.h.

#include "stdafx.h"

#ifdef _WIN32

#include "CrashReport.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>

#include "ErrorReport.h"

extern CErrorReport g_ErrorReport;

namespace
{
    // dbghelp נטען דינמית ולא בקישור סטטי: הוא נחוץ רק ברגע קריסה, ואם הוא
    // חסר במחשב של השחקן עדיף שהמשחק ייפול בלי דאמפ מאשר שלא יעלה בכלל.
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(
        HANDLE hProcess,
        DWORD processId,
        HANDLE hFile,
        int dumpType,
        void* exceptionParam,
        void* userStreamParam,
        void* callbackParam);

    struct MiniDumpExceptionInformation
    {
        DWORD ThreadId;
        EXCEPTION_POINTERS* ExceptionPointers;
        BOOL ClientPointers;
    };

    /// כתובת מוחלטת → "שם המודול + היסט בתוכו". ההיסט הוא מה שאפשר לפענח
    /// מול ה-pdb; הכתובת המוחלטת חסרת ערך כי ASLR מזיז את המודול בכל הרצה.
    void DescribeAddress(void* address, wchar_t* out, size_t outCount)
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(address),
                &module)
            || module == nullptr)
        {
            _snwprintf_s(out, outCount, _TRUNCATE, L"<unknown module> at 0x%p", address);
            return;
        }

        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = (name != nullptr) ? name + 1 : path;

        const auto offset = static_cast<size_t>(
            reinterpret_cast<unsigned char*>(address) - reinterpret_cast<unsigned char*>(module));
        _snwprintf_s(out, outCount, _TRUNCATE, L"%ls+0x%zX", name, offset);
    }

    void BuildDumpPath(wchar_t* out, size_t outCount)
    {
        wchar_t exePath[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash != nullptr)
        {
            *(lastSlash + 1) = L'\0';
        }
        else
        {
            exePath[0] = L'\0';
        }

        SYSTEMTIME now;
        GetLocalTime(&now);
        _snwprintf_s(
            out, outCount, _TRUNCATE,
            L"%lsMuCrash-%04d%02d%02d-%02d%02d%02d.dmp",
            exePath, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    }

    bool WriteMiniDump(EXCEPTION_POINTERS* pointers, const wchar_t* path)
    {
        const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        if (dbghelp == nullptr)
        {
            return false;
        }

        const auto writeDump =
            reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        if (writeDump == nullptr)
        {
            FreeLibrary(dbghelp);
            return false;
        }

        const HANDLE file = CreateFileW(
            path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            FreeLibrary(dbghelp);
            return false;
        }

        MiniDumpExceptionInformation info{};
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = pointers;
        info.ClientPointers = FALSE;

        // MiniDumpNormal (0) בכוונה: מספיק למחסנית ולרשימת המודולים, ומייצר
        // קובץ של מאות קילובייטים ולא של מאות מגה כמו דאמפ עם כל הזיכרון —
        // שחקן לא ישלח קובץ ענק, וזו הסיבה שרוב מנגנוני הדיווח מתים בשקט.
        const BOOL ok = writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, 0, &info, nullptr, nullptr);

        CloseHandle(file);
        FreeLibrary(dbghelp);
        return ok != FALSE;
    }

    LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers)
    {
        // אין הקצאות דינמיות ואין קריאות מסובכות: אנחנו כבר בתוך קריסה, וכל
        // דבר שיכול לזרוק כאן פשוט ימחק את הדיווח שאנחנו מנסים לכתוב.
        static LONG alreadyHandling = 0;
        if (InterlockedExchange(&alreadyHandling, 1) != 0)
        {
            return EXCEPTION_EXECUTE_HANDLER;
        }

        wchar_t where[MAX_PATH + 64] = L"<no exception record>";
        DWORD code = 0;
        if (pointers != nullptr && pointers->ExceptionRecord != nullptr)
        {
            code = pointers->ExceptionRecord->ExceptionCode;
            DescribeAddress(pointers->ExceptionRecord->ExceptionAddress, where, _countof(where));
        }

        g_ErrorReport.Write(L"\r\n");
        g_ErrorReport.AddSeparator();
        g_ErrorReport.Write(L"CRASH: code 0x%08X at %ls\r\n", code, where);

        wchar_t dumpPath[MAX_PATH] = L"";
        BuildDumpPath(dumpPath, _countof(dumpPath));
        if (WriteMiniDump(pointers, dumpPath))
        {
            g_ErrorReport.Write(L"CRASH: minidump written to %ls\r\n", dumpPath);
        }
        else
        {
            g_ErrorReport.Write(L"CRASH: could not write a minidump\r\n");
        }

        g_ErrorReport.AddSeparator();

        // EXECUTE_HANDLER ולא CONTINUE_SEARCH: אחרת ווינדוס מציג את חלון
        // "התוכנית הפסיקה לפעול" ומנסה לשלוח דיווח לאף אחד, אחרי שכבר כתבנו
        // את מה שבאמת שימושי.
        return EXCEPTION_EXECUTE_HANDLER;
    }
}

namespace MuCrash
{
    void Install()
    {
        SetUnhandledExceptionFilter(OnUnhandledException);
    }
}

#endif
