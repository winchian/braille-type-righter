// 點字鍵盤輸入 - a component of the Freedom Scientific Vispero 螢幕閱讀軟體中文延伸套件.
//
// Six-dot braille typing on an ordinary keyboard: F D S J K L are dots 1-6,
// A is dot 7 and ; is dot 8 (the Taiwanese convention, the same keys the
// BKey / 無字天書 users already have in their hands). Keys pressed together
// make one cell; the cell is turned into the 大千 keys that make the
// Microsoft Zhuyin IME compose the syllable, or into a letter when the IME
// is in English mode. The engine that does the turning is brl_engine.cpp;
// this file is the Windows around it.
//
// Rules this program lives by (Paul, 2026-09-09):
//
//   * Off unless the user turned it on: Ctrl+F12 toggles, default off. The
//     state survives JAWS closing and the machine shutting down. On start,
//     it says 點字鍵盤輸入已開啟 only if it is on - if it is off it says
//     nothing at all.
//
//   * It works only inside an editable text field. Its keyboard hook exists
//     only while the mode is on AND the focus is in a text field; the moment
//     the focus leaves, the hook is removed - not switched off, removed. A
//     braille hook that stays behind in a menu or a list turns JAWS key plus
//     dots 2-3-4-5 into a catastrophe, and single-letter navigation stops
//     dead. When the hook is not installed there is nothing in the keyboard
//     chain from this program at all.
//
//   * Inside the field, only the eight dot keys are taken, and only when no
//     Ctrl, Alt, Windows, Shift or JAWS key (Insert / Caps Lock held) is
//     down. Every other key, and every dot key with a modifier, passes
//     through untouched: editing keys, JAWS commands and Shift-to-switch-IME
//     all keep working. Injected keys pass through too.
//
//   * Chinese or English is decided by the operating system's own IME mode -
//     the same bare Shift the user switches with everywhere. This program
//     invents no switch of its own (feedback_braille-chord-space-rule).
//
//   * It shares nothing with IMEHelper and changes nothing in it. IMEHelper's
//     hook passes injected keys through, so the keys this program sends reach
//     the IME; what the user then hears comes from the IME's own composition
//     and candidate events, which IMEHelper already reads.
//
//   * The eight dot keys never queue (Paul, 13:45: 八顆鍵絕不排隊，稍微卡就會
//     唸錯). Nothing on the path from a chord to the injected keys waits on
//     another process: the hook thread does bit operations and a post; the
//     main thread reads the cached IME mode, builds one SendInput and hands
//     the words to say to the speech thread. The IME is asked its mode on a
//     worker thread, only on events that can change it. JAWS is spoken to on
//     the speech thread through a single-slot mailbox - an utterance that has
//     not been spoken yet is replaced by the next one, never queued behind
//     it (as IMEHelper does).
//
// No window, no tray icon, no console. Speech goes through JAWS's COM object,
// so it is JAWS's voice and JAWS's queue.
//
// The English table is JAWS's own US_Unicode.jbt, read from the JAWS
// installation at start-up (the running jfw.exe's folder, else the registry,
// else Program Files); the copy compiled in from the same file is the
// fallback. Every cell typed is logged with the mode decision and what was
// injected, so a report of 「打不出來」 can be read off the log.
//
// Threads:
//   main     message loop: hotkey, cells from the hook, focus verdicts.
//   hook     owns the WH_KEYBOARD_LL hook (installed / removed on request).
//   watcher  focus events (WinEvent) and the UIA / MSAA editable verdict.
//   mode     asks the focused window's IME for its mode (WM_IME_CONTROL).
//   speech   owns the JAWS COM object; speaks the mailbox's newest text.
#include <windows.h>
#include <imm.h>
#include <oleacc.h>
#include <uiautomation.h>
#include <mmsystem.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

#include "jaws_com.h"
#include "shimlog_local.h"
#include "brl_engine.h"

#ifndef IMC_GETCONVERSIONMODE
#define IMC_GETCONVERSIONMODE 0x0001
#endif
#ifndef IMC_GETOPENSTATUS
#define IMC_GETOPENSTATUS 0x0005
#endif

namespace {

// ------------------------------------------------------------------ clock
LONGLONG g_qpf = 1;
inline LONGLONG Qpc() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
inline long Us(LONGLONG from, LONGLONG to) { return (long)((to - from) * 1000000 / g_qpf); }
// JAWSCHT_BRAILLE_TIMING=1: one extra line per cell with the microseconds of
// each leg, and the speech thread's SayString cost. Debug runs only.
bool g_timing = false;

// ------------------------------------------------------------------ log
// Own log file, one line per event. The handle is opened once and kept, and
// flushed after every line: the data is with the operating system the moment
// the line is written, so nothing is lost when the process is ended - but no
// file is opened and closed on the typing path. Any thread may log.
SRWLOCK g_logLock = SRWLOCK_INIT;

void Log(const char* fmt, ...) {
    static FILE* f = nullptr;
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    AcquireSRWLockExclusive(&g_logLock);
    if (!f) {
        if (const char* dir = DiagDir()) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s\\braillekey.log", dir);
            f = fopen(path, "a");
        }
    }
    if (f) {
        fprintf(f, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fflush(f);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}
#define UiaLog Log

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string DotsText(unsigned dots) {
    std::string s;
    for (int i = 0; i < 8; ++i) if (dots & (1u << i)) s += (char)('1' + i);
    return s.empty() ? "0" : s;
}

// ------------------------------------------------------------------ settings
const wchar_t kRegPath[]  = L"Software\\Vispero\\JAWS-CHT";
const wchar_t kRegValue[] = L"BrailleKeyboard";

bool LoadEnabled() {
    HKEY k; DWORD v = 0, cb = sizeof(v), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool ok = RegQueryValueExW(k, kRegValue, nullptr, &type, (BYTE*)&v, &cb) == ERROR_SUCCESS && type == REG_DWORD;
    RegCloseKey(k);
    return ok && v != 0;
}

void SaveEnabled(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    DWORD v = on ? 1 : 0;
    RegSetValueExW(k, kRegValue, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
}

// ------------------------------------------------------------------ state
// Written by the focus watcher, read by the hook and the main thread.
std::atomic<bool>  g_enabled{false};
std::atomic<bool>  g_editable{false};
std::atomic<HWND>  g_fg{nullptr};       // the foreground window the verdict was made for
std::atomic<HWND>  g_focus{nullptr};    // and the focused window inside it
std::atomic<DWORD> g_hookThread{0};
std::atomic<bool>  g_waiting{false};        // the engine holds something: Space and Enter go through it first
std::atomic<DWORD> g_mainThread{0};
std::atomic<DWORD> g_modeThread{0};
HANDLE g_recheck = nullptr;             // wakes the focus watcher early

const UINT WM_APP_CELL     = WM_APP + 1;   // wParam = dots, lParam = QPC when posted
const UINT WM_APP_PLAINKEY = WM_APP + 2;   // a non-dot key went down: forget the half-typed cell
const UINT WM_APP_HOOK     = WM_APP + 3;   // wParam = 1 install, 0 remove (hook thread)
const UINT WM_APP_REFRESH  = WM_APP + 4;   // main thread: reconsider whether the hook should exist
const UINT WM_APP_MODE     = WM_APP + 5;   // mode thread: ask the IME again; wParam = reason (static string), lParam = delay ms

const ULONG_PTR kOurMark = 0x4B4C5242;     // 'BRLK' on every key we inject

// Test only: with the environment variable JAWSCHT_BRAILLE_DRYRUN set, the
// hook is installed and removed exactly as in real use and logs what it
// would have taken, but every key passes through. Lets the focus tracking
// be watched on a live machine without taking anyone's letters away.
bool g_dryRun = false;
// Test only: JAWSCHT_BRAILLE_UNICODE set makes every English character go in
// as a Unicode key event (VK_PACKET), the way the first build did. The
// default is virtual keys for ASCII letters and digits (see Injector::Text).
bool g_forceUnicode = false;

// ------------------------------------------------------------------ IME mode cache
// Chinese or English right now. The answer is asked of the focused window's
// IME the way IMEHelper asks (WM_IME_CONTROL with a short timeout: never a
// plain SendMessage) - but on the mode thread, never on a cell. The main
// thread reads the cached answer. It is asked again only when something that
// can change it happened: the hook went up (focus decided), the focus moved,
// a Shift or Ctrl was released (bare Shift toggles the IME, Ctrl+Shift the
// layout), Space went down with a modifier (Ctrl+Space), an injection failed,
// the layout the cell sees differs from the cached one - and, as the one
// safety against a change nobody saw (the mouse on the language bar), when
// the answer is older than two seconds, asynchronously.
//
// No Chinese IME at all means English letters; so does an IME that is
// closed (open status 0), whatever its conversion mode says. Every step of
// the decision is kept for the log, because a wrong answer here is
// 「打不出英文」.
struct ModeInfo {
    BrlMode mode = BrlMode::English;
    WORD    layout = 0;
    int     open = -1;          // IMC_GETOPENSTATUS, -1 = not asked / no answer
    long    conv = -1;          // IMC_GETCONVERSIONMODE
    const char* why = "尚未查詢";
};

SRWLOCK   g_modeLock = SRWLOCK_INIT;
ModeInfo  g_modeCache;                  // guarded by g_modeLock
ULONGLONG g_modeStamp = 0;              // GetTickCount64 when filled, 0 = never
std::atomic<bool> g_modeValid{false};   // false between an invalidating event and the next answer
std::atomic<bool> g_modeQueued{false};  // a stale-refresh has been posted and not yet answered
HANDLE g_modeFresh = nullptr;           // manual-reset: set when an answer lands, reset on invalidation

// Any thread. Cheap: an atomic, an event reset and a post - the hook thread
// calls it too.
void RequestMode(const char* reason, unsigned delayMs, bool invalidate) {
    if (invalidate) { g_modeValid.store(false); if (g_modeFresh) ResetEvent(g_modeFresh); }
    DWORD t = g_modeThread.load();
    if (t) PostThreadMessageW(t, WM_APP_MODE, (WPARAM)reason, (LPARAM)delayMs);
}

// A key that switches the IME went past (bare Shift, Caps Lock, Ctrl+Space,
// Shift+Space). The IME acts on it in the application's thread, after JAWS
// and the application have had the key - 40 ms was not always enough (the
// 15:54 log: the answer 40 ms after a Shift was the old mode, and five
// English cells went into a Chinese IME). So the IME is asked again and
// again on a short schedule until its answer differs from the last real one,
// or the schedule runs out. Every answer goes into the cache as it lands.
//   kind 1: a change of Chinese/English (or open/closed) is expected - bare Shift
//   kind 2: some change is expected - Caps Lock, Ctrl+Space, Shift+Space, Ctrl up
const UINT WM_APP_MODE_VERIFY = WM_APP + 6;   // mode thread: wParam = reason, lParam = kind
void VerifyMode(const char* reason, int kind, bool invalidate) {
    if (invalidate) { g_modeValid.store(false); if (g_modeFresh) ResetEvent(g_modeFresh); }
    DWORD t = g_modeThread.load();
    if (t) PostThreadMessageW(t, WM_APP_MODE_VERIFY, (WPARAM)reason, (LPARAM)kind);
}

// Bare Shift on the Microsoft Bopomofo IME toggles Chinese / English. The
// cache is flipped the moment the Shift comes up, so a cell typed right
// after it is already in the new mode; VerifyMode then confirms with the IME
// and corrects the cache if the IME did not switch (Shift switching turned
// off in its settings, IME closed, Caps Lock on). Only predicted when the
// last real answer was an open Chinese IME layout with Caps Lock off.
bool PredictShiftToggle() {
    bool caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    AcquireSRWLockExclusive(&g_modeLock);
    bool ok = g_modeValid.load() && g_modeCache.layout == 0x0404 && g_modeCache.open == 1 && g_modeCache.conv >= 0 && !caps;
    if (ok) {
        g_modeCache.conv ^= IME_CMODE_NATIVE;
        g_modeCache.mode = (g_modeCache.conv & IME_CMODE_NATIVE) ? BrlMode::Chinese : BrlMode::English;
        g_modeCache.why = (g_modeCache.conv & IME_CMODE_NATIVE) ? "Shift 切換為中文（預測）" : "Shift 切換為英文（預測）";
        g_modeStamp = GetTickCount64();
    }
    ReleaseSRWLockExclusive(&g_modeLock);
    return ok;
}

ModeInfo QueryMode() {
    ModeInfo m;
    HWND fg = GetForegroundWindow();
    if (!fg) { m.why = "無前景視窗"; return m; }
    DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    m.layout = LOWORD(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(tid)));
    if (m.layout != 0x0404) { m.why = "鍵盤配置非中文"; return m; }
    HWND target = fg;
    GUITHREADINFO gti = {}; gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(tid, &gti) && gti.hwndFocus) target = gti.hwndFocus;
    HWND ime = ImmGetDefaultIMEWnd(target);
    if (!ime && target != fg) ime = ImmGetDefaultIMEWnd(fg);
    if (!ime) { m.why = "無 IME 視窗"; return m; }
    DWORD_PTR v = 0;
    if (SendMessageTimeoutW(ime, WM_IME_CONTROL, IMC_GETOPENSTATUS, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 30, &v)) {
        m.open = v ? 1 : 0;
        if (!v) { m.why = "IME 關閉"; return m; }
    }
    v = 0;
    if (!SendMessageTimeoutW(ime, WM_IME_CONTROL, IMC_GETCONVERSIONMODE, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 30, &v)) { m.why = "IME 沒回答"; return m; }
    m.conv = (long)v;
    m.mode = (v & IME_CMODE_NATIVE) ? BrlMode::Chinese : BrlMode::English;
    m.why = (v & IME_CMODE_NATIVE) ? "IME 中文" : "IME 英數";
    return m;
}

DWORD WINAPI ModeThread(LPVOID) {
    g_modeThread.store(GetCurrentThreadId());
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // create the queue
    ModeInfo last;
    auto publish = [&](const ModeInfo& m) {
        AcquireSRWLockExclusive(&g_modeLock);
        g_modeCache = m; g_modeStamp = GetTickCount64();
        ReleaseSRWLockExclusive(&g_modeLock);
        g_modeValid.store(true);
        g_modeQueued.store(false);
        SetEvent(g_modeFresh);
    };
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_APP_MODE_VERIFY) {
            const char* reason = (const char*)msg.wParam;
            int kind = (int)msg.lParam;
            // Newer switch keys make this one moot: the last one wins.
            MSG more;
            while (PeekMessageW(&more, nullptr, WM_APP_MODE_VERIFY, WM_APP_MODE_VERIFY, PM_REMOVE)) {
                reason = (const char*)more.wParam; kind = (int)more.lParam;
            }
            static const DWORD kSchedule[] = { 8, 8, 10, 14, 20, 30, 40, 60, 90, 120, 160, 200, 240 };   // ~1 s in all
            const ModeInfo base = last;
            ModeInfo m; long us = 0; DWORD waited = 0; int polls = 0; bool changed = false;
            LONGLONG t0 = Qpc();
            for (DWORD d : kSchedule) {
                Sleep(d); waited += d; ++polls;
                LONGLONG a = Qpc();
                m = QueryMode();
                us = Us(a, Qpc());
                publish(m);
                bool native = (m.conv & IME_CMODE_NATIVE) != (base.conv & IME_CMODE_NATIVE);
                bool state  = m.open != base.open || m.layout != base.layout || m.mode != base.mode;
                changed = kind == 1 ? (native || state) : (state || m.conv != base.conv);
                if (changed) break;
                // A newer switch key arrived while waiting: start over from it.
                if (PeekMessageW(&more, nullptr, WM_APP_MODE_VERIFY, WM_APP_MODE_VERIFY, PM_NOREMOVE)) break;
            }
            bool caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
            UiaLog("點字鍵盤輸入: IME 模式 %s（%s 配置=%04X open=%d conv=%ld caps=%d）因=%s %s，第 %d 次查詢、%lu ms 後，本次 %ld µs，共 %ld µs",
                   m.mode == BrlMode::Chinese ? "中文" : "英文", m.why, m.layout, m.open, m.conv, caps ? 1 : 0, reason,
                   changed ? "已切換" : "沒切換", polls, (unsigned long)waited, us, Us(t0, Qpc()));
            last = m;
            continue;
        }
        if (msg.message != WM_APP_MODE) continue;
        const char* reason = (const char*)msg.wParam;
        DWORD delay = (DWORD)msg.lParam;
        // Requests that piled up are one question; the longest delay wins
        // (a Shift release wants the IME to have processed the key first).
        MSG more;
        while (PeekMessageW(&more, nullptr, WM_APP_MODE, WM_APP_MODE, PM_REMOVE)) {
            reason = (const char*)more.wParam;
            if ((DWORD)more.lParam > delay) delay = (DWORD)more.lParam;
        }
        if (delay) Sleep(delay);
        LONGLONG a = Qpc();
        ModeInfo m = QueryMode();
        long us = Us(a, Qpc());
        publish(m);
        bool changed = m.mode != last.mode || m.layout != last.layout || m.open != last.open || m.conv != last.conv;
        bool stale = reason && reason[0] == '@';   // the safety checks (two seconds, after a cell): log only a change
        if (changed || !stale || g_timing) {
            bool caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
            UiaLog("點字鍵盤輸入: IME 模式 %s（%s 配置=%04X open=%d conv=%ld caps=%d）因=%s 查詢 %ld µs%s", m.mode == BrlMode::Chinese ? "中文" : "英文",
                   m.why, m.layout, m.open, m.conv, caps ? 1 : 0, reason ? (stale ? reason + 1 : reason) : "?", us,
                   (changed && stale) ? "（沒人看見的切換，之前的方可能打錯模式）" : "");
        }
        last = m;
    }
    return 0;
}

// Main thread, per cell. Never asks the IME. Waits for the mode thread only
// when an invalidating event (focus, Shift) has just happened and the answer
// is not in yet - at most 15 ms, and in practice the answer is there long
// before a chord can be completed.
ModeInfo CachedMode(long* waitedUs) {
    *waitedUs = 0;
    if (!g_modeValid.load()) {
        LONGLONG a = Qpc();
        WaitForSingleObject(g_modeFresh, 15);
        *waitedUs = Us(a, Qpc());
    }
    ModeInfo m; ULONGLONG stamp;
    AcquireSRWLockShared(&g_modeLock);
    m = g_modeCache; stamp = g_modeStamp;
    ReleaseSRWLockShared(&g_modeLock);
    // The layout is cheap to read (thread state, no message to anyone) and a
    // layout switch nobody saw is caught here on the spot.
    HWND fg = GetForegroundWindow();
    WORD layout = LOWORD(reinterpret_cast<UINT_PTR>(GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, nullptr) : 0)));
    if (layout != m.layout) {
        m.layout = layout;
        if (layout != 0x0404) { m.mode = BrlMode::English; m.why = "鍵盤配置非中文（即時）"; }
        RequestMode("配置變了", 0, false);
    } else if (stamp && GetTickCount64() - stamp > 2000 && !g_modeQueued.exchange(true)) {
        RequestMode("@逾兩秒", 0, false);
    }
    return m;
}

// ------------------------------------------------------------------ speech
// One slot. The main thread drops text in and returns; the speech thread,
// which owns the JAWS COM object (jaws_com.h: the object is only ever called
// from the thread that created it), speaks whatever is in the slot when it
// gets to it. Text that was replaced before it was spoken is gone - the
// previous cell's name is stale the moment the next cell lands (IMEHelper's
// rule), and speech never holds a cell back.
struct SpeechSlot {
    std::wstring text;
    bool refuse = false;        // play the refusal sound first
    bool reset = false;         // JAWS went away: drop the COM object
    bool quit = false;
    ULONGLONG notBefore = 0;    // GetTickCount64; 0 = now
};
SRWLOCK    g_sayLock = SRWLOCK_INIT;
SpeechSlot g_say;               // guarded by g_sayLock
HANDLE     g_sayEvent = nullptr;   // auto-reset
JawsCom    g_jaws;              // speech thread only

bool JawsRunning() { return FindWindowW(L"JFWUI2", nullptr) != nullptr; }

void Say(const wchar_t* s, unsigned delayMs = 0) {
    AcquireSRWLockExclusive(&g_sayLock);
    g_say.text = s;
    g_say.notBefore = delayMs ? GetTickCount64() + delayMs : 0;
    ReleaseSRWLockExclusive(&g_sayLock);
    SetEvent(g_sayEvent);
}

void Refuse() {
    AcquireSRWLockExclusive(&g_sayLock);
    g_say.refuse = true;
    ReleaseSRWLockExclusive(&g_sayLock);
    SetEvent(g_sayEvent);
}

void SpeechCommand(bool reset, bool quit) {
    AcquireSRWLockExclusive(&g_sayLock);
    if (reset) g_say.reset = true;
    if (quit) g_say.quit = true;
    ReleaseSRWLockExclusive(&g_sayLock);
    SetEvent(g_sayEvent);
}

DWORD WINAPI SpeechThread(LPVOID) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        WaitForSingleObject(g_sayEvent, INFINITE);
        for (;;) {
            SpeechSlot s;
            AcquireSRWLockExclusive(&g_sayLock);
            ULONGLONG now = GetTickCount64();
            if (g_say.notBefore > now && !g_say.quit && !g_say.reset && !g_say.refuse) {
                DWORD wait = (DWORD)(g_say.notBefore - now);
                ReleaseSRWLockExclusive(&g_sayLock);
                WaitForSingleObject(g_sayEvent, wait);   // a newer text replaces the delayed one
                continue;
            }
            s = g_say;
            g_say.text.clear(); g_say.refuse = g_say.reset = false; g_say.notBefore = 0;
            ReleaseSRWLockExclusive(&g_sayLock);
            if (s.quit) { g_jaws.Shutdown(); if (SUCCEEDED(hrCo)) CoUninitialize(); return 0; }
            if (s.reset) g_jaws.Shutdown();
            if (s.refuse) PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
            if (!s.text.empty() && JawsRunning()) {
                LONGLONG a = Qpc();
                bool ok = g_jaws.SayString(s.text, true);
                if (!ok) UiaLog("點字鍵盤輸入: 送話失敗");
                else if (g_timing) UiaLog("點字鍵盤輸入: 計時 語音 SayString %ld µs 「%s」", Us(a, Qpc()), Utf8(s.text).c_str());
            }
            break;
        }
    }
}

// ------------------------------------------------------------------ hook
// Runs on the hook thread. The rules: do almost nothing, never block, and
// take a key only when every condition holds. Missing a cell is a mistyped
// character; swallowing a key that was a command is a screen reader that
// stopped obeying. Nothing here sleeps or debounces: a chord is complete the
// moment its last dot key comes up, and the cell is posted right then.
HHOOK    g_hook = nullptr;
unsigned g_pressed = 0;   // dot bits currently held
unsigned g_accum   = 0;   // dot bits seen since the chord began
unsigned g_swallowUp = 0; // Space (1) / Enter (2) whose key-down was taken: take the key-up too
bool     g_shiftBare = false;   // a Shift is down and no other key has gone down since (hook thread only)
std::atomic<LONGLONG> g_tLastOwnKeyUp{0};   // QPC when the raw input queue last delivered one of our key-ups

// Sent to the main thread as WM_APP_CELL instead of a cell.
const unsigned kSpaceCommit = 0x1000;   // Space taken while the engine holds something
const unsigned kEnterCommit = 0x1001;   // Enter taken while the engine holds something
const unsigned kSpacePassed = 0x1002;   // Space went through to the application
const unsigned kEnterPassed = 0x1003;   // Enter went through to the application

unsigned DotFor(DWORD vk) {
    switch (vk) {
    case 'F': return kDot1; case 'D': return kDot2; case 'S': return kDot3;
    case 'J': return kDot4; case 'K': return kDot5; case 'L': return kDot6;
    case 'A': return kDot7; case VK_OEM_1: return kDot8;     // ';'
    }
    return 0;
}

// Seven GetAsyncKeyState reads. Asked only for a dot key, Space or Enter -
// never for the other keys, which are passed on after the dot test alone.
bool ModifierHeld() {
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) ||
           (GetAsyncKeyState(VK_SHIFT) & 0x8000)   || (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
           (GetAsyncKeyState(VK_RWIN) & 0x8000)    ||
           (GetAsyncKeyState(VK_INSERT) & 0x8000)  || (GetAsyncKeyState(VK_CAPITAL) & 0x8000);   // JAWS keys
}

inline void PostCell(unsigned what) { PostThreadMessageW(g_mainThread.load(), WM_APP_CELL, what, (LPARAM)Qpc()); }

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION) return CallNextHookEx(g_hook, code, wParam, lParam);
    const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

    // Not ours to touch: injected keys (including our own), or a focus that
    // is no longer the one the verdict was made for. GetForegroundWindow and
    // GetGUIThreadInfo read shared state and do not wait on anyone.
    if (k->flags & LLKHF_INJECTED) {
        // Our own keys coming back through the raw input queue: the stamp
        // of the last one tells (with JAWSCHT_BRAILLE_TIMING) how long the
        // queue held them - the part of a cell's delay that is not ours.
        if (k->dwExtraInfo == kOurMark && !down) g_tLastOwnKeyUp.store(Qpc());
        return CallNextHookEx(g_hook, code, wParam, lParam);
    }
    // Bare Shift = a Shift that came down and went up with no other key in
    // between: the Microsoft IMEs' Chinese/English switch. Any other key
    // going down while it is held makes it a plain modifier.
    const DWORD vk = k->vkCode;
    const bool isShift = vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
    if (down) g_shiftBare = isShift;
    HWND fg = GetForegroundWindow();
    HWND focus = fg;
    if (fg) {
        GUITHREADINFO gti = {}; gti.cbSize = sizeof(gti);
        if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gti) && gti.hwndFocus) focus = gti.hwndFocus;
    }
    if (fg != g_fg.load() || focus != g_focus.load() || !g_editable.load()) {
        g_pressed = g_accum = 0;
        if (g_recheck) SetEvent(g_recheck);
        return CallNextHookEx(g_hook, code, wParam, lParam);
    }

    unsigned dot = DotFor(vk);
    if (dot) {
        if (ModifierHeld()) { g_pressed = g_accum = 0; return CallNextHookEx(g_hook, code, wParam, lParam); }
        if (g_dryRun) {
            if (down) UiaLog("點字鍵盤輸入(試跑): 會吃掉 vk=%lu 點%u，實際放行", vk, dot);
            return CallNextHookEx(g_hook, code, wParam, lParam);
        }
        if (down) {
            g_pressed |= dot;             // repeats just set the bit again
            g_accum   |= dot;
        } else {
            g_pressed &= ~dot;
            if (g_pressed == 0 && g_accum) { PostCell(g_accum); g_accum = 0; }
        }
        return 1;   // the dot key never reaches the application or JAWS
    }

    // Space and Enter belong to the engine while it holds something: a
    // Chinese mark waiting for its confirming Space (the space is then
    // consumed, as Taiwanese braille typing has it), or English cells
    // waiting for the next one (they are typed first, then the key is sent
    // on). Taken here, on the way in, so that the order on screen is the
    // order typed. A Space with nothing waiting passes through, and the
    // engine is only told - it ends a number, not the Nemeth context.
    if ((vk == VK_SPACE || vk == VK_RETURN) && !g_dryRun && !ModifierHeld()) {
        unsigned bit = vk == VK_SPACE ? 1 : 2;
        if (down) {
            if (g_waiting.load()) {
                g_swallowUp |= bit;
                PostCell(vk == VK_SPACE ? kSpaceCommit : kEnterCommit);
                return 1;
            }
            g_pressed = g_accum = 0;
            PostCell(vk == VK_SPACE ? kSpacePassed : kEnterPassed);
            return CallNextHookEx(g_hook, code, wParam, lParam);
        }
        if (g_swallowUp & bit) { g_swallowUp &= ~bit; return 1; }
        return CallNextHookEx(g_hook, code, wParam, lParam);
    }

    // Every other key (arrow, Backspace, Escape, a modifier, Space with a
    // modifier...) ends whatever cell was half typed and, for the engine,
    // ends a number or a word. The keys that switch the IME are noticed on
    // the way past and the IME is then asked on a schedule until it has
    // acted (VerifyMode): a bare Shift coming up (Chinese / English - the
    // cache is flipped at once, the IME confirms), Caps Lock coming up
    // (English while it is on), Space going down with a modifier (Ctrl+Space
    // opens / closes, Shift+Space full / half width), Ctrl coming up
    // (Ctrl+Shift or Ctrl+Space just happened).
    if (down) {
        g_pressed = g_accum = 0;
        PostThreadMessageW(g_mainThread.load(), WM_APP_PLAINKEY, 0, 0);
        if (vk == VK_SPACE) VerifyMode("修飾鍵加空白", 2, true);
    } else if (isShift) {
        bool ctrlOrAlt = (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000) || (GetAsyncKeyState(VK_LWIN) & 0x8000);
        if (g_shiftBare && !ctrlOrAlt) {
            bool predicted = PredictShiftToggle();
            VerifyMode(predicted ? "單獨 Shift（已預測切換）" : "單獨 Shift", 1, !predicted);
        } else VerifyMode("Shift 放開（組合鍵）", 2, true);
        g_shiftBare = false;
    } else if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        VerifyMode("Ctrl 放開", 2, true);
    } else if (vk == VK_CAPITAL) {
        VerifyMode("Caps Lock", 2, true);
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

// The hook lives on this thread so that nothing the main thread does can
// hold the keyboard callback up. Windows drops a hook whose callback is slow.
DWORD WINAPI HookThread(LPVOID) {
    g_hookThread.store(GetCurrentThreadId());
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // create the queue
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message != WM_APP_HOOK) continue;
        bool want = msg.wParam != 0;
        if (want && !g_hook) {
            g_pressed = g_accum = 0;
            g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
            UiaLog("點字鍵盤輸入: 鉤子%s", g_hook ? "已掛上（在編輯框內）" : "掛載失敗");
            if (g_hook) RequestMode("鉤子掛上", 0, true);
        } else if (!want && g_hook) {
            UnhookWindowsHookEx(g_hook);
            g_hook = nullptr;
            g_pressed = g_accum = 0;
            UiaLog("點字鍵盤輸入: 鉤子已拆除");
        }
    }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    return 0;
}

// ------------------------------------------------------------------ focus
// Whether the focus is in something that takes typing. UI Automation first,
// the way IMEHelper decides it; then MSAA, because Chromium editors (Claude,
// LINE, browsers) answer MSAA with an editable text role where UIA reports
// the document. Runs only on the watcher thread, which owns the UIA object.
int EditableByUia(IUIAutomation* uia) {
    IUIAutomationElement* fe = nullptr;
    int verdict = -1;
    if (SUCCEEDED(uia->GetFocusedElement(&fe)) && fe) {
        IUIAutomationTextEditPattern* te = nullptr;
        fe->GetCurrentPatternAs(UIA_TextEditPatternId, IID_PPV_ARGS(&te));
        if (te) { verdict = 1; te->Release(); }
        if (verdict < 0) {
            CONTROLTYPEID ct = 0;
            if (SUCCEEDED(fe->get_CurrentControlType(&ct)) && ct) {
                if (ct == UIA_EditControlTypeId) verdict = 1;
                else if (ct == UIA_DocumentControlTypeId || ct == UIA_ComboBoxControlTypeId) {
                    IUIAutomationValuePattern* vp = nullptr;
                    fe->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&vp));
                    if (vp) { BOOL ro = TRUE; if (SUCCEEDED(vp->get_CurrentIsReadOnly(&ro))) verdict = ro ? 0 : 1; vp->Release(); }
                    else verdict = 0;
                } else verdict = 0;
            }
        }
        fe->Release();
    }
    return verdict;
}

int EditableByMsaa(HWND focus) {
    if (!focus) return -1;
    IAccessible* acc = nullptr;
    if (FAILED(AccessibleObjectFromWindow(focus, OBJID_CLIENT, IID_IAccessible, (void**)&acc)) || !acc) return -1;
    int verdict = -1;
    VARIANT vf; VariantInit(&vf);
    IAccessible* target = acc; target->AddRef();
    VARIANT id; VariantInit(&id); id.vt = VT_I4; id.lVal = CHILDID_SELF;
    // Follow accFocus down a few levels to the focused leaf.
    for (int depth = 0; depth < 8; ++depth) {
        VariantClear(&vf);
        if (FAILED(target->get_accFocus(&vf))) break;
        if (vf.vt == VT_DISPATCH && vf.pdispVal) {
            IAccessible* next = nullptr;
            if (SUCCEEDED(vf.pdispVal->QueryInterface(IID_IAccessible, (void**)&next)) && next) {
                target->Release(); target = next; id.lVal = CHILDID_SELF; continue;
            }
            break;
        } else if (vf.vt == VT_I4) {
            if (vf.lVal == CHILDID_SELF) break;
            id.lVal = vf.lVal; break;
        } else break;
    }
    VARIANT role; VariantInit(&role);
    VARIANT state; VariantInit(&state);
    if (SUCCEEDED(target->get_accRole(id, &role)) && role.vt == VT_I4 &&
        SUCCEEDED(target->get_accState(id, &state)) && state.vt == VT_I4) {
        if (role.lVal == ROLE_SYSTEM_TEXT) verdict = (state.lVal & STATE_SYSTEM_READONLY) ? 0 : 1;
        else verdict = 0;
    }
    VariantClear(&role); VariantClear(&state); VariantClear(&vf);
    target->Release();
    acc->Release();
    return verdict;
}

// The focused object as the focus event names it: role and read-only state
// straight off that object, no tree walk.
int EditableByEvent(HWND hwnd, LONG idObject, LONG idChild) {
    IAccessible* acc = nullptr; VARIANT child; VariantInit(&child);
    if (FAILED(AccessibleObjectFromEvent(hwnd, idObject, idChild, &acc, &child)) || !acc) return -1;
    int verdict = -1;
    VARIANT role; VariantInit(&role);
    VARIANT state; VariantInit(&state);
    if (SUCCEEDED(acc->get_accRole(child, &role)) && role.vt == VT_I4 &&
        SUCCEEDED(acc->get_accState(child, &state)) && state.vt == VT_I4) {
        if (role.lVal == ROLE_SYSTEM_TEXT) verdict = (state.lVal & STATE_SYSTEM_READONLY) ? 0 : 1;
        else verdict = 0;
    }
    VariantClear(&role); VariantClear(&state); VariantClear(&child);
    acc->Release();
    return verdict;
}

IUIAutomation* g_uia = nullptr;
HWND g_lastFocusHwnd = nullptr; int g_lastVerdict = -2;
HWND g_lastEventHwnd = nullptr; LONG g_lastEventObj = 0, g_lastEventChild = 0;   // the focus event the verdict was made for
DWORD g_ignoredPid = 0;                                                         // last foreign process whose focus events were ignored (log once)

// The window that has the keyboard: the foreground window's thread says.
HWND KeyboardFocus(HWND fg) {
    if (!fg) return nullptr;
    GUITHREADINFO gti = {}; gti.cbSize = sizeof(gti);
    if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gti) && gti.hwndFocus) return gti.hwndFocus;
    return fg;
}

// One decision per focus change. Nothing here polls: the application is
// asked once when the focus event says it moved, and the answer stands until
// the next event. Paul, 2026-09-09: 「抓取是抓取，焦點是焦點」「不要頻繁刷新
// 抓取，即時性高的通訊軟體根本跟不上」 - a Chromium application answers every
// accessibility query by re-announcing its focus, and JAWS follows.
//
// Two kinds of focus event are not a focus change and get no query at all
// (15:59:42 log: the IME's candidate list, a Windows.UI.Core.CoreWindow in
// TextInputHost.exe, raised focus for its items - screen readers read them
// that way - and the hook was dropped in the middle of a word):
//   * an event from a window of another process than the foreground one -
//     the IME's candidate window, a tooltip, JAWS's own windows. The
//     keyboard is still where it was.
//   * an event naming the very object the standing verdict was made for -
//     the application re-announcing its focus, often because we asked.
void Decide(HWND hwnd, LONG idObject, LONG idChild, bool fromEvent) {
    if (!g_enabled.load()) return;
    HWND fg = GetForegroundWindow();
    if (fromEvent) {
        DWORD evPid = 0, fgPid = 0;
        GetWindowThreadProcessId(hwnd, &evPid);
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        if (fg && evPid != fgPid) {
            if (evPid != g_ignoredPid) {
                g_ignoredPid = evPid;
                wchar_t cls[64] = {}; GetClassNameW(hwnd, cls, 64);
                UiaLog("點字鍵盤輸入: 略過別的程序的焦點事件 %p 類別 %ls（前景沒變，鍵盤還在原處）", (void*)hwnd, cls);
            }
            return;
        }
        if (hwnd == g_lastEventHwnd && idObject == g_lastEventObj && idChild == g_lastEventChild && g_lastVerdict != -2 && fg == g_fg.load())
            return;   // the same object again: the verdict stands, nothing is asked
    }
    int verdict = -1;
    if (fromEvent) verdict = EditableByEvent(hwnd, idObject, idChild);
    if (verdict < 0) verdict = EditableByMsaa(hwnd);
    if (verdict < 0 && g_uia) verdict = EditableByUia(g_uia);
    bool editable = verdict == 1;
    // The hook compares against the keyboard-focus window of the foreground
    // thread, so that is what is remembered - not the event's window, which
    // in a Chromium application can be a different child of the same thread.
    HWND focus = KeyboardFocus(fg);
    if (!focus) focus = hwnd;
    g_fg.store(fg); g_focus.store(focus);
    g_lastEventHwnd = fromEvent ? hwnd : nullptr; g_lastEventObj = idObject; g_lastEventChild = idChild;
    bool changed = hwnd != g_lastFocusHwnd || verdict != g_lastVerdict || editable != g_editable.load();
    g_editable.store(editable);
    if (changed) {
        g_lastFocusHwnd = hwnd; g_lastVerdict = verdict;
        wchar_t cls[64] = {}; GetClassNameW(hwnd, cls, 64);
        UiaLog("點字鍵盤輸入: 焦點 %p 類別 %ls -> %s", (void*)hwnd, cls, verdict == 1 ? "可編輯" : verdict == 0 ? "非編輯" : "不明");
        PostThreadMessageW(g_mainThread.load(), WM_APP_REFRESH, 0, 0);
    }
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (event != EVENT_OBJECT_FOCUS || !hwnd) return;
    Decide(hwnd, idObject, idChild, true);
}

// Where the focus is right now, asked once - on enable, and when the hook
// found the focus pair it holds no longer matches.
void DecideNow() {
    HWND fg = GetForegroundWindow();
    HWND focus = fg;
    if (fg) {
        GUITHREADINFO gti = {}; gti.cbSize = sizeof(gti);
        if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gti) && gti.hwndFocus) focus = gti.hwndFocus;
    }
    Decide(focus, OBJID_CLIENT, CHILDID_SELF, false);
}

DWORD WINAPI FocusWatcher(LPVOID) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_uia));
    if (!g_uia) UiaLog("點字鍵盤輸入: 無法建立 UIA，只用 MSAA 判斷編輯框");

    // A focus subscription, out of context: the callback runs here, in this
    // process, on this thread's message loop. Not in the application, not in
    // JAWS.
    HWINEVENTHOOK ev = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, OnWinEvent, 0, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!ev) UiaLog("點字鍵盤輸入: 焦點事件訂閱失敗");

    bool lastEnabled = false;
    for (;;) {
        DWORD w = MsgWaitForMultipleObjects(1, &g_recheck, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) {
            bool en = g_enabled.load();
            if (en) DecideNow();
            else if (lastEnabled) { g_editable.store(false); g_fg.store(nullptr); g_focus.store(nullptr); g_lastFocusHwnd = nullptr; g_lastVerdict = -2; PostThreadMessageW(g_mainThread.load(), WM_APP_REFRESH, 0, 0); }
            lastEnabled = en;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    UnhookWinEvent(ev);
    if (g_uia) g_uia->Release();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}

// ------------------------------------------------------------------ typing
// Everything a cell produces - the 大千 keys of a syllable and its tone, an
// English run, the text that was waiting plus the Backspace / Enter / Space
// after it - is collected into one INPUT array and handed to SendInput once.
// One call, the whole sequence in order, no chance for anything to land in
// between. Every event carries 'BRLK'.
//
// The virtual keys and scan codes are resolved against the focused thread's
// layout, from a table filled once per layout (VkKeyScanExW for the 128
// ASCII characters, MapVirtualKeyExW for the 256 virtual keys), not per key.
struct LayoutTable {
    HKL   hkl = nullptr;
    SHORT vkOf[128];    // VkKeyScanExW result per ASCII character
    WORD  vscOf[256];   // scan code per virtual key
    void For(HKL h) {
        if (h == hkl && hkl) return;
        hkl = h;
        for (int c = 0; c < 128; ++c) vkOf[c] = VkKeyScanExW((wchar_t)c, h);
        for (int v = 0; v < 256; ++v) vscOf[v] = (WORD)MapVirtualKeyExW((UINT)v, MAPVK_VK_TO_VSC, h);
    }
};
LayoutTable g_layout;    // main thread only

HKL FocusedLayout() {
    HWND fg = GetForegroundWindow();
    return GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, nullptr) : 0);
}

struct Injector {
    std::vector<INPUT> in;
    bool vk = false, uni = false;

    Injector() { g_layout.For(FocusedLayout()); in.reserve(24); }

    void Down(WORD v, bool extended = false) { Add(v, extended, false); }
    void Up(WORD v, bool extended = false)   { Add(v, extended, true); }
    void Key(WORD v, bool extended = false)  { Down(v, extended); Up(v, extended); }
    void Add(WORD v, bool extended, bool up) {
        INPUT i = {};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = v;
        i.ki.wScan = g_layout.vscOf[v & 0xFF];
        i.ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
        i.ki.dwExtraInfo = kOurMark;
        in.push_back(i);
    }
    void Unicode(wchar_t c) {
        for (int k = 0; k < 2; ++k) {
            INPUT i = {};
            i.type = INPUT_KEYBOARD;
            i.ki.wScan = c;
            i.ki.dwFlags = KEYEVENTF_UNICODE | (k ? KEYEVENTF_KEYUP : 0);
            i.ki.dwExtraInfo = kOurMark;
            in.push_back(i);
        }
        uni = true;
    }
    // The 大千 keys are typed as the physical keys they are, so the IME
    // composes from them exactly as if the user had pressed them.
    void Keys(const std::wstring& keys) {
        for (wchar_t c : keys) {
            SHORT r = c < 128 ? g_layout.vkOf[c] : VkKeyScanExW(c, g_layout.hkl);
            if (r == -1) continue;
            Key((WORD)(r & 0xFF));
            vk = true;
        }
    }
    // English-mode text. ASCII letters and digits go in as the virtual keys
    // they are on the focused thread's layout - the one kind of key event
    // every application and every IME accepts; some drop the Unicode
    // (VK_PACKET) kind while an IME context is attached. A capital needs
    // Shift held around its key (Caps Lock is honoured), which is not a bare
    // Shift press and so does not trip the IME's own Shift switch. Everything
    // else - punctuation, symbols, Greek - goes in as a Unicode character,
    // the only way that does not depend on the layout.
    void Text(const std::wstring& text) {
        bool caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
        for (wchar_t c : text) {
            bool alpha = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
            bool digit = c >= L'0' && c <= L'9';
            if (!g_forceUnicode && (alpha || digit)) {
                SHORT r = g_layout.vkOf[c];
                if (r != -1 && !(r & 0x600)) {          // a key on this layout, without Ctrl or Alt
                    bool shift = (r & 0x100) != 0;
                    if (alpha && caps) shift = !shift;
                    if (shift) Down(VK_SHIFT);
                    Key((WORD)(r & 0xFF));
                    if (shift) Up(VK_SHIFT);
                    vk = true;
                    continue;
                }
            }
            Unicode(c);
        }
    }
    // Returns the number of events that went in; logs and asks for the IME
    // mode again if any did not (the focus may have moved under us).
    UINT Send() {
        if (in.empty()) return 0;
        UINT n = SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
        if (n != in.size()) {
            UiaLog("點字鍵盤輸入: SendInput 只送進 %u/%u 個事件 (%lu)", n, (unsigned)in.size(), GetLastError());
            RequestMode("注入失敗", 0, true);
        }
        return n;
    }
    const char* How() const { return vk && uni ? "vk+unicode" : vk ? "vk" : uni ? "unicode" : "-"; }
};

// ------------------------------------------------------------------ main
BrlEngine g_engine;

void ApplyHookWish() {
    bool want = g_enabled.load() && g_editable.load();
    DWORD t = g_hookThread.load();
    if (t) PostThreadMessageW(t, WM_APP_HOOK, want ? 1 : 0, 0);
    if (!want) g_engine.Reset();
    else RequestMode("焦點決定", 0, true);
}

void SetEnabled(bool on, bool announce) {
    g_enabled.store(on);
    SaveEnabled(on);
    g_engine.Reset();
    if (g_recheck) SetEvent(g_recheck);
    ApplyHookWish();
    UiaLog("點字鍵盤輸入: %s", on ? "開啟" : "關閉");
    if (announce) Say(on ? L"點字鍵盤輸入 開啟" : L"點字鍵盤輸入 關閉");
}

const char* ActionName(BrlOutput::Action a) {
    switch (a) {
    case BrlOutput::None: return "無"; case BrlOutput::Keys: return "大千鍵"; case BrlOutput::Text: return "文字";
    case BrlOutput::Backspace: return "Backspace"; case BrlOutput::Enter: return "Enter"; case BrlOutput::Error: return "拒絕";
    }
    return "?";
}

LONGLONG g_tInjected = 0, g_tSpoken = 0;   // main thread: stamps of the last Act, for the timing line

// Do what the engine said - one SendInput for all of it, with `trailing`
// (Space or Enter the hook took) at the end - then log it and hand the words
// to the speech thread. Text that was waiting is typed before a Backspace or
// Enter, and before the refusal sound.
void Act(const BrlOutput& o, const char* what, WORD trailing = 0) {
    Injector inj;
    switch (o.action) {
    case BrlOutput::None:      break;
    case BrlOutput::Keys:      inj.Keys(o.text); break;
    case BrlOutput::Text:      inj.Text(o.text); break;
    case BrlOutput::Backspace: if (!o.text.empty()) inj.Text(o.text); inj.Key(VK_BACK); break;
    case BrlOutput::Enter:     if (!o.text.empty()) inj.Text(o.text); inj.Key(VK_RETURN); break;
    case BrlOutput::Error:     if (!o.text.empty()) inj.Text(o.text); break;
    }
    if (trailing) inj.Key(trailing);
    inj.Send();
    g_tInjected = Qpc();
    if (o.action == BrlOutput::Error) Refuse();
    // What was typed is invisible to IMEHelper and to JAWS's own echo (both
    // ignore injected keys), so it is said here, in JAWS's voice - queued to
    // the speech thread, never waited for.
    if (!o.spoken.empty()) Say(o.spoken.c_str());
    g_tSpoken = Qpc();
    g_waiting.store(g_engine.HasPendingPunct() || g_engine.HasPendingCells());
    UiaLog("點字鍵盤輸入: %s -> %s 「%s」 唸「%s」 注入=%s%s%s", what, ActionName(o.action), Utf8(o.text).c_str(), Utf8(o.spoken).c_str(), inj.How(),
           g_engine.InNemeth() ? " Nemeth" : "", g_engine.HasPendingCells() ? " 等下一方" : "");
}

void OnCell(unsigned dots, LONGLONG tPosted) {
    LONGLONG tReceived = Qpc();
    if (!g_enabled.load() || !g_editable.load()) return;
    if (dots == kSpaceCommit) {
        // The space bar was taken: a Chinese mark is confirmed by it and it
        // is consumed; otherwise the waiting cells are typed and the space
        // goes on to the application, in the same SendInput.
        if (g_engine.HasPendingPunct()) Act(g_engine.CommitPunct(), "空白鍵確認標點");
        else Act(g_engine.Flush(), "空白鍵前先打出等待中的方", VK_SPACE);
        return;
    }
    if (dots == kEnterCommit) {
        BrlOutput f = g_engine.Flush();
        g_engine.PlainKey();
        Act(f, "Enter 前先打出等待中的方", VK_RETURN);
        return;
    }
    if (dots == kSpacePassed) { g_engine.Flush(); g_waiting.store(false); return; }    // a space ends a number
    if (dots == kEnterPassed) { g_engine.PlainKey(); g_waiting.store(false); return; }
    // Before this cell: how long the raw input queue took to hand the
    // previous cell's keys on (the last of them came back through the hook
    // at g_tLastOwnKeyUp). Time the application and the IME then spend on
    // them is not visible from here.
    static LONGLONG prevInjected = 0;
    long queueUs = -1;
    if (g_timing && prevInjected) {
        LONGLONG back = g_tLastOwnKeyUp.load();
        if (back > prevInjected) queueUs = Us(prevInjected, back);
    }
    long waitedUs = 0;
    ModeInfo m = CachedMode(&waitedUs);
    char what[96];
    snprintf(what, sizeof(what), "方 %s %s(%s 配置=%04X open=%d conv=%ld)", DotsText(dots).c_str(),
             m.mode == BrlMode::Chinese ? "中文" : "英文", m.why, m.layout, m.open, m.conv);
    BrlOutput o = g_engine.OnCell(dots, m.mode);
    Act(o, what);
    if (o.again) Act(g_engine.OnCell(dots, m.mode), "  同一方再處理");
    prevInjected = g_tInjected;
    // The safety after a cell: ask the IME once more, on the mode thread,
    // after it has had the keys - a switch nobody saw (the language bar, a
    // key that is not watched) is then caught one cell late instead of two
    // seconds late. Never waited for.
    if (o.action != BrlOutput::None) RequestMode("@方後", 30, false);
    if (g_timing)
        UiaLog("點字鍵盤輸入: 計時 方 %s 鉤子→主線 %ld µs、主線→注入完 %ld µs（含等模式 %ld µs）、注入完→語音排入 %ld µs、合計 %ld µs；上一方的鍵在輸入佇列 %ld µs",
               DotsText(dots).c_str(), Us(tPosted, tReceived), Us(tReceived, g_tInjected), waitedUs, Us(g_tInjected, g_tSpoken), Us(tPosted, g_tSpoken), queueUs);
}

// ------------------------------------------------------------------ the JAWS table
// Where JAWS is: the running jfw.exe's folder, else the newest version's
// Target in the registry, else the newest folder under Program Files.
std::wstring JawsDir() {
    if (HWND w = FindWindowW(L"JFWUI2", nullptr)) {
        DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
            BOOL ok = QueryFullProcessImageNameW(h, 0, path, &n);
            CloseHandle(h);
            if (ok) { std::wstring p(path); size_t s = p.find_last_of(L'\\'); if (s != std::wstring::npos) return p.substr(0, s); }
        }
    }
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Freedom Scientific\\JAWS", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t name[64]; DWORD len = 64, best = 0; std::wstring found;
        for (DWORD i = 0; RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS; ++i) {
            DWORD year = (DWORD)_wtoi(name);
            if (year > best) { best = year; found = name; }
            len = 64;
        }
        RegCloseKey(key);
        if (!found.empty()) {
            std::wstring sub = L"SOFTWARE\\Freedom Scientific\\JAWS\\" + found;
            wchar_t target[MAX_PATH] = {}; DWORD cb = sizeof(target), type = 0;
            if (RegGetValueW(HKEY_LOCAL_MACHINE, sub.c_str(), L"Target", RRF_RT_REG_SZ, &type, target, &cb) == ERROR_SUCCESS && target[0]) {
                std::wstring t(target);
                while (!t.empty() && t.back() == L'\\') t.pop_back();
                return t;
            }
        }
    }
    wchar_t pf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
        std::wstring base = std::wstring(pf) + L"\\Freedom Scientific\\JAWS";
        WIN32_FIND_DATAW fd; std::wstring found; int best = 0;
        HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && iswdigit(fd.cFileName[0])) {
                    int v = _wtoi(fd.cFileName); if (v > best) { best = v; found = fd.cFileName; }
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!found.empty()) return base + L"\\" + found;
    }
    return {};
}

bool g_tableFromFile = false;

// US_Unicode.jbt: the 8-dot US computer braille table JAWS itself uses for
// braille input (its back-translator for enu, chs and most languages; the
// cht display table Taiwanese.jbt carries the identical ASCII section but
// no [input] section and no Special Symbols). Read once; the embedded copy
// of the same file is what runs until then.
void LoadJawsTable() {
    if (g_tableFromFile) return;
    std::wstring dir = JawsDir();
    if (dir.empty()) { UiaLog("點字鍵盤輸入: 找不到 JAWS 安裝夾，英文表用內建的 US_Unicode（%d 方）", BrlEnglishTableSize()); return; }
    std::wstring path = dir + L"\\US_Unicode.jbt";
    if (BrlLoadJbt(path.c_str())) {
        g_tableFromFile = true;
        UiaLog("點字鍵盤輸入: 英文表讀自 %s（%d 方）", Utf8(path).c_str(), BrlEnglishTableSize());
    } else {
        UiaLog("點字鍵盤輸入: 讀不到 %s，英文表用內建的 US_Unicode（%d 方）", Utf8(path).c_str(), BrlEnglishTableSize());
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\JAWS-CHT-BrailleKey");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    LARGE_INTEGER f; if (QueryPerformanceFrequency(&f) && f.QuadPart) g_qpf = f.QuadPart;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_mainThread.store(GetCurrentThreadId());
    g_recheck   = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_modeFresh = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_sayEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_enabled.store(LoadEnabled());
    g_dryRun = GetEnvironmentVariableW(L"JAWSCHT_BRAILLE_DRYRUN", nullptr, 0) != 0;
    g_forceUnicode = GetEnvironmentVariableW(L"JAWSCHT_BRAILLE_UNICODE", nullptr, 0) != 0;
    g_timing = GetEnvironmentVariableW(L"JAWSCHT_BRAILLE_TIMING", nullptr, 0) != 0;
    // The shell starts the deployed copy without our environment: a file
    // braille_timing.txt beside the log turns the timing lines on as well.
    if (!g_timing) {
        if (const char* dir = DiagDir()) {
            char flag[MAX_PATH]; snprintf(flag, sizeof(flag), "%s\\braille_timing.txt", dir);
            g_timing = GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES;
        }
    }
    UiaLog("點字鍵盤輸入: 已啟動，狀態 %s%s%s%s", g_enabled.load() ? "開啟" : "關閉", g_dryRun ? "（試跑模式，不吃鍵）" : "",
           g_forceUnicode ? "（英文全走 Unicode 注入）" : "", g_timing ? "（計時記錄）" : "");
    LoadJawsTable();
    UiaLog("點字鍵盤輸入: 中文表：音節 %d 筆（Phn.tbl）、標點符號 %d 條（國語點字，來源 BrlIMEHelper 資料表）", BrlSyllableTableSize(), BrlChineseSymbolCount());
    UiaLog("點字鍵盤輸入: Nemeth 表：%d 條（JAWS Liblouis nemeth 表加 BKey Sign.tbl）", BrlNemethSymbolCount());

    HANDLE speechThread = CreateThread(nullptr, 0, SpeechThread, nullptr, 0, nullptr);
    HANDLE modeThread   = CreateThread(nullptr, 0, ModeThread, nullptr, 0, nullptr);
    HANDLE hookThread   = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    HANDLE watcher      = CreateThread(nullptr, 0, FocusWatcher, nullptr, 0, nullptr);
    (void)modeThread; (void)hookThread; (void)watcher;

    if (!RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_NOREPEAT, VK_F12))
        UiaLog("點字鍵盤輸入: Ctrl+F12 註冊失敗 (%lu)", GetLastError());

    // Announce on every JAWS start while the mode is on; never when it is off.
    bool jawsWasUp = false;
    SetTimer(nullptr, 1, 1000, nullptr);

    // The JAWS edition lives and dies with JAWS: the shell (cht-shell) sets
    // this event when JAWS has been gone for a few seconds, and this
    // process leaves at once, hook and all. The stand-alone edition
    // (Braille Typewriter) has no shell, so nobody sets it.
    wchar_t own[MAX_PATH] = {}; GetModuleFileNameW(nullptr, own, MAX_PATH);
    const wchar_t* ownName = wcsrchr(own, L'\\'); ownName = ownName ? ownName + 1 : own;
    HANDLE quitEvent = CreateEventW(nullptr, TRUE, FALSE, (std::wstring(L"Local\\JAWS-CHT-Quit-") + ownName).c_str());

    MSG msg;
    for (bool running = true; running;) {
        DWORD w = MsgWaitForMultipleObjects(quitEvent ? 1 : 0, &quitEvent, FALSE, INFINITE, QS_ALLINPUT);
        if (quitEvent && w == WAIT_OBJECT_0) { UiaLog("點字鍵盤輸入: 殼程式要求結束（JAWS 已關閉）"); break; }
        while (running && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { running = false; break; }
        switch (msg.message) {
        case WM_HOTKEY:
            if (msg.wParam == 1) SetEnabled(!g_enabled.load(), true);
            break;
        case WM_APP_CELL:
            OnCell((unsigned)msg.wParam, (LONGLONG)msg.lParam);
            break;
        case WM_APP_PLAINKEY:
            g_engine.PlainKey();
            g_waiting.store(false);
            break;
        case WM_APP_REFRESH:
            ApplyHookWish();
            break;
        case WM_TIMER: {
            bool up = JawsRunning();
            if (up != jawsWasUp) {
                jawsWasUp = up;
                if (!up) SpeechCommand(true, false);
                else {
                    LoadJawsTable();   // JAWS was not there at start-up: its folder is known now
                    if (g_enabled.load()) Say(L"點字鍵盤輸入已開啟", 1500);   // the speech thread waits, not this one
                }
            }
            break;
        }
        default:
            DispatchMessageW(&msg);
        }
        }
    }
    if (quitEvent) CloseHandle(quitEvent);
    UnregisterHotKey(nullptr, 1);
    SpeechCommand(false, true);
    if (speechThread) { WaitForSingleObject(speechThread, 2000); CloseHandle(speechThread); }
    CoUninitialize();
    CloseHandle(mutex);
    return 0;
}
