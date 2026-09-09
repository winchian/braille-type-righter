#include "brl_engine.h"

#include <cstdio>
#include <cstring>
#include <cwctype>

namespace {

// Cell value (dots 1-6 as bits) -> North American ASCII braille. The standard
// 64-character table in cell-value order; index = the bit mask. Used by the
// Chinese syllable table (BKey writes cells this way) and by the BKey
// mathematics sequences.
const char kAscii[65] = " a1b'k2l@cif/msp\"e3h9o6r^djg>ntq,*5<-u8v.%[$+x!&;:4\\0z7(_?w]#y)=";

unsigned AsciiToCell(char c) {
    for (unsigned v = 1; v < 64; ++v) if (kAscii[v] == c) return v;
    return 0;
}

struct Syllable { const char* cells; const char* keys; };
const Syllable kSyllables[] = {
#include "syllables.inc"
};
const int kSyllableCount = sizeof(kSyllables) / sizeof(kSyllables[0]);

const char* LookupSyllable(const std::string& cells) {
    for (int i = 0; i < kSyllableCount; ++i)
        if (cells == kSyllables[i].cells) return kSyllables[i].keys;
    return nullptr;
}

// A cell that begins a two-cell entry is an initial: it must wait for its rime.
bool IsInitial(char c) {
    for (int i = 0; i < kSyllableCount; ++i)
        if (kSyllables[i].cells[0] == c && kSyllables[i].cells[1] != 0) return true;
    return false;
}

// Tone cells of 國語點字 and the 大千 key that types each tone. The first
// tone is written with dot 3 in braille and typed with Space on the keyboard.
//   dot 3  ˉ  Space      dot 2  ˊ  6      dot 4  ˇ  3
//   dot 5  ˋ  4          dot 1  ˙  7
// Dot 1 is also the initial ㄓ; which one it is depends on position, and
// OnCell resolves that (a cell after a finished syllable is the tone).
struct Tone { unsigned dots; wchar_t key; const wchar_t* name; };
const Tone kTones[] = {
    { kDot3, L' ', L"一聲" }, { kDot2, L'6', L"二聲" }, { kDot4, L'3', L"三聲" },
    { kDot5, L'4', L"四聲" }, { kDot1, L'7', L"輕聲" },
};
const Tone* ToneFor(unsigned dots) {
    for (const auto& t : kTones) if (t.dots == dots) return &t;
    return nullptr;
}

// 大千 key -> the Zhuyin symbol it types -> the symbol's spoken name. Names are
// the ones IMEHelper's character database uses (zh_TW_char.ini 「注音符號，X」),
// so the braille typist hears the same words the keyboard typist does.
struct KeyName { char key; wchar_t zhuyin; const wchar_t* name; };
const KeyName kKeyNames[] = {
    { '1', L'ㄅ', L"波" }, { 'q', L'ㄆ', L"坡" }, { 'a', L'ㄇ', L"摸" }, { 'z', L'ㄈ', L"佛" },
    { '2', L'ㄉ', L"得" }, { 'w', L'ㄊ', L"特" }, { 's', L'ㄋ', L"訥" }, { 'x', L'ㄌ', L"勒" },
    { 'e', L'ㄍ', L"哥" }, { 'd', L'ㄎ', L"科" }, { 'c', L'ㄏ', L"喝" },
    { 'r', L'ㄐ', L"基" }, { 'f', L'ㄑ', L"欺" }, { 'v', L'ㄒ', L"希" },
    { '5', L'ㄓ', L"知" }, { 't', L'ㄔ', L"吃" }, { 'g', L'ㄕ', L"詩" }, { 'b', L'ㄖ', L"日" },
    { 'y', L'ㄗ', L"資" }, { 'h', L'ㄘ', L"雌" }, { 'n', L'ㄙ', L"思" },
    { '8', L'ㄚ', L"啊" }, { 'i', L'ㄛ', L"喔" }, { 'k', L'ㄜ', L"鵝" }, { ',', L'ㄝ', L"耶" },
    { '9', L'ㄞ', L"哀" }, { 'o', L'ㄟ', L"欸" }, { 'l', L'ㄠ', L"熬" }, { '.', L'ㄡ', L"歐" },
    { '0', L'ㄢ', L"安" }, { 'p', L'ㄣ', L"恩" }, { ';', L'ㄤ', L"昂" }, { '/', L'ㄥ', L"亨" },
    { '-', L'ㄦ', L"兒" }, { 'u', L'ㄧ', L"衣" }, { 'j', L'ㄨ', L"烏" }, { 'm', L'ㄩ', L"迂" },
};

std::wstring SpokenForKeys(const char* keys) {
    std::wstring s;
    for (; *keys; ++keys)
        for (const auto& k : kKeyNames)
            if (k.key == *keys) { if (!s.empty()) s += L' '; s += k.name; break; }
    return s;
}

// Chinese punctuation and symbols: the cell sequences of 國語點字 as the
// rule table of the BrlIMEHelper NVDA add-on (bopomofo.json, Bo-Cheng Jhan,
// GPL v3) lists them. Only that data - the cells the national braille
// standard assigns to each mark - is taken, through tools/gen_bopomofo_rules.py;
// none of the add-on's code is used or copied, since GPL code may not enter
// this product. A blank cell (0) in a sequence is the space bar: 逗號 is 2-3
// followed by Space, the confirmation Paul specified 2026-09-09.
struct TwSym { const char* cells; const wchar_t* text; const wchar_t* spoken; };
const TwSym kTwSyms[] = {
#include "tw_braille.inc"
};

struct TwSeq { std::vector<unsigned> cells; const wchar_t* text; const wchar_t* spoken; };
std::vector<TwSeq> g_tw;
bool g_twReady = false;

void EnsureTw() {
    if (g_twReady) return;
    g_twReady = true;
    for (const auto& t : kTwSyms) {
        TwSeq s; unsigned m = 0; bool any = false;
        for (const char* p = t.cells; ; ++p) {
            if (*p == '-' || *p == 0) { if (any) s.cells.push_back(m); m = 0; any = false; if (!*p) break; }
            else if (*p >= '1' && *p <= '8') { m |= 1u << (*p - '1'); any = true; }
            else if (*p == '0') any = true;          // the blank cell
        }
        if (s.cells.empty()) continue;
        s.text = t.text; s.spoken = t.spoken;
        g_tw.push_back(s);
    }
}

const TwSeq* TwExact(const std::vector<unsigned>& cells) {
    for (const auto& s : g_tw) if (s.cells == cells) return &s;
    return nullptr;
}

// Some symbol is longer than `cells` and starts with it.
bool TwPrefix(const std::vector<unsigned>& cells) {
    for (const auto& s : g_tw) {
        if (s.cells.size() <= cells.size()) continue;
        bool same = true;
        for (size_t i = 0; i < cells.size(); ++i) if (s.cells[i] != cells[i]) { same = false; break; }
        if (same) return true;
    }
    return false;
}

// Two rimes that are read by their tone when they stand alone, as the
// RHYMES rules of bopomofo.json have it: 3-5-6 is ㄧㄛ (唷) with the first or
// the neutral tone and ㄟ otherwise; 2-6 is ㄧㄞ (崖) with the second tone and
// ㄝ otherwise. Phn.tbl reads them as ㄟ and ㄧㄞ for every tone; the add-on's
// reading is followed here (docs/braille-table-comparison-2026-09-09.md).
struct ToneRime { unsigned dots; unsigned tones; const char* keys; const char* otherKeys; };
const ToneRime kToneRimes[] = {
    { kDot3 | kDot5 | kDot6, kDot3 | kDot1, "ui", "o" },
    { kDot2 | kDot6,         kDot2,         "u9", "," },
};
const ToneRime* ToneRimeFor(unsigned dots) {
    for (const auto& r : kToneRimes) if (r.dots == dots) return &r;
    return nullptr;
}

// ------------------------------------------------------------- English table
// 8-dot US computer braille, as JAWS's US_Unicode.jbt defines it. The
// embedded copy is generated from that file (tools/gen_tables.py); the live
// file is read over it at start-up when the JAWS installation is found.
struct JbtEntry { unsigned char dots; unsigned short cp; };
const JbtEntry kUsUnicode[] = {
#include "us_unicode.inc"
};

wchar_t g_english[256];
bool g_englishReady = false;

// Where Taiwanese braille practice differs from JAWS's US table, Taiwan wins
// - whichever way the table came in (embedded copy or the .jbt file).
// Paul, 2026-09-09: 1256 is the backslash (North American ASCII braille);
// JAWS gives 1256 to | and 12567 to \. Swapped. (tools/gen_tables.py applies
// the same swap to the embedded copy, so the two agree.)
void ApplyTaiwanOverrides() {
    const unsigned d1256 = kDot1 | kDot2 | kDot5 | kDot6;
    g_english[d1256] = L'\\';
    g_english[d1256 | kDot7] = L'|';
}

void EnsureEnglish() {
    if (g_englishReady) return;
    memset(g_english, 0, sizeof(g_english));
    for (const auto& e : kUsUnicode) g_english[e.dots] = (wchar_t)e.cp;
    ApplyTaiwanOverrides();
    g_englishReady = true;
}

// One line of a .jbt: `key=dots` where key is \N (decimal), u+XXXX, or one
// literal character; dots is digits 1-8. Returns false for anything else,
// including multi-cell entries (space-separated) and empty ones.
bool ParseJbtLine(const std::string& line, unsigned* cp, unsigned* mask) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    if (eq == 0 && line.size() > 1 && line[1] == '=') eq = 1;   // `==123456`: the = character
    std::string key = line.substr(0, eq), val = line.substr(eq + 1);
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
        size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        s.erase(0, i);
    };
    trim(key); trim(val);
    if (key.empty() || val.empty()) return false;
    unsigned m = 0;
    for (char c : val) { if (c < '1' || c > '8') return false; m |= 1u << (c - '1'); }
    unsigned code = 0;
    if (key[0] == '\\' && key.size() > 1) {
        for (size_t i = 1; i < key.size(); ++i) { if (!isdigit((unsigned char)key[i])) return false; code = code * 10 + (key[i] - '0'); }
    } else if (key.size() > 2 && (key[0] == 'u' || key[0] == 'U') && key[1] == '+') {
        for (size_t i = 2; i < key.size(); ++i) {
            char c = key[i]; unsigned d;
            if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else return false;
            code = code * 16 + d;
        }
    } else if (key.size() == 1) {
        code = (unsigned char)key[0];
    } else return false;
    *cp = code; *mask = m;
    return true;
}

// Spoken names of the ASCII signs, so that a blind typist hears 「逗號」 and
// not a pause. Letters and digits are spoken as themselves.
struct AsciiName { wchar_t ch; const wchar_t* name; };
const AsciiName kAsciiNames[] = {
    { L' ', L"空白" }, { L'!', L"驚嘆號" }, { L'"', L"引號" }, { L'#', L"井字號" }, { L'$', L"錢號" },
    { L'%', L"百分號" }, { L'&', L"和" }, { L'\'', L"撇號" }, { L'(', L"左括號" }, { L')', L"右括號" },
    { L'*', L"星號" }, { L'+', L"加" }, { L',', L"逗號" }, { L'-', L"連字號" }, { L'.', L"句點" },
    { L'/', L"斜線" }, { L':', L"冒號" }, { L';', L"分號" }, { L'<', L"小於" }, { L'=', L"等於" },
    { L'>', L"大於" }, { L'?', L"問號" }, { L'@', L"小老鼠" }, { L'[', L"左方括號" }, { L'\\', L"反斜線" },
    { L']', L"右方括號" }, { L'^', L"插入號" }, { L'_', L"底線" }, { L'`', L"重音符" }, { L'{', L"左大括號" },
    { L'|', L"直線" }, { L'}', L"右大括號" }, { L'~', L"波浪號" },
};

std::wstring SpokenForChar(wchar_t ch) {
    if (ch >= L'A' && ch <= L'Z') return std::wstring(L"大寫 ") + ch;
    for (const auto& n : kAsciiNames) if (n.ch == ch) return n.name;
    return std::wstring(1, ch);
}

// ------------------------------------------------------------- Nemeth
// Multi-cell symbol sequences, recognised inside the Nemeth context by
// longest match. nemeth.inc is generated from JAWS's Liblouis Nemeth tables;
// kMath is the BKey Sign.tbl list (North American ASCII cells), kept for the
// users who learned it - where the two agree on a sequence, which is most of
// them, the Nemeth entry is the one in force.
struct NemethSym { const char* cells; const wchar_t* text; const wchar_t* spoken; };
const NemethSym kNemeth[] = {
#include "nemeth.inc"
};

struct MathSym { const char* cells; const wchar_t* sym; const wchar_t* name; };
const MathSym kMath[] = {
    { ".a", L"α", L"阿爾法" }, { ".b", L"β", L"貝塔" }, { ".g", L"γ", L"伽瑪" }, { ".d", L"δ", L"德爾塔" },
    { ".e", L"ε", L"艾普西隆" }, { ".z", L"ζ", L"澤塔" }, { ".:", L"η", L"伊塔" }, { ".?", L"θ", L"西塔" },
    { ".i", L"ι", L"約塔" }, { ".k", L"κ", L"卡帕" }, { ".l", L"λ", L"拉姆達" }, { ".m", L"μ", L"謬" },
    { ".n", L"ν", L"紐" }, { ".x", L"ξ", L"克西" }, { ".o", L"ο", L"歐米克戎" }, { ".r", L"ρ", L"羅" },
    { ".s", L"σ", L"西格瑪" }, { ".t", L"τ", L"陶" }, { ".u", L"υ", L"宇普西隆" }, { ".f", L"φ", L"斐" },
    { ".&", L"χ", L"卡伊" }, { ".y", L"ψ", L"普賽" }, { ".w", L"ω", L"歐米伽" }, { ".,s", L"Σ", L"大寫西格瑪" },
    { ".p", L"π", L"派" },
    { "@*", L"×", L"乘" }, { "./", L"÷", L"除" }, { ".+", L"∪", L"聯集" }, { ".%", L"∩", L"交集" },
    { "_&", L"＆", L"和" }, { "/.k", L"≠", L"不等於" }, { "$p", L"⊥", L"垂直" }, { ">]", L"√", L"根號" },
    { "$t", L"△", L"三角形" }, { "+-", L"±", L"正負" }, { "$%", L"∠", L"角" }, { "$l", L"∥", L"平行" },
    { ".$", L"▽", L"倒三角形" }, { "@/", L"∵", L"因為" }, { ",*", L"∴", L"所以" },
    { "$<33o", L"↑", L"上箭頭" }, { "$%33o", L"↓", L"下箭頭" }, { "$[33", L"←", L"左箭頭" }, { "$33o", L"→", L"右箭頭" },
    { "@:.k", L"≡", L"恆等於" }, { ".1:", L"≧", L"大於等於" }, { "\"k:", L"≦", L"小於等於" },
};
// (BKey writes the dot-4 prefix as ` and the 2-4-6 / 1-2-4-5-6 cells as { } ;
// here they are the canonical @ [ ] of the cell table.)

struct Seq { std::vector<unsigned> cells; std::wstring text; std::wstring spoken; };
std::vector<Seq> g_seqs;
bool g_seqsReady = false;

const Seq* ExactSeq(const std::vector<unsigned>& cells, size_t n) {
    for (const auto& s : g_seqs) {
        if (s.cells.size() != n) continue;
        bool same = true;
        for (size_t i = 0; i < n; ++i) if (s.cells[i] != cells[i]) { same = false; break; }
        if (same) return &s;
    }
    return nullptr;
}

void EnsureSeqs() {
    if (g_seqsReady) return;
    g_seqsReady = true;
    for (const auto& n : kNemeth) {
        Seq s; unsigned m = 0;
        for (const char* p = n.cells; ; ++p) {
            if (*p == '-' || *p == 0) { if (m) s.cells.push_back(m); m = 0; if (!*p) break; }
            else if (*p >= '1' && *p <= '8') m |= 1u << (*p - '1');
        }
        if (s.cells.empty()) continue;
        s.text = n.text; s.spoken = n.spoken;
        g_seqs.push_back(s);
    }
    for (const auto& b : kMath) {
        Seq s;
        for (const char* p = b.cells; *p; ++p) { unsigned c = AsciiToCell(*p); if (c) s.cells.push_back(c); }
        if (s.cells.empty() || ExactSeq(s.cells, s.cells.size())) continue;
        s.text = b.sym; s.spoken = b.name;
        g_seqs.push_back(s);
    }
}

// Some entry is longer than `cells` and starts with it.
bool IsPrefixSeq(const std::vector<unsigned>& cells) {
    for (const auto& s : g_seqs) {
        if (s.cells.size() <= cells.size()) continue;
        bool same = true;
        for (size_t i = 0; i < cells.size(); ++i) if (s.cells[i] != cells[i]) { same = false; break; }
        if (same) return true;
    }
    return false;
}

const unsigned kNumSign  = kDot3 | kDot4 | kDot5 | kDot6;   // 3456 numeric indicator / fraction close
const unsigned kFracOpen = kDot1 | kDot4 | kDot5 | kDot6;   // 1456
const unsigned kRadical  = kDot3 | kDot4 | kDot5;           // 345
const unsigned kRadIndex = kDot1 | kDot2 | kDot6;           // 126 index-of-radical indicator
const unsigned kUnder    = kDot1 | kDot4 | kDot6;           // 146 underscript indicator
const unsigned kTermin   = kDot1 | kDot2 | kDot4 | kDot5 | kDot6;   // 12456
const unsigned kComma    = kDot6;                           // 6: first cell of the complex-fraction indicators
const unsigned kMinus    = kDot3 | kDot6;
const unsigned kDecimal  = kDot4 | kDot6;
const unsigned kSwitch   = kDot4 | kDot5 | kDot6;           // 456: first cell of the Nemeth open/close indicators
const unsigned kOpen     = kDot1 | kDot4 | kDot6;           // 456-146 opens
const unsigned kClose    = kDot1 | kDot5 | kDot6;           // 456-156 closes

bool IsDigitCell(unsigned dots) {
    EnsureEnglish();
    wchar_t c = g_english[dots & 0xFF];
    return c >= L'0' && c <= L'9';
}

std::wstring Widen(const char* s) {
    std::wstring w;
    for (; *s; ++s) w += (wchar_t)(unsigned char)*s;
    return w;
}

// Append one piece of output to another: text and speech concatenate, an
// error anywhere is an error (the text typed so far is still typed).
void Join(BrlOutput& acc, const BrlOutput& piece) {
    if (!piece.text.empty()) { acc.text += piece.text; if (acc.action != BrlOutput::Error) acc.action = BrlOutput::Text; }
    if (!piece.spoken.empty()) { if (!acc.spoken.empty()) acc.spoken += L' '; acc.spoken += piece.spoken; }
    if (piece.action == BrlOutput::Error) acc.action = BrlOutput::Error;
}

}  // namespace

char CellToAscii(unsigned dots6) {
    dots6 &= 63;
    return kAscii[dots6];
}

bool BrlLoadJbt(const wchar_t* path) {
    // _wfopen rather than iostreams: a wide path without pulling the whole
    // iostream library into a statically linked program.
    FILE* f = _wfopen(path, L"rb");
    if (!f) return false;
    std::string data;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.empty()) return false;
    wchar_t table[256]; int prio[256];
    memset(table, 0, sizeof(table));
    for (int& p : prio) p = 99;
    int sectionPrio = 99;   // [input] 0, [ANSI] 1, Unicode sections 2, skipped 99
    size_t pos = 0; int count = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) nl = data.size();
        std::string line = data.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t i = 0; while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i) line.erase(0, i);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {   // `[=2467` is the [ character, not a section
            std::string sec; for (char c : line) sec += (char)tolower((unsigned char)c);
            if (sec == "[input]") sectionPrio = 0;
            else if (sec == "[ansi]") sectionPrio = 1;
            else if (sec == "[table attributes]" || sec == "[braille unicode]") sectionPrio = 99;
            else sectionPrio = 2;
            continue;
        }
        if (sectionPrio == 99) continue;
        unsigned cp, mask;
        if (!ParseJbtLine(line, &cp, &mask)) continue;
        if (cp < 32 || cp == 127 || (cp >= 0x2800 && cp <= 0x28FF) || cp > 0xFFFF) continue;
        if (prio[mask] <= sectionPrio) continue;
        table[mask] = (wchar_t)cp; prio[mask] = sectionPrio;
        ++count;
    }
    if (count < 64) return false;   // not a braille table
    memcpy(g_english, table, sizeof(g_english));
    ApplyTaiwanOverrides();
    g_englishReady = true;
    return true;
}

int BrlEnglishTableSize() {
    EnsureEnglish();
    int n = 0;
    for (wchar_t c : g_english) if (c) ++n;
    return n;
}

wchar_t BrlEnglishChar(unsigned dots) {
    EnsureEnglish();
    return g_english[dots & 0xFF];
}

int BrlSyllableTableSize() { return kSyllableCount; }

int BrlChineseSymbolCount() {
    EnsureTw();
    return (int)g_tw.size();
}

int BrlNemethSymbolCount() {
    EnsureSeqs();
    return (int)g_seqs.size();
}

void BrlEngine::Reset() {
    cbuf_.clear();
    awaitingTone_ = false;
    nemeth_ = false; nemethSticky_ = false; held_ = 0;
    seq_.clear();
    fracDepth_ = termDepth_ = 0;
    radIndex_ = false;
    lastWasDigit_ = false;
}

void BrlEngine::PlainKey() {
    bool keep = nemethSticky_;
    Reset();
    if (keep) nemeth_ = nemethSticky_ = true;
}

bool BrlEngine::HasPendingPunct() const {
    if (cbuf_.empty()) return false;
    EnsureTw();
    std::vector<unsigned> ext = cbuf_; ext.push_back(0);
    return TwExact(ext) != nullptr || TwPrefix(ext);
}

BrlOutput BrlEngine::CommitPunct() {
    if (cbuf_.empty()) return BrlOutput();
    return Chinese(0, true);
}

BrlOutput BrlEngine::Flush() {
    BrlOutput out;
    if (!cbuf_.empty()) Join(out, ChineseFlush());
    awaitingTone_ = false;   // Chinese: a space ends the syllable
    if (held_) { unsigned h = held_; held_ = 0; Join(out, Plain(h)); }
    if (!seq_.empty()) Join(out, ResolveSeq(true));
    lastWasDigit_ = false;   // a space ends a number: 4-6 then a digit is no longer a decimal point
    return out;
}

BrlOutput BrlEngine::OnCell(unsigned dots, BrlMode mode) {
    BrlOutput out;
    if (dots == 0) return out;
    if (mode != lastMode_) { Reset(); lastMode_ = mode; }
    // Dot 7 alone and dot 8 alone are editing keys in either regime. While
    // cells are waiting in English, dot 7 takes the last one back instead of
    // deleting a character that is already on screen.
    if (dots == kDot7) {
        if (mode == BrlMode::English && !seq_.empty()) { seq_.pop_back(); out.spoken = L"退格"; return out; }
        if (mode == BrlMode::English && held_) { held_ = 0; out.spoken = L"退格"; return out; }
        if (mode == BrlMode::Chinese && !cbuf_.empty()) { cbuf_.pop_back(); out.spoken = L"退格"; return out; }
        if (mode == BrlMode::English) { seq_.clear(); } else Reset();
        out.action = BrlOutput::Backspace; out.spoken = L"退格"; return out;
    }
    if (dots == kDot8) {
        out = Flush();
        PlainKey();
        out.action = BrlOutput::Enter;
        out.spoken = out.spoken.empty() ? L"Enter" : out.spoken + L" Enter";
        return out;
    }
    if (mode == BrlMode::Chinese) return Chinese(dots, false);
    return English(dots);
}

// A rime standing alone as a syllable: its 大千 keys, and the tone key when
// the tone is already known.
BrlOutput BrlEngine::SyllableKeys(unsigned rime, unsigned tone) {
    BrlOutput out;
    const char* keys = nullptr;
    if (const ToneRime* r = ToneRimeFor(rime)) keys = (tone & r->tones) ? r->keys : r->otherKeys;
    else keys = LookupSyllable(std::string(1, CellToAscii(rime)));
    if (!keys) { out.action = BrlOutput::Error; return out; }
    out.action = BrlOutput::Keys; out.text = Widen(keys); out.spoken = SpokenForKeys(keys);
    if (const Tone* t = ToneFor(tone)) { out.text += t->key; out.spoken += L" " + std::wstring(t->name); awaitingTone_ = false; }
    else awaitingTone_ = tone == 0;
    return out;
}

// The space bar or Enter with Chinese cells held: a symbol that is complete
// is typed, a rime alone becomes its syllable (the space that follows is
// then its first tone, as the IME reads it), an initial alone is dropped and
// anything else is refused.
BrlOutput BrlEngine::ChineseFlush() {
    EnsureTw();
    BrlOutput out;
    std::vector<unsigned> held; held.swap(cbuf_);
    if (const TwSeq* s = TwExact(held)) { out.action = BrlOutput::Text; out.text = s->text; out.spoken = s->spoken; return out; }
    if (held.size() == 1) {
        unsigned h = held[0];
        char c = CellToAscii(h);
        if (IsInitial(c)) return out;
        if (ToneRimeFor(h) || LookupSyllable(std::string(1, c))) { out = SyllableKeys(h, 0); awaitingTone_ = false; return out; }
        std::vector<unsigned> withBlank = { h, 0 };
        if (TwExact(withBlank)) return out;      // a mark left unconfirmed at Enter: dropped, as the IME drops its composition
    }
    out.action = BrlOutput::Error;
    return out;
}

BrlOutput BrlEngine::Chinese(unsigned dots, bool blank) {
    EnsureTw();
    BrlOutput out;
    if (blank) dots = 0;
    bool eightDot = (dots & (kDot7 | kDot8)) != 0;
    char c = eightDot ? 0 : CellToAscii(dots);

    // A finished syllable is waiting for its tone.
    if (awaitingTone_) {
        awaitingTone_ = false;
        if (const Tone* t = ToneFor(dots)) { out.action = BrlOutput::Keys; out.text = t->key; out.spoken = t->name; return out; }
        // No tone written: the user went straight on to the next syllable.
        // Fall through and treat this cell as a new start.
    }

    // Does this cell carry the held cells on towards a symbol?
    std::vector<unsigned> ext = cbuf_; ext.push_back(dots);
    const TwSeq* exact = TwExact(ext);
    if (exact && !TwPrefix(ext)) {
        cbuf_.clear();
        out.action = BrlOutput::Text; out.text = exact->text; out.spoken = exact->spoken;
        return out;
    }
    if (exact || TwPrefix(ext)) { cbuf_ = ext; return out; }   // may still grow: wait (co-existing exact matches wait too)

    if (cbuf_.empty()) {
        if (blank) return out;
        if (eightDot) { out.action = BrlOutput::Error; return out; }   // no 8-dot cells in 國語點字
        if (IsInitial(c)) { cbuf_.push_back(dots); return out; }       // wait for the rime
        if (ToneRimeFor(dots)) { cbuf_.push_back(dots); return out; }  // read by its tone: wait for it
        if (LookupSyllable(std::string(1, c))) return SyllableKeys(dots, 0);
        out.action = BrlOutput::Error;                                 // a lone tone, or a cell that is nothing
        return out;
    }

    // Held cells that this cell does not extend. One cell held: it was a
    // syllable cell or a mark that a blank completes.
    if (cbuf_.size() == 1) {
        unsigned h = cbuf_[0];
        char hc = CellToAscii(h);
        std::vector<unsigned> withBlank = { h, 0 };
        const TwSeq* mark = TwExact(withBlank);
        const Tone* tone = ToneFor(dots);
        if (ToneRimeFor(h) || LookupSyllable(std::string(1, hc))) {
            // A rime: with a tone it is the syllable; with anything else
            // (a blank, another cell) the syllable is typed and that cell
            // is looked at again.
            cbuf_.clear();
            if (tone) return SyllableKeys(h, dots);
            out = SyllableKeys(h, 0);
            if (blank) awaitingTone_ = false;    // the space that follows is the first tone
            else out.again = true;
            return out;
        }
        if (IsInitial(hc) && !blank) {
            if (!eightDot) {
                const char* keys = LookupSyllable(std::string(1, hc) + c);
                if (keys) {
                    cbuf_.clear();
                    out.action = BrlOutput::Keys; out.text = Widen(keys); out.spoken = SpokenForKeys(keys); awaitingTone_ = true;
                    return out;
                }
            }
            // A tone right after an initial: the writer left out the empty
            // rime (帀, dots 1-5-6) that ㄓㄔㄕㄖㄗㄘㄙ take when they stand
            // alone. Accept it - look the initial up with 帀 and apply the
            // tone at once.
            if (tone) {
                const char* alone = LookupSyllable(std::string(1, hc) + ":");
                if (alone) {
                    cbuf_.clear();
                    out.action = BrlOutput::Keys; out.text = Widen(alone) + tone->key;
                    out.spoken = SpokenForKeys(alone) + L" " + tone->name;
                    return out;
                }
            }
        }
        if (mark && !blank) {
            // The mark that a blank would complete (2-3 逗號, or ㄋ 1-3-4-5
            // which is 問號 before a blank): dot 3 confirms it as Space
            // does, any other cell confirms it and is then handled itself.
            cbuf_.clear();
            out.action = BrlOutput::Text; out.text = mark->text; out.spoken = mark->spoken;
            if (dots != kDot3) out.again = true;
            return out;
        }
        cbuf_.clear();
        out.action = BrlOutput::Error;
        return out;
    }

    // Several cells held. If they are a symbol themselves (∠ 1-2-4-6 2-4-6,
    // which also begins ←) it is typed and this cell is looked at again;
    // otherwise the cell is refused and the sequence keeps waiting, as a
    // wrong cell in the middle of a symbol should not turn it into syllables.
    if (const TwSeq* s = TwExact(cbuf_)) {
        cbuf_.clear();
        out.action = BrlOutput::Text; out.text = s->text; out.spoken = s->spoken;
        if (!blank && dots != kDot3) out.again = true;
        return out;
    }
    out.action = BrlOutput::Error;
    return out;
}

// One cell of the English table, typed as-is.
BrlOutput BrlEngine::Plain(unsigned dots) {
    BrlOutput out;
    wchar_t ch = BrlEnglishChar(dots);
    if (!ch) { out.action = BrlOutput::Error; lastWasDigit_ = false; return out; }
    out.action = BrlOutput::Text;
    out.text = ch;
    out.spoken = SpokenForChar(ch);
    lastWasDigit_ = ch >= L'0' && ch <= L'9';
    return out;
}

BrlOutput BrlEngine::English(unsigned dots) {
    EnsureSeqs();
    if (nemeth_) return Nemeth(dots);
    BrlOutput out;
    if (held_) {
        unsigned h = held_; held_ = 0;
        // 3456 was a numeric indicator if a number follows: a digit, a minus
        // or a decimal point. 456 was the opening indicator if 146 follows.
        // Otherwise the held cell was the plain character (# or _), and this
        // cell is handed back in on its own.
        if (h == kNumSign && (IsDigitCell(dots) || dots == kMinus || dots == kDecimal)) {
            nemeth_ = true; lastWasDigit_ = false;
            return Nemeth(dots);
        }
        if (h == kSwitch && dots == kOpen) {
            nemeth_ = nemethSticky_ = true; lastWasDigit_ = false;
            out.spoken = L"Nemeth 開始";
            return out;
        }
        out = Plain(h);
        out.again = true;
        return out;
    }
    if (dots == kNumSign || dots == kSwitch) { held_ = dots; return out; }
    return Plain(dots);
}

BrlOutput BrlEngine::Nemeth(unsigned dots) {
    // A decimal point: 4-6 between digits. Without this rule 4-6 followed by
    // dots 2 would be the greater-than sign (46-2 in Nemeth, the same cells).
    if (seq_.size() == 1 && seq_[0] == kDecimal && lastWasDigit_ && IsDigitCell(dots)) {
        seq_.clear();
        BrlOutput out; out.action = BrlOutput::Text; out.text = L"."; out.spoken = L"點";
        Join(out, Nemeth(dots));
        return out;
    }
    seq_.push_back(dots);
    return ResolveSeq(false);
}

// Longest match over the cells waiting in seq_. With `final` false the
// sequence may still grow, so it is left alone while it is the start of some
// longer symbol; otherwise the longest symbol at its head is emitted, or the
// head cell as a plain character, and the rest is looked at again.
BrlOutput BrlEngine::ResolveSeq(bool final) {
    BrlOutput acc;
    while (!seq_.empty()) {
        if (!final && IsPrefixSeq(seq_)) break;
        size_t best = 0; const Seq* s = nullptr;
        for (size_t k = seq_.size(); k >= 1; --k) { s = ExactSeq(seq_, k); if (s) { best = k; break; } }
        BrlOutput piece;
        if (best) {
            unsigned first = seq_[0];
            piece.action = BrlOutput::Text; piece.text = s->text; piece.spoken = s->spoken;
            lastWasDigit_ = s->text.size() == 1 && s->text[0] >= L'0' && s->text[0] <= L'9';
            if (best == 1 && first == kNumSign) {
                // Fraction close when a fraction is open, else the numeric
                // indicator, which types nothing.
                if (fracDepth_ > 0) --fracDepth_; else { piece.action = BrlOutput::None; piece.text.clear(); piece.spoken.clear(); }
            } else if (best == 1 && first == kFracOpen) {
                ++fracDepth_;
            } else if (best == 2 && (first == kComma || first == kSwitch) && seq_[1] == kFracOpen) {
                ++fracDepth_;                                               // complex 6-1456, mixed 456-1456
            } else if (best == 2 && (first == kComma || first == kSwitch) && seq_[1] == kNumSign) {
                if (fracDepth_ > 0) --fracDepth_;                           // complex 6-3456, mixed 456-3456
            } else if (best == 1 && first == kRadIndex) {
                radIndex_ = true;                                           // √[ ... the index follows
            } else if (best == 1 && first == kRadical) {
                if (radIndex_) { radIndex_ = false; piece.text = L"]("; }   // index typed: √[3](8)
                ++termDepth_;
            } else if (best == 1 && first == kUnder) {
                ++termDepth_;
            } else if (best == 1 && first == kTermin) {
                if (termDepth_ > 0) --termDepth_;
                else if (radIndex_) { radIndex_ = false; piece.text = L"]"; }
                else piece = Plain(first);   // nothing open: the cell is a plain }
            } else if (best == 2 && first == kSwitch && seq_[1] == kClose) {
                // The closing indicator: leave the context, whatever was open.
                piece.action = BrlOutput::None;
                Join(acc, piece);
                Reset();
                return acc;
            } else if (piece.text.empty()) {
                piece.action = BrlOutput::None;                             // baseline, type-form, opening indicator: spoken only
            }
            seq_.erase(seq_.begin(), seq_.begin() + best);
        } else {
            piece = Plain(seq_[0]);
            seq_.erase(seq_.begin());
        }
        Join(acc, piece);
    }
    return acc;
}
