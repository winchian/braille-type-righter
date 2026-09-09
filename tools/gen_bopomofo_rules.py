# 從 BrlIMEHelper（NVDA 附加元件，鄭博丞 Bo-Cheng Jhan，GPL v3）的 bopomofo.json
# 產生 src/tw_braille.inc，並把它的音節規則跟我們從 BKey Phn.tbl 產生的
# syllables.inc 逐筆比對，寫成 docs/braille-table-comparison-<日期>.md。
#
# 授權說明：只讀 bopomofo.json 的「資料」——國語點字（教育部國字點字）的標點、
# 符號、聲韻調方位。這些是國家點字標準的事實，不是程式碼；本檔跟引擎裡的比對
# 邏輯全部自己寫，沒有複製 brl_tables.py 或 __init__.py 的任何一行（那些是 GPL，
# 依專案規定不能進商用產品）。tw_braille.inc 只含資料。
#
# bopomofo.json 的語意（自己讀 brl_tables.py 分析出來的，這裡用文字重述）：
#   * 鍵是 Unicode 點字方（U+2800 起，位元 i = 第 i+1 點；U+2800 = 空方）字串，
#     或用 [方集合] 寫的字元類別。值是字串（直接輸出）或規則名稱清單。
#   * 純方序列的鍵是「符號」：整串打完才輸出；尾端的空方 ⠀ 就是「打完按空白」
#     （Paul 的慣例：逗號 2-3 加空白）。前綴狀態不輸出，繼續等。
#   * [聲母][韻母][聲調] 與 [韻母][聲調] 是音節：每個位置用同名字典
#     （CONSONANTS／RHYMES／TONES）翻成注音，字典的鍵是正規表示式，可帶
#     前後文條件（lookahead／lookbehind），在整個緩衝區上比對。
#       - ⠅⠚⠑（ㄍㄘㄙ）後面接 ㄧ／ㄩ 系韻母時是 ㄐㄑㄒ。
#       - ⠱（帀，1-5-6）跟在 ㄓㄔㄕㄖㄗㄘㄙ 後面不輸出，其他位置是 ㄦ。
#       - 緩衝區開頭的 ⠴（3-5-6，ㄟ）接輕聲或一聲時是 ㄧㄛ；開頭的 ⠢（2-6，ㄝ）
#         接二聲時是 ㄧㄞ。
#     多個規則同時完整符合時，JSON 裡先出現的贏（所以 ⠨⠁ 是 α 不是 ㄧㄤ˙）。
#   * ⡼（3-4-5-6-7）接聲母／韻母／聲調＝只打那個注音符號本身；⡼⡅⡚⡑＝ㄐㄑㄒ、
#     ⡼⡁＝˙。這些含第 7 點，我們的中文模式不收（列在報告裡）。
#
# 用法：python gen_bopomofo_rules.py [bopomofo.json 路徑]
import datetime
import json
import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
src_dir = os.path.join(here, '..', 'src')
docs_dir = os.path.join(here, '..', 'docs')
phn_path = os.path.join(here, '..', 'data', 'Phn.tbl')
json_path = sys.argv[1] if len(sys.argv) > 1 else \
    r'E:\URC\Vispero files\點字輸入法-NVDA\extracted\globalPlugins\BrlIMEHelper\bopomofo.json'
mapping_path = os.path.join(os.path.dirname(json_path), 'keyboard_mappings.json')

BLANK = '\u2800'


def is_cell(ch):
    return '\u2800' <= ch <= '\u28FF'


def mask(ch):
    return ord(ch) - 0x2800


def dots_text(ch):
    m = mask(ch)
    return '0' if m == 0 else ''.join(str(i + 1) for i in range(8) if m & (1 << i))


def cells_text(s):
    return '-'.join(dots_text(c) for c in s)


# ---------------------------------------------------------------- 唸法
# 標點與符號的中文名稱（跟 gen_tables.py 的 NAMES 同一套用語）。
NAMES = {
    '，': '逗號', '、': '頓號', '。': '句號', '；': '分號', '：': '冒號', '？': '問號', '！': '驚嘆號',
    '…': '刪節號', '—': '破折號', '（': '左括號', '）': '右括號',
    '《': '左書名號', '》': '右書名號', '〈': '左單書名號', '〉': '右單書名號',
    '【': '左方頭括號', '】': '右方頭括號', '「': '左引號', '」': '右引號', '『': '左雙引號', '』': '右雙引號',
    '°': '度', '＆': '和', '＊': '星號', '×': '乘', '÷': '除', '±': '正負', '≠': '不等於', '∞': '無限大',
    '≒': '約等於', '≦': '小於等於', '≧': '大於等於', '∩': '交集', '∪': '聯集', '▽': '倒三角形',
    '⊥': '垂直', '∠': '角', '√': '根號', '∵': '因為', '∴': '所以', '≡': '恆等於', '∥': '平行',
    '↑': '上箭頭', '↓': '下箭頭', '←': '左箭頭', '→': '右箭頭', '△': '三角形', '□': '正方形',
}
# 希臘字母（同 gen_tables.py 的 GREEK；大寫前加「大寫 」）。
GREEK = {
    '\u03B1': '阿爾法', '\u03B2': '貝塔', '\u03B3': '伽瑪', '\u03B4': '德爾塔', '\u03B5': '艾普西隆', '\u03B6': '澤塔',
    '\u03B7': '伊塔', '\u03B8': '西塔', '\u03B9': '約塔', '\u03BA': '卡帕', '\u03BB': '拉姆達', '\u03BC': '謬',
    '\u03BD': '紐', '\u03BE': '克西', '\u03BF': '歐米克戎', '\u03C0': '派', '\u03C1': '羅', '\u03C2': '尾西格瑪',
    '\u03C3': '西格瑪', '\u03C4': '陶', '\u03C5': '宇普西隆', '\u03C6': '斐', '\u03C7': '卡伊', '\u03C8': '普賽',
    '\u03C9': '歐米伽',
}
for ch, name in GREEK.items():
    NAMES[ch] = name
    NAMES[ch.upper()] = '大寫 ' + name

# ---------------------------------------------------------------- 讀 JSON
with open(json_path, encoding='utf-8') as f:
    rules = json.load(f)

symbols = []          # (cells string, text)
patterns = []         # (list of cell-classes, [rule names])  音節規則
escapes = []          # 含 ⡼ 的規則，只進報告
dicts = {}            # CONSONANTS / RHYMES / TONES: [(compiled regex, output)]


def parse_key(key):
    """把鍵拆成「每個位置可接受的方集合」清單；不是純方序列就回 None。"""
    out = []
    i = 0
    while i < len(key):
        ch = key[i]
        if ch == '[':
            j = key.index(']', i)
            out.append(set(c for c in key[i + 1:j] if is_cell(c)))
            i = j + 1
        elif is_cell(ch):
            out.append({ch})
            i += 1
        else:
            return None
    return out


for key, val in rules.items():
    if isinstance(val, dict):
        dicts[key] = [(re.compile(cond), act) for cond, act in val.items()]
        continue
    classes = parse_key(key)
    if classes is None:
        continue                                   # "stuff": "value" 之類
    if isinstance(val, str):
        if any(mask(c) & 0x40 and len(cls) == 1 for cls in classes for c in cls) and key[0] == '\u287C':
            escapes.append((key, val))
        elif all(len(cls) == 1 for cls in classes):
            cells = ''.join(next(iter(cls)) for cls in classes)
            if cells.strip(BLANK):
                symbols.append((cells, val))
            # 純空方＝空白鍵本身，程式讓它直接過，不進表
        continue
    # 規則名稱清單
    if key[0] == '\u287C':
        escapes.append((key, val))
    else:
        patterns.append((classes, val))


def evaluate(brl):
    """自己寫的規則求值：回傳 (輸出字串, 是否為前綴狀態)；完全不符合回 None。
    與 BrlIMEHelper 相同的語意：先看符號（JSON 順序），再看音節規則；完整符合
    的第一個規則決定輸出；只有前綴符合則 intermediate。"""
    output = None
    intermediate = False
    for cells, text in symbols:
        if cells == brl:
            if output is None:
                output = text
        elif cells.startswith(brl):
            intermediate = True
    for classes, names in patterns:
        n = len(brl)
        if n > len(classes):
            continue
        if not all(brl[i] in classes[i] for i in range(n)):
            continue
        if n < len(classes):
            intermediate = True
            continue
        if output is not None:
            continue
        res = ''
        for i, name in enumerate(names):
            piece = name
            for rx, act in dicts.get(name, []):
                hit = False
                for m in rx.finditer(brl):
                    if m.start() == i:
                        piece = act
                        hit = True
                        break
                if hit:
                    break
            res += piece
        output = res
    if output is None and not intermediate:
        return None
    return (output or '', intermediate)


# ---------------------------------------------------------------- tw_braille.inc
def c_wstr(s):
    return 'L"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


unnamed = [t for _, t in symbols if t not in NAMES]
if unnamed:
    sys.exit('沒有唸法的符號：' + ' '.join(unnamed))

inc_path = os.path.join(src_dir, 'tw_braille.inc')
with open(inc_path, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// Generated by tools/gen_bopomofo_rules.py from BrlIMEHelper bopomofo.json - do not edit by hand.\n')
    f.write('// Data only: the punctuation and symbol cells of Taiwanese (教育部) braille as that\n')
    f.write('// GPL add-on lists them; no code of it is used here (see the generator).\n')
    f.write('// { cells "dots-dots" (0 = a blank cell: the space bar), text to type, spoken name }\n')
    for cells, text in symbols:
        f.write('{ "%s", %s, %s },  // %s\n' % (cells_text(cells), c_wstr(text), c_wstr(NAMES[text]), cells))
print('tw_braille.inc:', len(symbols), 'symbols')

# ---------------------------------------------------------------- 與 Phn.tbl 比對
ASCII = " a1b'k2l@cif/msp\"e3h9o6r^djg>ntq,*5<-u8v.%[$+x!&;:4\\0z7(_?w]#y)="
CANON = {'{': '[', '|': '\\', '}': ']', '~': '^'}
BPMF = 'ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ ˊˇˋ˙'
with open(mapping_path, encoding='utf-8') as f:
    mp = json.load(f)
STANDARD = mp[1]['STANDARD'][0]
if len(STANDARD) != len(mp[0]) or mp[0] != BPMF:
    sys.exit('keyboard_mappings.json 的注音順序跟預期不同')
KEY = dict(zip(BPMF, STANDARD))
TONES = {'\u2804': ' ', '\u2802': 'ˊ', '\u2808': 'ˇ', '\u2810': 'ˋ', '\u2801': '˙'}
TONE_NAME = {'\u2804': '一聲', '\u2802': '二聲', '\u2808': '三聲', '\u2810': '四聲', '\u2801': '輕聲'}


def ascii_to_cells(s):
    return ''.join(chr(0x2800 + ASCII.index(CANON.get(ch, ch))) for ch in s)


def keys_of(zhuyin):
    return ''.join(KEY[c] for c in zhuyin if c in KEY and c not in ' ˊˇˋ˙')


def zhuyin_of_keys(keys):
    inv = {v: k for k, v in KEY.items()}
    return ''.join(inv.get(k, '?') for k in keys)


phn = []
raw = open(phn_path, 'rb').read().decode('ascii').replace('\r\n', '\n').split('\n')
for i in range(0, len(raw) - 2, 3):
    cells, keys, tones = raw[i].strip(), raw[i + 1].strip(), raw[i + 2].strip()
    if cells and keys:
        phn.append((''.join(CANON.get(c, c) for c in cells), keys, tones))
phn.sort()
phn_by_cells = {c: (k, t) for c, k, t in phn}

agree, differ, rejected, phn_only_tone = [], [], [], []
for cells, keys, tones in phn:
    ub = ascii_to_cells(cells)
    per_tone = {}
    for t in TONES:
        r = evaluate(ub + t)
        per_tone[t] = None if r is None or r[1] and not r[0] else r[0]
    got = {t: (keys_of(v) if v else None) for t, v in per_tone.items()}
    if all(g == keys for g in got.values()):
        agree.append(cells)
    elif all(g is None for g in got.values()):
        rejected.append((cells, keys))
    else:
        differ.append((cells, keys, got))

# BrlIMEHelper 接受、Phn.tbl 沒有的組合
cons = [c for cls in patterns[0][0][:1] for c in cls] if patterns else []
rhymes = sorted(patterns[0][0][1]) if patterns else []
cons = sorted(patterns[0][0][0])
extra = []
for c in [''] + cons:
    for r in rhymes:
        ub = c + r
        cells = ''.join(ASCII[mask(x)] for x in ub)
        if cells in phn_by_cells:
            continue
        outs = {}
        for t in TONES:
            res = evaluate(ub + t)
            if res and res[0]:
                outs.setdefault(res[0].rstrip(' ˊˇˋ˙'), []).append(TONE_NAME[t])
        if outs:
            extra.append((cells, ub, outs))

# 教育部國語辭典（jaws-localization/moedict-data/chars.json，每個字的注音）裡真有的
# 音節：拿來看 Phn.tbl 漏了哪些真音節。
moedict_path = os.path.join(here, '..', '..', 'moedict-data', 'chars.json')
real_syllables = {}
if os.path.isfile(moedict_path):
    with open(moedict_path, encoding='utf-8') as f:
        for ch, readings in json.load(f).items():
            for r in readings:
                key = re.sub('[ˊˇˋ˙ ]', '', r)
                if key:
                    real_syllables.setdefault(key, []).append(ch)
missing_real = []
for cells, ub, outs in extra:
    for z in outs:
        if z in real_syllables:
            missing_real.append((cells, ub, z, real_syllables[z][:6]))

# 符號方與音節方的碰撞
syl_cells = set(cons) | set(rhymes) | set(TONES)
collide = []
for cells, text in symbols:
    first = cells[0]
    if first in syl_cells:
        role = '聲母' if first in cons else '韻母' if first in rhymes else '聲調'
        collide.append((cells, text, role))

today = datetime.date.today().isoformat()
os.makedirs(docs_dir, exist_ok=True)
rep_path = os.path.join(docs_dir, 'braille-table-comparison-%s.md' % today)
with open(rep_path, 'w', encoding='utf-8', newline='\n') as f:
    w = f.write
    w('# 國語點字表比對：BrlIMEHelper bopomofo.json 對 BKey Phn.tbl（%s）\n\n' % today)
    w('產生者：`tools/gen_bopomofo_rules.py`（自己寫的規則求值器，沒有用到 BrlIMEHelper 的程式碼；'
      'bopomofo.json 只當國家點字標準的資料讀）。\n\n')
    w('- Phn.tbl（引擎現用 `src/syllables.inc`）：%d 筆音節（一或兩方 → 大千鍵）。\n' % len(phn))
    w('- bopomofo.json：%d 個符號／標點規則（進 `src/tw_braille.inc`），%d 條音節規則，'
      '%d 條 ⡼ 逃脫規則（含第 7 點，中文模式不收）。\n\n' % (len(symbols), len(patterns), len(escapes)))
    w('## 一、Phn.tbl 的每一筆，BrlIMEHelper 怎麼說\n\n')
    w('- 完全一致（五個聲調都得到同一組大千鍵）：**%d 筆**\n' % len(agree))
    w('- 有出入：**%d 筆**\n' % len(differ))
    w('- BrlIMEHelper 不接受（沒有規則能配）：**%d 筆**\n\n' % len(rejected))
    if differ:
        w('### 有出入的\n\n| 方（ASCII） | 點位 | Phn.tbl 鍵（注音） | BrlIMEHelper 各聲調 |\n|---|---|---|---|\n')
        for cells, keys, got in differ:
            ub = ascii_to_cells(cells)
            desc = '；'.join('%s→%s' % (TONE_NAME[t], ('%s（%s）' % (g, zhuyin_of_keys(g))) if g else '拒絕') for t, g in got.items())
            w('| `%s` | %s | `%s`（%s） | %s |\n' % (cells, cells_text(ub), keys, zhuyin_of_keys(keys), desc))
        w('\n')
    if rejected:
        w('### BrlIMEHelper 不接受的\n\n| 方（ASCII） | 點位 | Phn.tbl 鍵（注音） |\n|---|---|---|\n')
        for cells, keys in rejected:
            ub = ascii_to_cells(cells)
            w('| `%s` | %s | `%s`（%s） |\n' % (cells, cells_text(ub), keys, zhuyin_of_keys(keys)))
        w('\n')
    w('## 二、BrlIMEHelper 接受、Phn.tbl 沒有的聲韻組合：%d 組\n\n' % len(extra))
    w('BrlIMEHelper 是規則式（任何聲母配任何韻母都翻），不檢查音節存不存在；Phn.tbl 只列真有的音節，'
      '引擎對查不到的組合響鈴不猜字（Paul 定案）。這些組合大多不是國語音節，列出供查核有沒有漏的真音節。\n\n')
    if real_syllables:
        w('### 其中教育部國語辭典真有字的：%d 組（已加進 gen_syllables.py 的 EXTRA）\n\n' % len(missing_real))
        w('| 方（ASCII） | 點位 | 音節 | 大千鍵 | 字例 |\n|---|---|---|---|---|\n')
        for cells, ub, z, chars in missing_real:
            w('| `%s` | %s | %s | `%s` | %s |\n' % (cells, cells_text(ub), z, keys_of(z), ''.join(chars)))
        w('\n### 全部\n\n')
    w('| 方（ASCII） | 點位 | BrlIMEHelper 輸出 |\n|---|---|---|\n')
    for cells, ub, outs in extra:
        desc = '；'.join('%s（%s）%s' % (z, keys_of(z), '' if len(ts) == 5 else '［' + '、'.join(ts) + '］') for z, ts in outs.items())
        w('| `%s` | %s | %s |\n' % (cells, cells_text(ub), desc))
    w('\n## 三、符號規則裡第一方跟音節方相同的（引擎靠下一方分辨）：%d 條\n\n' % len(collide))
    w('| 方序列 | 點位 | 輸出 | 第一方在音節裡是 |\n|---|---|---|---|\n')
    for cells, text, role in collide:
        w('| %s | %s | %s | %s |\n' % (cells, cells_text(cells), text, role))
    w('\n## 四、⡼ 逃脫規則（未實作）\n\n')
    for key, val in escapes:
        w('- `%s` → %s\n' % (key, val if isinstance(val, str) else '／'.join(v or '（略）' for v in val)))
    w('\n## 五、規則語意摘要\n\n')
    w('- 符號：整串方打完才輸出；尾端空方（U+2800）＝空白鍵（Paul 的「打完按空白」）。\n')
    w('- 音節：[聲母][韻母][聲調] 或 [韻母][聲調]，聲調必打。ㄍㄘㄙ（1-3、2-4-5、1-5）接 ㄧ／ㄩ 系韻母時翻成 ㄐㄑㄒ；'
      '帀（1-5-6）在 ㄓㄔㄕㄖㄗㄘㄙ 後不輸出、其他位置是 ㄦ；開頭的 3-5-6 接輕聲或一聲＝ㄧㄛ（其餘＝ㄟ）；'
      '開頭的 2-6 接二聲＝ㄧㄞ（其餘＝ㄝ）。\n')
    w('- 同時完整符合時 JSON 先出現的規則贏：4-6 接 1 是 α，不是 ㄧㄤ˙。\n')
    w('- 聲母後直接聲調（省略帀）BrlIMEHelper 拒絕；我們的引擎接受並自動補帀。\n')
print('report:', rep_path)
print('agree %d, differ %d, rejected %d, extra %d, collide %d' % (len(agree), len(differ), len(rejected), len(extra), len(collide)))
