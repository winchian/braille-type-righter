// JAWS API wrapper — COM only (FreedomSci.JawsApi)
// JAWS API 封裝 — 純 COM（FreedomSci.JawsApi）
#include "jaws_com.h"
#include "shimlog_local.h"
#include <oleauto.h>

// 2026-08-18: source documents (confirmed against a Bible passage sample)
// sometimes carry real Han characters replaced with their Kangxi Radical
// look-alikes - visually near-identical but a different Unicode block
// (U+2E80-U+2FDF) with no pronunciation of its own, so 磐石 arrives as 磐⽯
// and no synthesizer can read the second character. This is not a codepage
// problem - it round-trips through any codepage unharmed - it is the wrong
// codepoint from the start, most likely from whatever tool produced the
// source text.
//
// Standard Unicode NFKC compatibility normalization already maps every
// radical form to its real ideograph (verified: U+2F6F KANGXI RADICAL STONE
// -> U+77F3 石, and so on for all nine forms found in the sample) - this is
// exactly the kind of substitution NFKC exists for, not something needing a
// hand-built table. NVDA reads these correctly, and this is the likely
// reason why: normalize before speaking, the same as any other Unicode
// consumer that wants text in its canonical form.
// CJK Radicals Supplement (U+2E80-U+2EFF) has no NFKC decomposition at all -
// confirmed by testing every codepoint in the block. Unicode leaves these
// undecomposed on purpose: a radical is a component, not officially a
// substitute for the character it names, so giving it a compatibility
// mapping would be wrong for text that uses it as an actual radical (e.g. a
// dictionary entry describing "the water radical"). But source documents
// that used it as a stand-in for the real character - the same mistake as
// the Kangxi Radicals block - are a real, observed case (2026-08-18, a
// Traditional Chinese Bible passage), so it needs the same fix by hand.
//
// This block also covers the Simplified-Chinese compressed radical forms
// (the "C-SIMPLIFIED ..." Unicode names, e.g. U+2EC8 for 讠/言) - the same
// substitution shows up in Simplified text for the same reason.
//
// Table built from each codepoint's Unicode name matched to its Kangxi
// Radical counterpart (which NFKC does decompose) where one exists by name;
// hand-filled for the Simplified-only forms Unicode has no Kangxi match for.
// A few entries were changed from the technically-corresponding radical
// character to a more common one with the same meaning (e.g. 艸 -> 草, 靑 ->
// 青) purely for reliable pronunciation - which specific character is
// substituted does not matter for polyphones, since downstream tone lookup
// runs on whatever real character comes out of this table exactly as it
// would for ordinarily-typed text.
struct RadicalFix { wchar_t from; wchar_t to; };
static const RadicalFix kRadicalFixes[] = {
    {0x2E80, L'己'}, {0x2E81, L'厂'}, {0x2E82, L'乙'}, {0x2E83, L'乙'},
    {0x2E84, L'乙'}, {0x2E85, L'人'}, {0x2E86, L'冂'}, {0x2E87, L'几'},
    {0x2E88, L'刀'}, {0x2E89, L'刀'}, {0x2E8A, L'卜'}, {0x2E8B, L'卩'},
    {0x2E8C, L'小'}, {0x2E8D, L'小'}, {0x2E8E, L'尢'}, {0x2E8F, L'尢'},
    {0x2E90, L'尢'}, {0x2E91, L'尢'}, {0x2E92, L'虫'}, {0x2E93, L'絲'},
    {0x2E94, L'彐'}, {0x2E95, L'彐'}, {0x2E96, L'心'}, {0x2E97, L'心'},
    {0x2E98, L'手'}, {0x2E99, L'攴'}, {0x2E9B, L'火'}, {0x2E9C, L'日'},
    {0x2E9D, L'月'}, {0x2E9E, L'歹'}, {0x2E9F, L'母'}, {0x2EA0, L'民'},
    {0x2EA1, L'水'}, {0x2EA2, L'水'}, {0x2EA3, L'火'}, {0x2EA4, L'爪'},
    {0x2EA5, L'爪'}, {0x2EA6, L'木'}, {0x2EA7, L'牛'}, {0x2EA8, L'犬'},
    {0x2EA9, L'玉'}, {0x2EAA, L'疋'}, {0x2EAB, L'目'}, {0x2EAC, L'示'},
    {0x2EAD, L'示'}, {0x2EAE, L'竹'}, {0x2EAF, L'絲'}, {0x2EB0, L'絲'},
    {0x2EB1, L'网'}, {0x2EB2, L'网'}, {0x2EB3, L'网'}, {0x2EB4, L'网'},
    {0x2EB5, L'网'}, {0x2EB6, L'羊'}, {0x2EB7, L'羊'}, {0x2EB8, L'羊'},
    {0x2EB9, L'老'}, {0x2EBA, L'聿'}, {0x2EBB, L'聿'}, {0x2EBC, L'肉'},
    {0x2EBD, L'臼'}, {0x2EBE, L'草'}, {0x2EBF, L'草'}, {0x2EC0, L'草'},
    {0x2EC1, L'虎'}, {0x2EC2, L'衣'}, {0x2EC3, L'西'}, {0x2EC4, L'西'},
    {0x2EC5, L'見'}, {0x2EC6, L'角'}, {0x2EC7, L'角'}, {0x2EC8, L'言'},
    {0x2EC9, L'貝'}, {0x2ECA, L'足'}, {0x2ECB, L'車'}, {0x2ECC, L'走'},
    {0x2ECD, L'走'}, {0x2ECE, L'走'}, {0x2ECF, L'邑'}, {0x2ED0, L'金'},
    {0x2ED1, L'長'}, {0x2ED2, L'長'}, {0x2ED3, L'長'}, {0x2ED4, L'門'},
    {0x2ED5, L'阜'}, {0x2ED6, L'阜'}, {0x2ED7, L'雨'}, {0x2ED8, L'青'},
    {0x2ED9, L'革'}, {0x2EDA, L'頁'}, {0x2EDB, L'風'}, {0x2EDC, L'飛'},
    {0x2EDD, L'食'}, {0x2EDE, L'食'}, {0x2EDF, L'食'}, {0x2EE0, L'食'},
    {0x2EE1, L'首'}, {0x2EE2, L'馬'}, {0x2EE3, L'骨'}, {0x2EE4, L'鬼'},
    {0x2EE5, L'魚'}, {0x2EE6, L'鳥'}, {0x2EE7, L'鹵'}, {0x2EE8, L'麥'},
    {0x2EE9, L'黃'}, {0x2EEA, L'蛙'}, {0x2EEB, L'齊'}, {0x2EEC, L'齊'},
    {0x2EED, L'齒'}, {0x2EEE, L'齒'}, {0x2EEF, L'龍'}, {0x2EF0, L'龍'},
    {0x2EF1, L'龜'}, {0x2EF2, L'龜'}, {0x2EF3, L'龜'},
};

static wchar_t FixRadical(wchar_t c) {
    if (c < 0x2E80 || c > 0x2EFF) return c;
    for (const auto& f : kRadicalFixes)
        if (f.from == c) return f.to;
    return c;
}

static std::wstring NormalizeForSpeech(const std::wstring& text) {
    std::wstring fixed = text;
    for (wchar_t& c : fixed) c = FixRadical(c);

    int need = NormalizeString(NormalizationKC, fixed.c_str(), (int)fixed.size(), nullptr, 0);
    if (need <= 0) return fixed;  // already-normalized or non-normalizable text: speak as-is

    std::wstring out(need, L'\0');
    int written = NormalizeString(NormalizationKC, fixed.c_str(), (int)fixed.size(), &out[0], need);
    if (written <= 0) return fixed;
    out.resize(written);
    return out;
}

JawsCom::JawsCom()
    : m_pJaws(nullptr)
    , m_comConnected(false)
{}

JawsCom::~JawsCom() {
    Shutdown();
}

bool JawsCom::EnsureCom() {
    if (m_comConnected && m_pJaws) return true;

    // Rate-limited: a machine where JAWS's automation object genuinely never
    // registers must not pay for a failed CoCreateInstance on every keystroke.
    DWORD now = GetTickCount();
    if (now - m_lastComTry < 2000) return false;
    m_lastComTry = now;

    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"FreedomSci.JawsApi", &clsid);
    if (FAILED(hr)) return false;

    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                          IID_IDispatch, (void**)&m_pJaws);
    if (FAILED(hr) || !m_pJaws) {
        m_pJaws = nullptr;
        return false;
    }

    m_comConnected = true;
    UiaLog("語音走 COM（BSTR，不經字碼頁）");
    return true;
}

bool JawsCom::Init() {
    // Existence check only. The COM object itself is created lazily by
    // EnsureCom(), on whichever thread actually calls SayString/RunFunction/
    // StopSpeech - never here. This runs on JawsWatchdog, a thread of its own
    // that never calls CoInitializeEx; creating m_pJaws there once left the
    // interface pointer owned by an apartment the message-loop thread could
    // not call into, and every announcement failed with RPC_E_WRONG_THREAD
    // (0x8001010E) until JAWS was restarted. The watchdog only needs to know
    // whether there is a JAWS to serve, which its own FindJawsPid() already
    // established before calling this.
    return true;
}

void JawsCom::Shutdown() {
    if (m_pJaws) {
        m_pJaws->Release();
        m_pJaws = nullptr;
    }
    m_comConnected = false;
}

bool JawsCom::IsConnected() const {
    return m_comConnected && m_pJaws;
}

void JawsCom::LogInitFailure() const {
    UiaLog("JAWS COM 連線失敗: FreedomSci.JawsApi 尚未就緒（JAWS 可能還在啟動中）");
}

HRESULT JawsCom::InvokeMethod(const wchar_t* method, VARIANT* args, int argc) {
    if (!m_pJaws) return E_POINTER;

    // The dispatch id is looked up once per method and kept.
    //
    // GetIDsOfNames is not a local operation - JAWS's automation object lives
    // in another process, so every lookup is an RPC round trip. Calling it
    // before each announcement doubled the cross-process traffic for no gain:
    // a method's id is fixed for the lifetime of the object. With one Zhuyin
    // symbol announced per keystroke, that saved round trip is felt directly
    // as typing latency.
    struct Cached { const wchar_t* name; DISPID id; };
    static Cached cache[8] = {};
    static int cached = 0;

    DISPID dispid = 0;
    bool found = false;
    for (int i = 0; i < cached; ++i) {
        if (wcscmp(cache[i].name, method) == 0) { dispid = cache[i].id; found = true; break; }
    }
    if (!found) {
        LPOLESTR name = const_cast<LPOLESTR>(method);
        HRESULT hr = m_pJaws->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &dispid);
        if (FAILED(hr)) return hr;
        if (cached < (int)(sizeof(cache) / sizeof(cache[0])))
            cache[cached++] = { method, dispid };
    }

    DISPPARAMS params = {};
    params.cArgs = argc;
    params.rgvarg = args;

    EXCEPINFO excep = {};
    HRESULT hr = m_pJaws->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                 DISPATCH_METHOD, &params, nullptr, &excep, nullptr);

    if (hr == DISP_E_EXCEPTION) {
        SysFreeString(excep.bstrSource);
        SysFreeString(excep.bstrDescription);
        SysFreeString(excep.bstrHelpFile);
    }

    return hr;
}

bool JawsCom::SayString(const std::wstring& text, bool flush) {
    if (text.empty()) return false;
    if (!EnsureCom()) return false;

    std::wstring normalized = NormalizeForSpeech(text);

    VARIANT a[2];
    VariantInit(&a[0]);
    VariantInit(&a[1]);
    a[0].vt = VT_BOOL;
    a[0].boolVal = flush ? VARIANT_TRUE : VARIANT_FALSE;

    BSTR bs = SysAllocString(normalized.c_str());
    if (!bs) return false;
    a[1].vt = VT_BSTR;
    a[1].bstrVal = bs;

    HRESULT hr = InvokeMethod(L"SayString", a, 2);
    SysFreeString(bs);

    if (FAILED(hr)) {
        static DWORD lastFailLog = 0;
        DWORD t = GetTickCount();
        if (t - lastFailLog > 5000) {
            lastFailLog = t;
            UiaLog("COM SayString 失敗 hr=0x%08lX", (unsigned long)hr);
        }
        // A failed Invoke can mean the object died (JAWS restarting) -
        // dropping the stale pointer lets the next call reconnect instead of
        // repeating the same failure until the watchdog notices JAWS is gone.
        Shutdown();
        return false;
    }
    return true;
}

bool JawsCom::RunFunction(const std::wstring& func) {
    if (func.empty()) return false;
    if (!EnsureCom()) return false;

    VARIANT args[1];
    VariantInit(&args[0]);

    BSTR bstr = SysAllocString(func.c_str());
    if (!bstr) return false;
    args[0].vt = VT_BSTR;
    args[0].bstrVal = bstr;

    HRESULT hr = InvokeMethod(L"RunFunction", args, 1);
    SysFreeString(bstr);
    if (FAILED(hr)) Shutdown();
    return SUCCEEDED(hr);
}

bool JawsCom::StopSpeech() {
    if (!EnsureCom()) return false;
    HRESULT hr = InvokeMethod(L"StopSpeech", nullptr, 0);
    if (FAILED(hr)) Shutdown();
    return SUCCEEDED(hr);
}
