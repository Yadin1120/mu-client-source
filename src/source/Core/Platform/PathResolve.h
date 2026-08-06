// Case-correcting path resolution for POSIX filesystems (issue #462, Phase 4).
//
// The engine's asset paths are Windows-style: backslash separators and a case
// that does not always match the files on disk, which a case-sensitive
// filesystem rejects. The file-open shims funnel paths through here, which
// flips the separators and, when a component does not exist verbatim, falls
// back to a case-insensitive directory scan for it.
#pragma once

#ifndef _WIN32

#include <cstddef>
#include <cstdint>
#include <string>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>   // _NSGetExecutablePath
#endif

// Absolute path of the running executable, empty when it cannot be determined.
// Linux reads the /proc symlink; macOS has no /proc and answers through the
// dyld API instead. Three places need this - the config file next to the
// executable, the bundled fonts, and the .NET client library - so it lives here
// rather than being spelled out (and forgotten) in each of them.
inline std::string MuExecutablePath()
{
    char buf[4096];
#if defined(__APPLE__)
    // _NSGetExecutablePath takes the buffer size in and reports the size it
    // needed back out through the same argument; a non-zero return means the
    // path did not fit.
    std::uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    return std::string(buf);
#else
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    return std::string(buf, static_cast<std::size_t>(n));
#endif
}

// Directory holding the running executable, with a trailing '/'. Empty when the
// executable path is unavailable. Callers resolve their own files against this
// instead of the working directory, which is not theirs to rely on - a macOS
// app launched from Finder starts in "/".
inline std::string MuExecutableDir()
{
    const std::string path = MuExecutablePath();
    const std::string::size_type slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// Returns `utf8Path` with separators normalized to '/' and each component
// case-corrected to an existing directory entry where the verbatim spelling
// does not exist. Components with no case-insensitive match are kept as-is, so
// paths for files being created resolve their directory and keep the new name,
// and genuinely missing files still fail at open time.
std::string MuResolvePath(const char* utf8Path);

#endif // !_WIN32
