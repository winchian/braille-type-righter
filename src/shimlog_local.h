// Minimal file logger for IME UIA diagnostics — writes to the same debug log
// main.cpp uses, so UIA events and the polling path interleave in one file.
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

// %TEMP% rather than a fixed folder: the old path (C:\soundrts) was a
// leftover from another project and does not exist on a clean deployment, so
// every log line was silently dropped exactly where diagnostics were needed.
// Out of the user's way.
//
// %TEMP% is where this used to write, and %TEMP% is somewhere a user browses.
// A log file with a product name on it, sitting where it can be found and
// opened, invites people to read it, worry about it, or send the wrong one.
// The support path is the reporting tool that ships alongside: it knows where
// this is and collects it on request. Nobody else needs to.
inline const char* UiaLogPath() {
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        char base[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            n = GetTempPathA(MAX_PATH, base);
            if (n == 0 || n >= MAX_PATH) return nullptr;
        }
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s\\Vispero", base);
        CreateDirectoryA(dir, nullptr);
        snprintf(dir, sizeof(dir), "%s\\Vispero\\JAWS-CHT", base);
        CreateDirectoryA(dir, nullptr);
        snprintf(dir, sizeof(dir), "%s\\Vispero\\JAWS-CHT\\diag", base);
        CreateDirectoryA(dir, nullptr);
        snprintf(path, sizeof(path), "%s\\helper.log", dir);
    }
    return path;
}

// The directory the reporting tool collects from.
inline const char* DiagDir() {
    static char dir[MAX_PATH] = {};
    if (!dir[0]) {
        const char* p = UiaLogPath();
        if (!p) return nullptr;
        snprintf(dir, sizeof(dir), "%s", p);
        char* slash = strrchr(dir, '\\');
        if (slash) *slash = 0;
    }
    return dir;
}

inline void UiaLog(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char* p = UiaLogPath();
    if (!p) return;
    // The handle is opened once and kept.
    //
    // This used to fopen/fprintf/fclose per line, and with two or three lines
    // written per keystroke that is dozens of file opens a second on the same
    // thread that services typing. The user feels it as the keyboard going
    // heavy under fast input. A shipping build that charges the user for its
    // own diagnostics is a defect, not a convenience.
    static FILE* f = nullptr;
    static CRITICAL_SECTION cs;
    static bool init = false;
    if (!init) { InitializeCriticalSection(&cs); init = true; }
    EnterCriticalSection(&cs);
    if (!f) f = fopen(p, "a");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d][UIA] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        // Not flushed per line. A flush is a disk write, and on the typing
        // path that is several writes per keystroke - the user feels it as the
        // keyboard going heavy. The handle is flushed every 64 lines and
        // whenever the process is about to end; a crash costs the last few
        // lines, which is a cheaper price than a stall on every key.
        static int pending = 0;
        if (++pending >= 64) { fflush(f); pending = 0; }
    }
    LeaveCriticalSection(&cs);
}
