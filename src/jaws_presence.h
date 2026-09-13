// jaws_presence.h - "is JAWS running?" for every component of the套件.
//
// Why not FindWindow("JFWUI2") (what every component used until 2026-09-13):
// the JAWS main window is not always visible to a normal process. Measured
// 2026-09-12 15:00 to 2026-09-13 02:55: jfw.exe was running the whole time
// (the guard, which goes by process, waited on it and saw it exit), yet the
// shell never logged 「JAWS 已啟動」 and the notification reader logged
// 「JAWS 不在」 for every toast in those eleven hours - FindWindow returned
// NULL. jfw.exe carries uiAccess=true; when it is started certain ways (its
// own restart path, an elevated launcher) its windows land in a higher
// window band that EnumWindows / FindWindow do not enumerate for callers in
// the normal band. The process is the ground truth; the window is a
// convenience that is sometimes there.
//
// So: presence = a jfw.exe process exists. The window, when it can be found,
// is still useful for reading the menu language (shell) and nothing else.
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <string>

inline DWORD JawsProcessId() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do { if (_wcsicmp(pe.szExeFile, L"jfw.exe") == 0) { pid = pe.th32ProcessID; break; } } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

inline bool JawsProcessRunning() { return JawsProcessId() != 0; }

// Folder of the running jfw.exe, or empty. Works whether or not its window
// can be seen.
inline std::wstring JawsProcessDir() {
    DWORD pid = JawsProcessId();
    if (!pid) return L"";
    std::wstring dir;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            std::wstring p(path); size_t s = p.find_last_of(L'\\');
            if (s != std::wstring::npos) dir = p.substr(0, s);
        }
        CloseHandle(h);
    }
    return dir;
}
