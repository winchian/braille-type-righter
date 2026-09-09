// JAWS API wrapper — COM only (FreedomSci.JawsApi)
// JAWS API 封裝 — 純 COM（FreedomSci.JawsApi）
//
// 2026-08-18: the jfwapi.dll direct-call path was removed entirely. It was
// the DLL path's own ANSI-codepage round-trip that mangled Chinese in the
// first place (see SayString's history in the .cpp), and keeping it as a
// "faster" fallback meant every failure mode of JAWS's own automation object
// silently fell back to a path that speaks corrupted Chinese instead of
// failing loudly. One path, one behavior to reason about.
#pragma once

#include <windows.h>
#include <string>

class JawsCom {
public:
    JawsCom();
    ~JawsCom();

    // Existence check only - does not touch COM. See EnsureCom() for why the
    // COM object itself is never created here.
    bool Init();
    void Shutdown();
    bool IsConnected() const;

    // Writes to the log which connection path failed and why. Call after
    // Init() returns false.
    void LogInitFailure() const;

    // Speak text through JAWS
    // 透過 JAWS 朗讀文字
    bool SayString(const std::wstring& text, bool flush = true);

    // Run a JAWS script function
    // 執行 JAWS 腳本函式
    bool RunFunction(const std::wstring& func);

    // Stop current speech
    // 停止目前朗讀
    bool StopSpeech();

private:
    IDispatch* m_pJaws;
    bool m_comConnected;
    // Last attempt at establishing the COM connection, so a retry on every
    // announcement does not turn into a failed CoCreateInstance per keystroke
    // on a machine where COM is genuinely unavailable.
    unsigned long m_lastComTry = 0;

    // Creates m_pJaws if not already connected. Must only ever be called from
    // the thread that will go on to call Invoke on it - COM refuses a call
    // made from outside the apartment that owns the object (RPC_E_WRONG_
    // THREAD), which is exactly what happened when this was once created
    // from JawsWatchdog while SayString ran on the message-loop thread. Every
    // public method here calls this itself rather than relying on Init(), so
    // every one of them stays correct as long as its caller is consistent
    // about which thread it calls from.
    bool EnsureCom();
    HRESULT InvokeMethod(const wchar_t* method, VARIANT* args, int argc);
};
