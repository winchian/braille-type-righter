// Offline test of the braille engine: dot sequences in, keys out. Run before
// the program goes anywhere near a keyboard.
//   g++ -std=c++17 -O2 -static -municode -I ..\src brl_test.cpp ..\src\brl_engine.cpp
// Optional argument: a JAWS US_Unicode.jbt to load over the embedded table
// (the two must then agree - that is one of the tests).
#include <cstdio>
#include <cwchar>
#include <io.h>
#include <fcntl.h>
#include <string>
#include <vector>

#include "brl_engine.h"

static unsigned Dots(const char* s) {
    unsigned m = 0;
    for (; *s; ++s) if (*s >= '1' && *s <= '8') m |= 1u << (*s - '1');
    return m;
}

static int g_fail = 0;

// cells: space-separated dot strings, e.g. "34 126 4" ; SP = the space bar,
// CR = the Enter key (both flush what is waiting); expect: concatenated
// output, with <BS> <CR> <ERR> for the actions.
static void Case(const wchar_t* what, BrlMode mode, const char* cells, const std::wstring& expect, const wchar_t* expectSpoken = nullptr) {
    BrlEngine e;
    std::wstring got, spoken;
    std::string s(cells);
    size_t i = 0;
    auto take = [&](const BrlOutput& o) {
        if (!o.text.empty() && o.action != BrlOutput::Keys) got += o.text;
        switch (o.action) {
        case BrlOutput::None: break;
        case BrlOutput::Keys: got += o.text; break;
        case BrlOutput::Text: break;
        case BrlOutput::Backspace: got += L"<BS>"; break;
        case BrlOutput::Enter: got += L"<CR>"; break;
        case BrlOutput::Error: got += L"<ERR>"; break;
        }
        if (!o.spoken.empty()) spoken += o.spoken + L"|";
    };
    while (i < s.size()) {
        size_t j = s.find(' ', i); if (j == std::string::npos) j = s.size();
        std::string tok = s.substr(i, j - i); i = j + 1;
        if (tok.empty()) continue;
        if (tok == "SP") {
            // The shell: a waiting Chinese mark is confirmed by Space; otherwise
            // the waiting cells are flushed and the space itself goes through.
            if (e.HasPendingPunct()) take(e.CommitPunct());
            else { take(e.Flush()); got += L" "; }
            continue;
        }
        if (tok == "CR") { take(e.Flush()); e.PlainKey(); got += L"<CR>"; continue; }
        if (tok == "ESC") { e.PlainKey(); continue; }   // any other editing key
        BrlOutput o = e.OnCell(Dots(tok.c_str()), mode);
        take(o);
        if (o.again) take(e.OnCell(Dots(tok.c_str()), mode));
    }
    // Cells still waiting at the end of the input (a symbol that is also the
    // start of a longer one - ° before ∘, ↓ before ⇊) are resolved as the
    // shell would on the next Space or Enter; the line says so.
    const wchar_t* tail = L"";
    if (e.HasPendingCells()) { take(e.Flush()); tail = L"   (結尾等下一方，強制送出)"; }
    bool ok = got == expect && (!expectSpoken || spoken == expectSpoken);
    if (!ok) ++g_fail;
    std::wstring wc; for (const char* q = cells; *q; ++q) wc += (wchar_t)*q;
    wprintf(L"%ls  %ls\n    cells: %ls\n    got:   [%ls]   spoken: %ls%ls\n", ok ? L"ok  " : L"FAIL", what, wc.c_str(), got.c_str(), spoken.c_str(), tail);
    if (!ok) wprintf(L"    want:  [%ls]%ls%ls\n", expect.c_str(), expectSpoken ? L"   spoken: " : L"", expectSpoken ? expectSpoken : L"");
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), 0x40000);   // _O_U8TEXT
    wprintf(L"cell table check: dots 135 -> %c, 2346 -> %c, 156 -> %c, 34 -> %c\n",
            CellToAscii(Dots("135")), CellToAscii(Dots("2346")), CellToAscii(Dots("156")), CellToAscii(Dots("34")));

    // Embedded English table versus the live JAWS file: every cell must agree.
    wchar_t embedded[256];
    for (unsigned d = 0; d < 256; ++d) embedded[d] = BrlEnglishChar(d);
    wprintf(L"embedded English table: %d cells\n", BrlEnglishTableSize());
    if (argc > 1) {
        bool loaded = BrlLoadJbt(argv[1]);
        wprintf(L"%ls  load %ls\n", loaded ? L"ok  " : L"FAIL", argv[1]);
        if (!loaded) ++g_fail;
        int diff = 0;
        for (unsigned d = 0; d < 256; ++d) if (embedded[d] != BrlEnglishChar(d)) { ++diff; wprintf(L"    cell 0x%02X: embedded U+%04X file U+%04X\n", d, embedded[d], BrlEnglishChar(d)); }
        wprintf(L"%ls  embedded table agrees with the file (%d cells differ)\n", diff ? L"FAIL" : L"ok  ", diff);
        if (diff) ++g_fail;
    }
    // Spot checks of the 8-dot table itself.
    struct { const char* dots; wchar_t ch; } spots[] = {
        { "1", L'a' }, { "17", L'A' }, { "2", L'1' }, { "356", L'0' }, { "6", L',' }, { "3456", L'#' },
        { "456", L'_' }, { "4567", L'_' }, { "46", L'.' }, { "346", L'+' }, { "123456", L'=' }, { "1246", L'$' },
        { "2467", L'[' }, { "124567", L']' }, { "12567", L'|' }, { "47", L'@' }, { "457", L'^' }, { "18", L'¹' },
        { "246", L'{' }, { "12456", L'}' }, { "1256", L'\\' }, { "45", L'~' }, { "3567", L'Ð' },   // 1256 = \ (Paul 2026-09-09, 台灣照 NABCC)
    };
    for (const auto& sp : spots) {
        wchar_t got = BrlEnglishChar(Dots(sp.dots));
        bool ok = got == sp.ch; if (!ok) ++g_fail;
        wprintf(L"%ls  table %hs -> U+%04X (%lc)\n", ok ? L"ok  " : L"FAIL", sp.dots, got, got ? got : L' ');
    }
    // Nothing for a chord that means nothing (no braille-pattern glyphs).
    { bool ok = BrlEnglishChar(Dots("12345678")) != 0x28FF; if (!ok) ++g_fail; wprintf(L"%ls  no braille-pattern fallback (U+%04X)\n", ok ? L"ok  " : L"FAIL", BrlEnglishChar(Dots("12345678"))); }

    // 我 = ㄨㄛˇ : rime ㄨㄛ is one cell (dots 2-5), tone ˇ dot 4 -> keys ji then 3
    Case(L"我 ㄨㄛˇ", BrlMode::Chinese, "25 4", L"ji3");
    // 找 = ㄓㄠˇ : initial ㄓ dot 1, rime ㄠ 1-4-6, tone ˇ -> 5l3
    Case(L"找 ㄓㄠˇ", BrlMode::Chinese, "1 146 4", L"5l3");
    // 們 = ㄇㄣ˙ : ㄇ 134, ㄣ 136, neutral dot 1 -> ap7
    Case(L"們 ㄇㄣ˙", BrlMode::Chinese, "134 136 1", L"ap7");
    // 是 = ㄕ+帀 ˋ : ㄕ 24, 帀 156, tone ˋ dot 5 -> g4
    Case(L"是 ㄕˋ", BrlMode::Chinese, "24 156 5", L"g4");
    // 他 = ㄊㄚ 1st tone : ㄊ 124, ㄚ 345, dot 3 -> w8 then Space
    Case(L"他 ㄊㄚ", BrlMode::Chinese, "124 345 3", L"w8 ");
    Case(L"現 ㄒㄧㄢˋ", BrlMode::Chinese, "15 246 5", L"vul4");
    // 中文 : ㄓㄨㄥ = a + & (12346) tone1 ; ㄨㄣˊ = '=' (123456) tone2
    Case(L"中文", BrlMode::Chinese, "1 12346 3 123456 2", L"5j/ jp6");
    Case(L"lone tone then space", BrlMode::Chinese, "5 SP", L"<ERR> ");
    Case(L"lone dot 3", BrlMode::Chinese, "3", L"<ERR>");
    Case(L"ㄓˋ without 帀", BrlMode::Chinese, "1 5", L"54");
    Case(L"backspace, enter", BrlMode::Chinese, "7 8", L"<BS><CR>");
    Case(L"綠 ㄌㄩˋ", BrlMode::Chinese, "14 1256 5", L"xm4");
    Case(L"家 ㄐㄧㄚ", BrlMode::Chinese, "13 23456 3", L"ru8 ");
    Case(L"8-dot cell in Chinese", BrlMode::Chinese, "17", L"<ERR>");

    // Chinese punctuation: the mark waits for Space (dot 3 or the space bar);
    // another cell confirms it and is then handled itself.
    Case(L"逗號 then dot3", BrlMode::Chinese, "23 3", L"，");
    Case(L"句號 then space bar", BrlMode::Chinese, "36 SP", L"。");
    Case(L"逗號 then 我", BrlMode::Chinese, "23 25 4", L"，ji3");
    Case(L"逗號 spoken on confirmation", BrlMode::Chinese, "23 3", L"，", L"逗號|");
    Case(L"頓號", BrlMode::Chinese, "6 SP", L"、", L"頓號|");
    Case(L"句號 then dot3", BrlMode::Chinese, "36 3", L"。");
    Case(L"分號", BrlMode::Chinese, "56 3", L"；");
    Case(L"冒號 is 25 25 (ㄨㄛ ㄨㄛ is not)", BrlMode::Chinese, "25 25", L"：", L"冒號|");
    Case(L"問號 1456 then space bar (ㄧㄣ before a blank)", BrlMode::Chinese, "1456 SP", L"？", L"問號|");
    Case(L"問號 with ㄋ 1345", BrlMode::Chinese, "1345 3", L"？");
    Case(L"驚嘆號 with ㄎ 123", BrlMode::Chinese, "123 3", L"！");
    // 2026-09-13: a tone says nothing of its own any more. It finishes the
    // syllable, the IME composes the character, and the screen reader
    // announces that - which is what the user actually wants to hear.
    Case(L"因 ㄧㄣ first tone is dot 3, not the space bar", BrlMode::Chinese, "1456 3", L"up ", L"");
    Case(L"我 with a held rime speaks once", BrlMode::Chinese, "25 4", L"ji3", L"");
    Case(L"held rime then a new syllable", BrlMode::Chinese, "25 124 345 3", L"jiw8 ");
    Case(L"held rime then the space bar", BrlMode::Chinese, "25 SP", L"ji ");
    Case(L"刪節號", BrlMode::Chinese, "5 5 5", L"…", L"刪節號|");
    Case(L"破折號 short", BrlMode::Chinese, "5 2", L"—");
    Case(L"破折號 with dot 7 and 8 cells", BrlMode::Chinese, "2578 2578", L"—");
    Case(L"左括號 ㄧㄠ then space", BrlMode::Chinese, "246 SP", L"（");
    Case(L"右括號 ㄅ then space", BrlMode::Chinese, "135 SP", L"）");
    Case(L"「」", BrlMode::Chinese, "56 36 36 23", L"「」", L"左引號|右引號|");
    Case(L"『』", BrlMode::Chinese, "236 236 356 356", L"『』");
    Case(L"《》", BrlMode::Chinese, "126 126 345 345", L"《》", L"左書名號|右書名號|");
    Case(L"〈〉", BrlMode::Chinese, "126 SP 345 SP", L"〈〉");
    Case(L"【】", BrlMode::Chinese, "12346 SP 13456 SP", L"【】");
    Case(L"ㄛ first tone still dot 3", BrlMode::Chinese, "126 3", L"i ");
    Case(L"波 ㄅㄛ", BrlMode::Chinese, "135 126 3", L"1i ");
    Case(L"×", BrlMode::Chinese, "4 16", L"×", L"乘|");
    Case(L"÷", BrlMode::Chinese, "46 34", L"÷");
    Case(L"≠", BrlMode::Chinese, "34 46 13", L"≠", L"不等於|");
    Case(L"± ∞", BrlMode::Chinese, "346 36 6 123456", L"±∞");
    Case(L"右箭頭", BrlMode::Chinese, "1246 25 25 135", L"→", L"右箭頭|");
    Case(L"左箭頭", BrlMode::Chinese, "1246 246 25 25", L"←");
    Case(L"∠ then a syllable (co-existing longer symbol)", BrlMode::Chinese, "1246 246 124 345 3", L"∠w8 ", L"角|特 啊|");
    Case(L"∠ then the space bar", BrlMode::Chinese, "1246 246 SP", L"∠ ");
    Case(L"wrong cell inside a symbol is refused, sequence kept", BrlMode::Chinese, "1246 126 1 25 25 135", L"<ERR>↑");
    Case(L"dot 7 takes a held cell back", BrlMode::Chinese, "1246 126 7 25 25 135", L"→", L"退格|右箭頭|");
    Case(L"Α capital alpha", BrlMode::Chinese, "46 17", L"Α", L"大寫 阿爾法|");
    Case(L"α wins over ㄧㄤ neutral tone", BrlMode::Chinese, "46 1", L"α", L"阿爾法|");
    Case(L"央 ㄧㄤ first tone", BrlMode::Chinese, "46 3", L"u; ");
    Case(L"娘 ㄋㄧㄤ neutral tone", BrlMode::Chinese, "1345 46 1", L"su;7");
    Case(L"唷 ㄧㄛ 356 first tone", BrlMode::Chinese, "356 3", L"ui ", L"");
    Case(L"欸 ㄟ 356 fourth tone", BrlMode::Chinese, "356 5", L"o4");
    Case(L"崖 ㄧㄞ 26 second tone", BrlMode::Chinese, "26 2", L"u96");
    Case(L"誒 ㄝ 26 fourth tone", BrlMode::Chinese, "26 5", L",4");
    Case(L"飛 ㄈㄟ is unchanged", BrlMode::Chinese, "12345 356 3", L"zo ");
    Case(L"度", BrlMode::Chinese, "45 46 16", L"°", L"度|");
    Case(L"8-dot cell that starts nothing", BrlMode::Chinese, "18", L"<ERR>");
    Case(L"Enter with a held rime", BrlMode::Chinese, "25 8", L"ji<CR>");
    Case(L"Enter with a held mark drops it", BrlMode::Chinese, "23 8", L"<CR>");
    // Syllables the dictionary has that Phn.tbl lacked (gen_syllables.py EXTRA).
    Case(L"咯 ㄌㄛ neutral", BrlMode::Chinese, "14 126 1", L"xi7");
    Case(L"扽 ㄉㄣ fourth", BrlMode::Chinese, "145 136 5", L"2p4");

    // English: 8-dot US computer braille, straight from the table.
    Case(L"hello", BrlMode::English, "125 15 123 123 135", L"hello", L"h|e|l|l|o|");
    Case(L"capital with dot 7", BrlMode::English, "17 24", L"Ai", L"大寫 A|i|");
    Case(L"digits are lowered cells", BrlMode::English, "23 356 23 235", L"2026");
    Case(L"dot 6 is a comma, 3456 then letter is #", BrlMode::English, "6 3456 1", L",#a", L"逗號|井字號|a|");
    Case(L"period before space is not lost", BrlMode::English, "125 24 46 SP 1", L"hi. a");
    Case(L"underscore then space", BrlMode::English, "1346 456 SP", L"x_ ");
    Case(L"underscore then digit", BrlMode::English, "1346 456 2", L"x_1");
    Case(L"e.g. is plain outside Nemeth", BrlMode::English, "15 46 1245 46", L"e.g.");
    Case(L"punctuation", BrlMode::English, "6 46 1456 2346 12356 23456", L",.?!()");
    Case(L"underscore both ways", BrlMode::English, "456 4567", L"__");
    Case(L"unknown cell", BrlMode::English, "367", L"<ERR>");
    Case(L"# before space", BrlMode::English, "3456 SP", L"# ");
    Case(L"# before enter", BrlMode::English, "3456 8", L"#<CR>");
    Case(L"dot 7 takes # back", BrlMode::English, "3456 7 1", L"a", L"退格|a|");
    Case(L"dot-8 cells of the Unicode sections", BrlMode::English, "18 128", L"¹²");

    // Nemeth: numeric indicator opens the context.
    Case(L"number", BrlMode::English, "3456 2 23 25", L"123", L"1|2|3|");
    Case(L"decimal", BrlMode::English, "3456 25 46 2 256", L"3.14");
    Case(L"negative", BrlMode::English, "3456 36 26", L"-5", L"減 5|");
    Case(L"leading decimal", BrlMode::English, "3456 46 26", L".5");
    Case(L"2+3=5", BrlMode::English, "3456 23 346 25 46 13 3456 26", L"2+3=5", L"2|加 3|等於|5|");
    Case(L"2x3", BrlMode::English, "3456 23 4 16 25", L"2×3", L"2|乘|3|");
    Case(L"6/2 divide sign", BrlMode::English, "3456 235 46 34 23", L"6÷2");
    Case(L"not equal, greater, less", BrlMode::English, "3456 2 34 46 13 23 SP 46 2 SP 3456 25 5 13 256", L"1≠2 > 3<4");
    Case(L"greater-or-equal", BrlMode::English, "3456 2 SP 46 2 156 SP 3456 356", L"1 ≥ 0", L"1|大於等於|0|");
    Case(L"plus-minus", BrlMode::English, "3456 2 346 36 23", L"1±2");
    Case(L"minus then space flushes", BrlMode::English, "3456 2 36 SP 23", L"1- 2");
    Case(L"context survives a space", BrlMode::English, "3456 2 SP 46 13 SP 3456 23", L"1 = 2", L"1|等於|2|");
    Case(L"context ends at Enter", BrlMode::English, "3456 2 8 46 1", L"1<CR>.a");
    Case(L"greek", BrlMode::English, "3456 23 46 1234 46 1 46 6 234", L"2παΣ", L"2|派|阿爾法|大寫 西格瑪|");
    Case(L"letters inside", BrlMode::English, "3456 23 1346 346 13456", L"2x+y");
    Case(L"capital inside", BrlMode::English, "3456 23 17", L"2A");
    Case(L"fraction", BrlMode::English, "3456 2 1456 1 34 12 3456 25", L"1(a/b)3", L"1|分數開始|a|斜線 b|分數結束|3|");
    Case(L"radical", BrlMode::English, "456 146 345 23 12456 456 156", L"√(2)", L"Nemeth 開始|根號 2|結束|Nemeth 結束|");
    Case(L"radical after a number", BrlMode::English, "3456 23 345 25 12456", L"2√(3)");
    Case(L"termination without radical is }", BrlMode::English, "3456 2 12456", L"1}");
    Case(L"superscript baseline", BrlMode::English, "3456 23 45 23 5 346 2", L"2^2+1", L"2|上標 2|基線|加 1|");
    // Explicit Nemeth open / close (456-146 / 456-156): stays across Enter.
    Case(L"open, letter expression, close", BrlMode::English, "456 146 1346 45 23 5 346 2 456 156 46 1245", L"x^2+1.g",
         L"Nemeth 開始|x|上標 2|基線|加 1|Nemeth 結束|句點|g|");
    Case(L"open survives Enter", BrlMode::English, "456 146 46 1 8 46 12 456 156 46 1245", L"α<CR>β.g");
    Case(L"open survives arrows", BrlMode::English, "456 146 46 1 ESC 46 12", L"αβ");
    Case(L"number context ends at arrows", BrlMode::English, "3456 2 ESC 46 12", L"1.b");
    Case(L"456 then something else is _", BrlMode::English, "456 1", L"_a");
    Case(L"close inside a fraction is harmless", BrlMode::English, "456 146 1456 2 456 156", L"(1", L"Nemeth 開始|分數開始|1|Nemeth 結束|");
    Case(L"arrow, infinity, sum", BrlMode::English, "3456 2 1246 135 6 123456 46 6 234", L"1→∞Σ", L"1|右箭頭|無限大|大寫 西格瑪|");
    Case(L"down arrow long sequence", BrlMode::English, "3456 2 1246 146 25 25 135", L"1↓");
    Case(L"broken sequence: longest symbol then the rest", BrlMode::English, "3456 2 1246 146 1", L"1∠a");
    Case(L"broken sequence: no symbol at all", BrlMode::English, "3456 2 1246 1256 1", L"1$|a");   // inside Nemeth 1256 is the bar; only the English table has 1256 = backslash
    Case(L"factorial, percent, degree", BrlMode::English, "3456 26 12346 2 4 356 25 45 46 16", L"5!1%3°", L"5|階乘|1|百分號 3|度|");
    Case(L"times before space", BrlMode::English, "3456 23 4 SP", L"2` ");
    Case(L"nemeth # sign", BrlMode::English, "3456 2 35 2345", L"1#");
    Case(L"BKey sequence still works inside", BrlMode::English, "3456 2 1246 1234", L"1⊥");
    Case(L"dot 7 takes a pending cell back", BrlMode::English, "3456 2 46 7 23", L"12");
    Case(L"backspace with nothing pending", BrlMode::English, "3456 2 7", L"1<BS>");
    Case(L"digit after 9 waits for #", BrlMode::English, "3456 35 35 SP", L"99 ");

    // (Spoken words separated by a space came out of one call: a cell that
    // begins a longer sequence waits, and is spoken together with the cell
    // that resolved it - 9 waits for # (35-2345), + for ± (346-36), % for ‰,
    // the radical 345 for the BKey √ (345-12456).)
    // ---- The full Nemeth table (2026-09-09): every character of the JAWS
    // Liblouis Nemeth tables, by category, plus the indicators.
    wprintf(L"Nemeth table: %d sequences\n", BrlNemethSymbolCount());
    // Relations
    Case(L"≤ inside an opened expression", BrlMode::English, "456 146 1 5 13 156 12 456 156", L"a≤b", L"Nemeth 開始|a|小於等於|b|Nemeth 結束|");
    Case(L"≈ (4-156 twice)", BrlMode::English, "3456 2 4 156 4 156 23", L"1≈2", L"1|近似等於|2|");
    Case(L"≅", BrlMode::English, "3456 2 4 156 46 13 23", L"1≅2", L"1|全等|2|");
    Case(L"∼ alone before a space", BrlMode::English, "3456 2 4 156 SP", L"1~ ", L"1|相似|");
    Case(L"≡ ∝", BrlMode::English, "3456 2 456 123 23 456 123456 25", L"1≡2∝3", L"1|恆等於|2|正比|3|");
    Case(L"≪ (much less than)", BrlMode::English, "3456 2 5 13 4 5 13 12456 23", L"1≪2", L"1|遠小於|2|");
    Case(L"≫ needs a space after a number (4-6 then digit is a decimal point)", BrlMode::English, "3456 2 SP 46 2 4 46 2 12456 SP 3456 23", L"1 ≫ 2", L"1|遠大於|2|");
    Case(L"≦ ≧ (Taiwanese textbook forms)", BrlMode::English, "3456 2 5 13 46 13 23 SP 46 2 46 13 SP 3456 25", L"1≦2 ≧ 3", L"1|小於等於 雙橫線|2|大於等於 雙橫線|3|");
    Case(L"≮ negated relation", BrlMode::English, "3456 2 34 5 13 23", L"1≮2", L"1|不小於 2|");
    Case(L"⋜ starts with the macron cell", BrlMode::English, "3456 2 156 5 13 23", L"1⋜2", L"1|等於或小於|2|");
    Case(L"≍ ≭", BrlMode::English, "456 146 1 4 126 6 126 12 34 4 126 6 126 14", L"a≍b≭c", L"Nemeth 開始|a|等價|b|不等價|c|");
    Case(L"3.1 is still a decimal, not 3 > 1", BrlMode::English, "3456 25 46 2", L"3.1", L"3|點 1|");
    // Sets
    Case(L"∈ then a capital by dot 6", BrlMode::English, "456 146 1346 4 15 6 234", L"x∈S", L"Nemeth 開始|x|屬於|大寫 S|");
    Case(L"∉ ∋", BrlMode::English, "456 146 1346 34 4 15 6 234 4 26 13456", L"x∉S∋y", L"Nemeth 開始|x|不屬於|大寫 S|包含元素|y|");
    Case(L"⊂ ⊇", BrlMode::English, "456 146 6 1 456 5 13 6 12 456 46 2 156 6 14", L"A⊂B⊇C", L"Nemeth 開始|大寫 A|子集合|大寫 B|超集合或等於|大寫 C|");
    Case(L"⊊ (proper subset)", BrlMode::English, "456 146 6 1 456 5 13 34 46 13 6 12", L"A⊊B", L"Nemeth 開始|大寫 A|真子集合|大寫 B|");
    Case(L"∪ = ∅, ∩", BrlMode::English, "456 146 6 1 46 346 6 12 46 13 456 356 SP 6 1 46 146 6 12", L"A∪B=∅ A∩B", L"Nemeth 開始|大寫 A|聯集|大寫 B|等於|空集合|大寫 A|交集|大寫 B|");
    Case(L"ℝ ℕ ⅇ", BrlMode::English, "456 146 4 56 6 1235 4 56 6 1345 4 56 15", L"ℝℕⅇ", L"Nemeth 開始|實數集|自然數集|雙線體 e|");
    // Logic
    Case(L"∧ ∨ ¬", BrlMode::English, "456 146 1234 4 146 12345 4 346 4 1456 1235", L"p∧q∨¬r", L"Nemeth 開始|p|且|q|或|非|r|");
    Case(L"⇒ is 若則", BrlMode::English, "456 146 1234 1246 2356 2356 135 12345", L"p⇒q", L"Nemeth 開始|p|若則|q|");
    Case(L"⇔ is 若且唯若", BrlMode::English, "456 146 1234 1246 246 2356 2356 135 12345", L"p⇔q", L"Nemeth 開始|p|若且唯若|q|");
    Case(L"∀ ∃ ∄", BrlMode::English, "456 146 4 12346 1346 4 123456 13456 34 4 123456 1356", L"∀x∃y∄z", L"Nemeth 開始|對所有|x|存在|y|不存在|z|");
    Case(L"∵ ∴ ∎", BrlMode::English, "456 146 4 34 SP 6 16 SP 456 1256", L"∵ ∴ ∎", L"Nemeth 開始|因為|所以|證畢|");
    // Calculus
    Case(L"∭ ∮", BrlMode::English, "456 146 2346 2346 2346 124 SP 2346 4 1246 14 12456 1245", L"∭f ∮g", L"Nemeth 開始|三重積分|f|環積分|g|");
    Case(L"∂f/∂x", BrlMode::English, "456 146 4 145 124 34 4 145 1346", L"∂f/∂x", L"Nemeth 開始|偏微分|f|斜線 偏微分|x|");
    Case(L"∇ (46-1246, not ▽)", BrlMode::English, "456 146 46 1246 124", L"∇f", L"Nemeth 開始|梯度|f|");
    Case(L"lim with an underscript", BrlMode::English, "456 146 123 24 134 146 1346 1246 135 356 12456 124", L"lim_(x→0)f",
         L"Nemeth 開始|l|i|m|下方|x|右箭頭|0|結束|f|");
    Case(L"d/dx as a fraction", BrlMode::English, "3456 2 1456 145 34 145 1346 3456", L"1(d/dx)", L"1|分數開始|d|斜線 d|x|分數結束|");
    Case(L"Σ with sub- and superscript", BrlMode::English, "456 146 46 6 234 56 24 46 13 2 45 1345 5 1", L"Σ_i=1^na",
         L"Nemeth 開始|大寫 西格瑪|下標 i|等於|1|上標 n|基線 a|");
    // Geometry
    Case(L"∠ABC=90°", BrlMode::English, "456 146 1246 246 6 1 6 12 6 14 46 13 3456 35 356 45 46 16", L"∠ABC=90°",
         L"Nemeth 開始|角|大寫 A|大寫 B|大寫 C|等於|9 0|度|");
    Case(L"90°C: degree resolves when the next cell comes", BrlMode::English, "3456 35 356 45 46 16 6 14", L"90°C", L"9 0|度|大寫 C|");
    Case(L"∘ (superscript degree baseline)", BrlMode::English, "456 146 124 45 46 16 5 1245", L"f∘g", L"Nemeth 開始|f|小圓|g|");
    Case(L"⊥ ∥ ∦", BrlMode::English, "456 146 6 1 6 12 1246 1234 6 14 6 145 SP 6 15 6 124 1246 123 6 1245 6 125 SP 34 1246 123", L"AB⊥CD EF∥GH ∦",
         L"Nemeth 開始|大寫 A|大寫 B|垂直|大寫 C|大寫 D|大寫 E|大寫 F|平行|大寫 G|大寫 H|不平行|");
    Case(L"△ □ ○ ∟", BrlMode::English, "456 146 1246 2345 6 1 6 12 6 14 SP 1246 256 SP 1246 14 SP 1246 246 46 1235 12456", L"△ABC □ ○ ∟",
         L"Nemeth 開始|三角形|大寫 A|大寫 B|大寫 C|正方形|圓|直角|");
    Case(L"filled shapes and the trapezium", BrlMode::English, "456 146 1246 456 2345 1246 456 14 1246 1356", L"▲●⏢", L"Nemeth 開始|實心三角形|實心圓|梯形|");
    Case(L"⊕ ⊗ (circled operators)", BrlMode::English, "456 146 1 1246 14 456 1246 346 12456 12 1246 14 456 1246 4 16 12456 14", L"a⊕b⊗c",
         L"Nemeth 開始|a|圈加|b|圈乘|c|");
    // Brackets
    Case(L"( ) [ ] { }", BrlMode::English, "456 146 12356 1 23456 4 12356 12 4 23456 46 12356 14 46 23456", L"(a)[b]{c}",
         L"Nemeth 開始|左小括號|a|右小括號|左中括號|b|右中括號|左大括號|c|右大括號|");
    Case(L"⟨ ⟩ ⟦ ⟧", BrlMode::English, "456 146 46 46 12356 1 46 46 23456 4 456 12356 12 4 456 23456", L"⟨a⟩⟦b⟧",
         L"Nemeth 開始|左角括號|a|右角括號|左白中括號|b|右白中括號|");
    Case(L"|x| and ‖x‖", BrlMode::English, "456 146 1256 1346 1256 SP 1256 1256 1346 1256 1256", L"|x| ‖x‖",
         L"Nemeth 開始|絕對值 x|絕對值|範數 x|範數|");
    Case(L"⌈ ⌉ ⌊ ⌋", BrlMode::English, "456 146 4 45 12356 1346 4 45 23456 4 56 12356 13456 4 56 23456", L"⌈x⌉⌊y⌋",
         L"Nemeth 開始|左上取整|x|右上取整|左下取整|y|右下取整|");
    Case(L"matrix brackets 4-6-12356", BrlMode::English, "456 146 4 6 12356 1 4 6 23456", L"[a]", L"Nemeth 開始|矩陣左括號|a|矩陣右括號|");
    // Greek
    Case(L"Greek variants 46-4-letter", BrlMode::English, "456 146 46 4 1456 46 4 124 46 4 1234 46 4 15", L"ϑϕϖϵ",
         L"Nemeth 開始|變體 西塔|變體 斐|變體 派|變體 艾普西隆|");
    Case(L"Greek capitals and lowercase", BrlMode::English, "456 146 46 6 145 46 6 2456 46 1245 46 2456", L"ΔΩγω",
         L"Nemeth 開始|大寫 德爾塔|大寫 歐米伽|伽瑪|歐米伽|");
    Case(L"Å (4-6-1) and § (4-6-234)", BrlMode::English, "456 146 4 6 1 4 6 234", L"Å§", L"Nemeth 開始|埃|章節號|");
    // Arrows
    Case(L"↔ ↗ ⇑", BrlMode::English, "3456 2 1246 246 25 25 135 3456 23 1246 45 25 25 135 3456 25 1246 126 2356 2356 135", L"1↔2↗3⇑",
         L"1|左右箭頭|2|右上箭頭|3|上雙箭頭|");
    Case(L"⇚ ↠ ↦", BrlMode::English, "456 146 1246 246 456 456 1246 25 25 135 135 1246 1256 25 25 135", L"⇚↠↦", L"Nemeth 開始|左三箭頭|右雙頭箭頭|映射到|");
    Case(L"⇉ (two arrows) by longest match", BrlMode::English, "456 146 1246 25 25 135 1246 25 25 135", L"⇉", L"Nemeth 開始|右成對箭頭|");
    Case(L"uncontracted → before a space (BKey form)", BrlMode::English, "456 146 1246 25 25 135 SP", L"→ ", L"Nemeth 開始|右箭頭|");
    Case(L"⇌ (equilibrium harpoons)", BrlMode::English, "456 146 1246 25 25 4 135 1246 4 246 25 25", L"⇌", L"Nemeth 開始|右魚叉上左魚叉下|");
    Case(L"↛ starts with the bar cell", BrlMode::English, "456 146 1256 4 1246 25 25 135 12456", L"↛", L"Nemeth 開始|右箭頭加斜線|");
    Case(L"⫴ (three bars)", BrlMode::English, "456 146 1256 1256 1256", L"⫴", L"Nemeth 開始|三直線|");
    // Level indicators
    Case(L"superscript, subscript, baseline", BrlMode::English, "456 146 1346 45 23 56 24 5 346 2", L"x^2_i+1", L"Nemeth 開始|x|上標 2|下標 i|基線|加 1|");
    Case(L"subscript then baseline then a digit", BrlMode::English, "456 146 1346 56 2 5 23", L"x_12", L"Nemeth 開始|x|下標 1|基線 2|");
    Case(L"double superscript", BrlMode::English, "456 146 15 45 1346 45 23", L"e^x^2", L"Nemeth 開始|e|上標 x|上標 2|");
    // Fractions
    Case(L"nested fraction", BrlMode::English, "456 146 1456 1456 1 34 12 3456 34 14 3456", L"((a/b)/c)",
         L"Nemeth 開始|分數開始|分數開始|a|斜線 b|分數結束|斜線 c|分數結束|");
    Case(L"complex fraction 6-1456 6-34 6-3456", BrlMode::English, "456 146 6 1456 1456 2 34 23 3456 6 34 1456 25 34 256 3456 6 3456", L"((1/2)/(3/4))",
         L"Nemeth 開始|複分數開始|分數開始|1|斜線 2|分數結束|複分數線|分數開始|3|斜線 4|分數結束|複分數結束|");
    Case(L"mixed number 3 1/2", BrlMode::English, "3456 25 456 1456 2 34 23 456 3456", L"3(1/2)", L"3|帶分數開始|1|斜線 2|帶分數結束|");
    Case(L"radical inside a fraction", BrlMode::English, "456 146 1456 345 23 12456 34 23 3456", L"(√(2)/2)",
         L"Nemeth 開始|分數開始|根號 2|結束|斜線 2|分數結束|");
    // Radicals
    Case(L"radical with index: cube root of 8", BrlMode::English, "456 146 126 25 345 236 12456", L"√[3](8)", L"Nemeth 開始|根指數|3|根號 8|結束|");
    Case(L"nested radical", BrlMode::English, "456 146 345 2 346 345 23 12456 12456", L"√(1+√(2))", L"Nemeth 開始|根號 1|加|根號 2|結束|結束|");
    Case(L"index without a radical is closed by 12456", BrlMode::English, "456 146 126 25 12456", L"√[3]", L"Nemeth 開始|根指數|3|結束|");
    // Type-form and capitals
    Case(L"italic 46-56, bold 456-56", BrlMode::English, "456 146 46 56 1346 456 56 13456", L"xy", L"Nemeth 開始|斜體|x|粗體|y|");
    Case(L"capital by dot 6 and by dot 7", BrlMode::English, "456 146 6 1 17 6 1356", L"AAZ", L"Nemeth 開始|大寫 A|大寫 A|大寫 Z|");
    Case(L"comma before a digit is still a comma", BrlMode::English, "3456 2 6 23", L"1,2", L"1|逗號 2|");
    Case(L"comma before a letter is the capital sign (Nemeth rule)", BrlMode::English, "456 146 1346 6 13456", L"xY", L"Nemeth 開始|x|大寫 Y|");
    // Punctuation, primes, ellipsis, percent, currency
    Case(L"punctuation indicator forms", BrlMode::English, "3456 2 456 256 SP 3456 23 456 236 SP 3456 25 456 25", L"1. 2? 3:", L"1|點|2|問號|3|冒號|");
    Case(L"prime, double prime, ellipsis", BrlMode::English, "456 146 1346 3 SP 1346 3 3 SP 3 3 3", L"x' x'' …", L"Nemeth 開始|x|撇號|x|雙撇號|刪節號|");
    Case(L"percent, per mille", BrlMode::English, "3456 26 4 356 SP 3456 26 4 356 356", L"5% 5‰", L"5|百分號|5|千分號|");
    Case(L"currency ¢ ¥ ₣ ₦ $", BrlMode::English, "456 146 4 14 4 13456 4 124 4 1345 4 234", L"¢¥₣₦$", L"Nemeth 開始|分|日圓|法郎|奈拉|錢號|");
    Case(L"ratio and proportion", BrlMode::English, "3456 2 5 2 23 56 23 25 5 2 256", L"1:2∷3:4", L"1|比|2|比例|3|比|4|");
    Case(L"† and ¨", BrlMode::English, "456 146 456 12456 1346 16 16", L"†x¨", L"Nemeth 開始|劍號|x|雙點號|");
    Case(L"∤ (34-1256) and ∣ is the bar", BrlMode::English, "3456 23 34 1256 25", L"2∤3", L"2|不整除|3|");

    wprintf(L"\n%d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
