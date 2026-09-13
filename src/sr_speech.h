// sr_speech.h - say something out loud through whatever is on the machine.
//
// The stand-alone edition runs where there may be no JAWS at all: NVDA,
// Narrator, another screen reader, or nobody. Paul 2026-09-13:
// 「NVDA 在的時候按 Ctrl+F12 也要能開關它」「一般模式下有人按到 Ctrl+F12 也能開，
// 但就要發出聲音」「這套輸入法在朗讀軟體上也要能用」.
//
// Two paths, chosen per utterance, no configuration:
//
//   A screen reader is running (SPI_GETSCREENREADER)
//       -> a UI Automation notification event. This is the operating
//          system's own way for a program to hand a screen reader something
//          to say; NVDA and Narrator speak it. We raise it from a hidden
//          1x1 window of our own, so nothing about any other program's
//          accessibility is touched - no properties rewritten, no focus
//          moved, no window shown (WS_EX_TOOLWINDOW, never ShowWindow, so
//          it is not in Alt+Tab or the taskbar).
//
//   No screen reader
//       -> the system voice (SAPI 5, through its automation interface so
//          this file needs no speech SDK header). A Chinese voice is asked
//          for first: on a machine with only an English voice the text
//          would otherwise come out as silence.
//
// The JAWS edition of this program does not use any of this: it talks to
// JAWS directly, and while JAWS is running the stand-alone edition stands
// down completely.
//
// Nothing here is created until it is needed, and Disable() takes the
// window away again, so a machine where this program is dormant has no
// window and no COM object belonging to it.
#pragma once
#include <windows.h>
#include <uiautomation.h>
#include <oleauto.h>
#include <atomic>
#include <string>

namespace srspeech {

// ---- the bits of UIAutomationCore we use, loaded by name ----
// Declared here rather than taken from the SDK headers so that the program
// builds against any mingw-w64 vintage; UiaRaiseNotificationEvent is
// Windows 10 1607 and later, and its absence is simply "no UIA path".
typedef HRESULT (WINAPI *PfnRaiseNotification)(IRawElementProviderSimple*, int, int, BSTR, BSTR);
typedef LRESULT (WINAPI *PfnReturnProvider)(HWND, WPARAM, LPARAM, IRawElementProviderSimple*);
typedef HRESULT (WINAPI *PfnHostFromHwnd)(HWND, IRawElementProviderSimple**);

const int kNotificationKindOther      = 4;   // NotificationKind_Other
const int kNotificationMostRecent     = 3;   // NotificationProcessing_MostRecent: a newer one replaces a queued one
const LONG kUiaRootObjectId           = -25;

inline HMODULE Core() {
    static HMODULE m = LoadLibraryW(L"UIAutomationCore.dll");
    return m;
}
inline PfnRaiseNotification RaiseNotification() {
    static PfnRaiseNotification f = Core() ? (PfnRaiseNotification)(void*)GetProcAddress(Core(), "UiaRaiseNotificationEvent") : nullptr;
    return f;
}
inline PfnReturnProvider ReturnProvider() {
    static PfnReturnProvider f = Core() ? (PfnReturnProvider)(void*)GetProcAddress(Core(), "UiaReturnRawElementProvider") : nullptr;
    return f;
}
inline PfnHostFromHwnd HostFromHwnd() {
    static PfnHostFromHwnd f = Core() ? (PfnHostFromHwnd)(void*)GetProcAddress(Core(), "UiaHostProviderFromHwnd") : nullptr;
    return f;
}

// ---- the provider ----
// The smallest thing UI Automation accepts as an element: it answers "I am a
// server-side provider for this window" and nothing else. It exists only so
// that notification events have a sender.
class Provider : public IRawElementProviderSimple {
public:
    explicit Provider(HWND h) : hwnd_(h) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
            *ppv = static_cast<IRawElementProviderSimple*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { long n = --ref_; if (!n) delete this; return (ULONG)n; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* o) override {
        if (!o) return E_POINTER;
        *o = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** p) override {
        if (p) *p = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* v) override {
        if (!v) return E_POINTER;
        VariantInit(v);
        if (id == UIA_ControlTypePropertyId) { v->vt = VT_I4; v->lVal = UIA_WindowControlTypeId; }
        else if (id == UIA_NamePropertyId)   { v->vt = VT_BSTR; v->bstrVal = SysAllocString(L"Braille Typewriter"); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** p) override {
        if (!p) return E_POINTER;
        *p = nullptr;
        PfnHostFromHwnd f = HostFromHwnd();
        return f ? f(hwnd_, p) : (HRESULT)S_OK;
    }
private:
    virtual ~Provider() = default;
    HWND hwnd_;
    std::atomic<long> ref_{1};
};

// ---- the hidden window that owns the provider ----
inline Provider*& TheProvider() { static Provider* p = nullptr; return p; }
inline HWND& TheWindow() { static HWND h = nullptr; return h; }

inline LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_GETOBJECT && (LONG)l == kUiaRootObjectId) {
        PfnReturnProvider f = ReturnProvider();
        Provider* p = TheProvider();
        if (f && p) return f(h, w, l, p);
    }
    return DefWindowProcW(h, m, w, l);
}

// Create the window (main thread, the one with the message loop). Cheap to
// call again; returns false when the window could not be made, in which
// case Speak falls back to the system voice.
inline bool Enable() {
    if (TheWindow()) return true;
    if (!RaiseNotification() || !ReturnProvider()) return false;   // too old a Windows: system voice only
    static ATOM atom = 0;
    if (!atom) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"BrailleTypewriterNotify";
        atom = RegisterClassExW(&wc);
        if (!atom) return false;
    }
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"BrailleTypewriterNotify", L"Braille Typewriter",
                             WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!h) return false;
    TheWindow() = h;
    TheProvider() = new Provider(h);
    return true;
}

// Take the window away again (main thread). Used while the JAWS edition has
// taken over: a dormant program should own nothing.
inline void Disable() {
    if (TheProvider()) { TheProvider()->Release(); TheProvider() = nullptr; }
    if (TheWindow()) { DestroyWindow(TheWindow()); TheWindow() = nullptr; }
}

inline bool ScreenReaderPresent() {
    BOOL on = FALSE;
    return SystemParametersInfoW(SPI_GETSCREENREADER, 0, &on, 0) != 0 && on;
}

inline bool SpeakViaUia(const std::wstring& text) {
    PfnRaiseNotification f = RaiseNotification();
    Provider* p = TheProvider();
    if (!f || !p) return false;
    BSTR msg = SysAllocString(text.c_str());
    BSTR activity = SysAllocString(L"BrailleTypewriter");
    HRESULT hr = f(p, kNotificationKindOther, kNotificationMostRecent, msg, activity);
    SysFreeString(activity);
    SysFreeString(msg);
    return SUCCEEDED(hr);
}

// ---- the system voice ----
// SAPI 5 through its automation interface (ProgID SAPI.SpVoice), so no
// speech SDK header is needed. Speech-thread only: the object is kept for
// the life of the program rather than created per utterance.
inline bool InvokeByName(IDispatch* obj, const wchar_t* name, WORD flags, DISPPARAMS* args, VARIANT* out) {
    if (!obj) return false;
    DISPID id = 0;
    OLECHAR* n = const_cast<OLECHAR*>(name);
    if (FAILED(obj->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id))) return false;
    DISPID putId = DISPID_PROPERTYPUT;
    if (flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) { args->rgdispidNamedArgs = &putId; args->cNamedArgs = 1; }
    return SUCCEEDED(obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, args, out, nullptr, nullptr));
}

// Ask the voice collection for a Chinese voice and select it. Failing that,
// whatever the user has stays.
inline void PreferChineseVoice(IDispatch* voice) {
    for (const wchar_t* want : { L"Language=404", L"Language=804" }) {
        VARIANT arg; VariantInit(&arg); arg.vt = VT_BSTR; arg.bstrVal = SysAllocString(want);
        DISPPARAMS dp = { &arg, nullptr, 1, 0 };
        VARIANT tokens; VariantInit(&tokens);
        bool got = InvokeByName(voice, L"GetVoices", DISPATCH_METHOD, &dp, &tokens);
        VariantClear(&arg);
        if (!got || tokens.vt != VT_DISPATCH || !tokens.pdispVal) { VariantClear(&tokens); continue; }
        VARIANT count; VariantInit(&count);
        DISPPARAMS none = { nullptr, nullptr, 0, 0 };
        bool haveCount = InvokeByName(tokens.pdispVal, L"Count", DISPATCH_PROPERTYGET | DISPATCH_METHOD, &none, &count);
        long n = (haveCount && count.vt == VT_I4) ? count.lVal : 0;
        VariantClear(&count);
        if (n > 0) {
            VARIANT idx; VariantInit(&idx); idx.vt = VT_I4; idx.lVal = 0;
            DISPPARAMS one = { &idx, nullptr, 1, 0 };
            VARIANT token; VariantInit(&token);
            if (InvokeByName(tokens.pdispVal, L"Item", DISPATCH_PROPERTYGET | DISPATCH_METHOD, &one, &token) &&
                token.vt == VT_DISPATCH && token.pdispVal) {
                VARIANT put; VariantInit(&put); put.vt = VT_DISPATCH; put.pdispVal = token.pdispVal;
                DISPPARAMS setArgs = { &put, nullptr, 1, 0 };
                InvokeByName(voice, L"Voice", DISPATCH_PROPERTYPUTREF, &setArgs, nullptr);
            }
            VariantClear(&token);
        }
        VariantClear(&tokens);
        if (n > 0) return;
    }
}

inline bool SpeakViaSystemVoice(const std::wstring& text) {
    static IDispatch* voice = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        CLSID clsid;
        if (SUCCEEDED(CLSIDFromProgID(L"SAPI.SpVoice", &clsid))) {
            if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice)))) voice = nullptr;
            if (voice) PreferChineseVoice(voice);
        }
    }
    if (!voice) return false;
    VARIANT args[2];                                  // reversed: flags first, then the text
    VariantInit(&args[0]); args[0].vt = VT_I4; args[0].lVal = 1 | 2;   // async, purge whatever is being said
    VariantInit(&args[1]); args[1].vt = VT_BSTR; args[1].bstrVal = SysAllocString(text.c_str());
    DISPPARAMS dp = { args, nullptr, 2, 0 };
    bool ok = InvokeByName(voice, L"Speak", DISPATCH_METHOD, &dp, nullptr);
    VariantClear(&args[1]);
    return ok;
}

// Speech-thread only.
inline bool Speak(const std::wstring& text) {
    if (text.empty()) return true;
    if (ScreenReaderPresent() && SpeakViaUia(text)) return true;
    return SpeakViaSystemVoice(text);
}

}  // namespace srspeech
