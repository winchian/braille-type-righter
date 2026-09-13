// The braille-cell engine: turns cells typed on the six/eight-key keyboard into
// the keys to send and the words to speak. Pure logic, no Windows calls - so
// the offline test in tools/brl_test.cpp can drive it without a keyboard, an
// IME or a screen reader.
//
// Two regimes, chosen by the operating system's own IME mode (the bare Shift
// the user already knows; this program never invents a switch of its own -
// see feedback_braille-chord-space-rule):
//
//   Chinese   cells are 國語點字. An initial waits for its rime, a rime alone
//             is a syllable, and the pair is looked up in the table generated
//             from the BKey Phn.tbl (syllables.inc) to get the 大千 keys that
//             make the Microsoft Zhuyin IME compose it. The tone cell that
//             follows becomes the tone key (dot 3 = first tone = Space).
//             Punctuation and symbols are the sequences of tw_braille.inc,
//             generated from the rule table of the BrlIMEHelper NVDA add-on
//             (data only - see the generator): a trailing blank cell in a
//             sequence is the writer's confirming Space (逗號 2-3 then Space,
//             as Taiwanese braille typing has always done), multi-cell
//             symbols (《 1-2-6 1-2-6, × 4 1-6, → 1-2-4-6 2-5 2-5 1-3-5) are
//             recognised by longest match. Cells that are both a syllable
//             and the start of a symbol (2-5 is ㄨㄛ and the first cell of
//             the colon) are held until the next cell says which: a tone
//             cell makes them the syllable, a blank or the symbol's next
//             cell makes them the symbol.
//
//   English   cells are characters of JAWS's own 8-dot US computer braille
//             table (US_Unicode.jbt: dot 7 = capital, digits are the lowered
//             cells 2 = 1, 23 = 2 ... 356 = 0, every ASCII sign one cell).
//             The table is read from the JAWS installation at start-up; the
//             copy generated into us_unicode.inc stands in if it is missing.
//             There is no capital sign and no number sign: dot 6 alone is a
//             comma and 3-4-5-6 is the Nemeth numeric indicator (below).
//
//   Nemeth    inside English, the Nemeth numeric indicator (3-4-5-6) followed
//             by a digit, a minus (3-6) or a decimal point (4-6) opens a
//             mathematics context in which the multi-cell Nemeth symbols
//             generated from JAWS's Liblouis nemeth tables (nemeth.inc) are
//             recognised by longest match: 4-16 ×, 46-34 ÷, 46-13 =, 34-46-13
//             ≠, 46-1 α, 46-6-1 Α, 1246-135 → and so on; single cells that
//             are no symbol are still the computer-braille characters, so
//             letters and digits type as usual. The table holds every
//             character the JAWS tables define (relations, sets, logic,
//             calculus, geometry, arrows, brackets, Greek with variants,
//             currency) and the Nemeth indicators, which type the nearest
//             plain-text form and are always spoken: fractions (1456 34
//             3456, complex 6-1456 6-34 6-3456, mixed 456-1456 34
//             456-3456) type ( / ), radicals √( ... ) with the index
//             126 typed as √[3](8), superscript 45 ^, subscript 56 _,
//             baseline 5 (spoken only), underscript 146 _( ... ),
//             capital 6-letter, italic 46-56, bold 456-56. The context survives spaces
//             (the numeric indicator is simply given again before the next
//             number, as Nemeth requires) and ends at Enter, at any other
//             editing key, when the focus moves, or when the IME switches to
//             Chinese. 3-4-5-6 followed by anything else is the # sign.
//             The Nemeth code switch indicators of the BANA rules open and
//             close the context explicitly: 4-5-6 1-4-6 opens it (for an
//             expression that starts with a letter, x^2+1) and it then
//             stays open across Enter until 4-5-6 1-5-6 closes it.
//             The BKey Sign.tbl sequences are accepted there too.
//
// Dot 7 alone is Backspace and dot 8 alone is Enter in both regimes, as on a
// Perkins-style keyboard; when cells are waiting, dot 7 takes the last one
// back instead.
//
// Every output carries `spoken`: what JAWS should say for it. The keys this
// program injects are invisible to IMEHelper and to JAWS's own echo (both
// ignore injected keys by design), so a blind typist hears nothing unless the
// program says it itself.
#pragma once

#include <string>
#include <vector>

enum class BrlMode { Chinese, English };

struct BrlOutput {
    enum Action { None, Keys, Text, Backspace, Enter, Error };
    Action       action = None;
    std::wstring text;      // Keys: 大千 key characters to press; Text: characters to type as-is;
                            // Backspace/Enter: characters to type BEFORE the key (cells that were waiting)
    std::wstring spoken;    // what to announce through JAWS (may be empty)
    bool         again = false;   // after acting on this, call OnCell once more with the same cell
};

// Cells as a bit mask: bit 0 = dot 1 ... bit 7 = dot 8.
const unsigned kDot1 = 1, kDot2 = 2, kDot3 = 4, kDot4 = 8, kDot5 = 16, kDot6 = 32, kDot7 = 64, kDot8 = 128;

// The North American ASCII braille character for a six-dot cell, or 0.
char CellToAscii(unsigned dots6);

// The English table. The embedded copy (us_unicode.inc) is in force until
// LoadJbt reads a JAWS .jbt file (US_Unicode.jbt); it returns false and
// leaves the embedded table alone if the file cannot be read. Sections are
// honoured as JAWS does: [input] first, then [ANSI], then the Unicode
// sections in file order, first definition of a cell wins; only single-cell
// entries count, control characters and the braille-pattern block do not.
bool BrlLoadJbt(const wchar_t* path);
int  BrlEnglishTableSize();
// Character for an 8-dot cell in the English table, or 0.
wchar_t BrlEnglishChar(unsigned dots);
// Sizes of the Chinese tables: syllables (syllables.inc) and punctuation /
// symbol sequences (tw_braille.inc), for the start-up log.
int BrlSyllableTableSize();
int BrlChineseSymbolCount();
// Size of the Nemeth table (nemeth.inc plus the BKey sequences it lacks).
int BrlNemethSymbolCount();

class BrlEngine {
public:
    // Everything typed so far is forgotten: focus moved, the mode changed,
    // the program was switched off. Leaves the Nemeth context.
    void Reset();
    // An editing key (arrow, Enter, Escape...) was pressed: cells waiting are
    // dropped and a numeric-indicator context ends, but a context opened
    // with the Nemeth opening indicator stays.
    void PlainKey();

    BrlOutput OnCell(unsigned dots, BrlMode mode);

    // Chinese: the cells waiting would be completed by a blank cell (the
    // space bar): 逗號 2-3 then Space.
    bool HasPendingPunct() const;
    // The space bar was pressed while that was so: feed the blank cell.
    BrlOutput CommitPunct();

    // Cells are waiting for more (a Nemeth sequence, the numeric indicator,
    // a Chinese initial or a cell that may start a symbol). The space bar and
    // Enter must flush them first.
    bool HasPendingCells() const { return !seq_.empty() || held_ != 0 || !cbuf_.empty(); }
    // Resolve what is waiting without further input (Space or Enter was
    // pressed). Keeps the Nemeth context.
    BrlOutput Flush();

    bool InNemeth() const { return nemeth_; }

private:

    BrlOutput Chinese(unsigned dots, bool blank);   // blank: the space bar (dots ignored)
    BrlOutput ChineseFlush();
    BrlOutput SyllableKeys(unsigned rime, unsigned tone);   // a rime alone, tone 0 = none yet
    BrlOutput English(unsigned dots);
    BrlOutput Nemeth(unsigned dots);
    BrlOutput Plain(unsigned dots);           // one cell of the English table
    BrlOutput ResolveSeq(bool final);         // longest match on seq_

    std::vector<unsigned> cbuf_;  // Chinese: cells held - an initial, a rime that may start a symbol, or a symbol prefix
    bool awaitingTone_ = false;  // Chinese: syllable keys sent, tone cell expected next

    BrlMode lastMode_ = BrlMode::English;   // a change of regime forgets everything
    bool nemeth_ = false;            // English: inside the Nemeth context
    bool nemethSticky_ = false;      // opened with 456-146: only 456-156 closes it
    unsigned held_ = 0;              // English: 3456 or 456 seen, waiting for the next cell to say what it was
    std::vector<unsigned> seq_;      // Nemeth: cells of a symbol sequence so far
    int fracDepth_ = 0;              // Nemeth: fraction indicators open (simple, complex or mixed)
    int termDepth_ = 0;              // Nemeth: groups the termination indicator (12456) closes - radicals, underscripts
    bool radIndex_ = false;          // Nemeth: index-of-radical indicator (126) seen, its radical (345) not yet
    bool lastWasDigit_ = false;      // Nemeth: the previous character typed was a digit (4-6 then a digit is a decimal point)
};
