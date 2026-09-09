# 點字顯示器六點輸入接進「點字鍵盤輸入」的設計（2026-09-09）

目標：Focus 等有 Perkins 鍵的點字顯示器，打六點注音時直接餵進 `braille-key`（`braillekey.cpp` 加 `brl_engine.cpp`），不經 Ctrl 加 F12 的開關、完全不碰實體鍵盤、任何有點字鍵的顯示器都適用。本文只做分析與設計，沒有改任何檔案。

一句話結論：**JAWS 顯示器打字的每一格，最後都是 jfw.exe 用 SendInput 合成的按鍵**，所以我們現有的 WH_KEYBOARD_LL 鉤子看得到（帶 LLKHF_INJECTED）。最乾淨的接法不是補 `[backtranslators] cht=`，而是打開 JAWS 內建的「Unicode 點字輸入」（空白鍵加點 1 3 6 7），讓每一格以 U+2800 起算的 Unicode 點字字元注入；鉤子把 vkCode 等於 VK_PACKET、掃描碼落在 U+2800 到 U+28FF 的注入事件攔下來，碼位減去 0x2800 就是點位遮罩（bit0 是點 1、bit7 是點 8，跟引擎的 kDot 定義一模一樣），直接丟給引擎。八個點、和弦、Backspace、Enter 都不會被表格查詢弄壞。這條路的前提（U+28xx 真的以 KEYEVENTF_UNICODE 注入）還要 Paul 拿 Focus 做一次實驗確認，實驗步驟在第 5 節。

---

## 1. 已查證的事實（檔案都看過，不是推測）

### 1.1 顯示器打字對照表：三層，先查特殊鍵，再查修飾鍵，最後才查點字表

`C:\Program Files\Freedom Scientific\JAWS\2026\Drivers\Braille\fsbrl\fsbrl.ini`（舊 Focus、序列埠／藍牙）與 `C:\ProgramData\Freedom Scientific\JAWS\2026\Scripts\enu\Default.JKM` 的 `[HID Braille Display Typing Keys]`／`[HID Braille Display Function Keys]`／`[HID Braille Display Modifier Keys]`（HID 顯示器，含 Focus 5 HID、Focus 6-40 `Vid_0f4e&Pid_0140`）內容一致；cht 資料夾的 Default.JKM 這三節跟 enu 的 md5 相同。

fsbrl.ini 自己的註解說明了查詢順序：
- `[Typing]`：「Mapping of dot patterns to special keys on the Qwerty keyboard. There is no need to add entries for dot patterns that are listed in the active Braille table (.jbt file) since it will be consulted if no match is found here.」→ 先查 Typing，查不到才查 .jbt。
- `[Typing Modifiers]`：點 8 加空白鍵的和弦是修飾鍵（`Dots78 Chord=Shift`、`Dots38 Chord=Ctrl`、`Dots68 Chord=Alt`、`Dots48 Chord=Windows`、`Dots58 Chord=JawsKey`、`Dots28 Chord=Insert`、`Dots18 Chord=Function`）；帶 Function 時下一格查 `[function keys]`（點 1 是 F1 … 點 1 2 3 是 F12）。
- fsbrl.jlb 裡的格式字串 `qwerty:%s%sF%d`、`qwerty:%s+%s`、`qwerty:%s`、`qwerty:%s+%c` 證實驅動把點位轉成 `qwerty:` 開頭的按鍵名字交回 jfw.exe。FsHidBraille.jlb 也有 `qwerty:` 和 `Typing Keys`/`Function Keys`/`Modifier Keys` 字串，走同一套。

在 cht 語系下，一格輸入會變成什麼：

| 按的 | 結果 | 依據 |
|---|---|---|
| 純六點格，例如點 1 3 5，沒有空白鍵 | Typing 沒這條 → 查現用 .jbt 反查成字元 → `CJAWSBrailleManager::TypeKeys` 合成按鍵。128 以下的字元用虛擬鍵碼注入（VkKeyScan 反查，大寫走 Shift），128 以上用 Unicode 字元注入（見 1.2） | fsbrl.ini 註解、default.jcf 註解、jfw.exe 字串 |
| 點 7 單獨 | `Dot7=Backspace`（Typing 節，優先於 .jbt） | fsbrl.ini 第 120 行、Default.JKM 第 4375 行 |
| 點 8 單獨 | `Dot8=enter` | fsbrl.ini 第 119 行、Default.JKM 第 4374 行 |
| 空白鍵單獨（HID） | `Braille Chord=Space:32` → 空白鍵 | Default.JKM 第 4393 行 |
| 空白鍵加點 4 5 | Tab；加點 1 2 是 Shift Tab；加點 1／4／3／6 是上下左右；加點 1 3 5 6 是 Escape；加點 1 2 3 4 5 6 是 Delete；加點 2 3 7 / 5 6 7 是 PageUp/PageDown | Typing 節 |
| 空白鍵加其他點（沒在 Typing 節） | 跑 JKM 腳本，不是打字：例如 `[focus Keys]` 空白鍵加點 1 3 4 是 BrailleToggleMode、加點 2 3 6 是 BrailleToggle8Dots、加點 1 3 6 7 是 ToggleUnicodeBrailleInput、加點 1 2 4 5 7 是 ChangeContractedBrailleSetting | Default.JKM 第 2448 行起、第 4267 行起 |
| 空白鍵加點 8 系列 | 修飾鍵前綴，下一格才是主鍵 | Typing Modifiers 節 |

### 1.2 JAWS 怎麼把字送進應用程式：SendInput，虛擬鍵碼或 KEYEVENTF_UNICODE

- `C:\ProgramData\Freedom Scientific\JAWS\2026\Settings\enu\default.jcf` `[Braille]` 節原文：「JAWS always injects characters with values above ASCII 127 as Unicode characters. Those below 127 are by default injected as virtual keycodes to allow them to be properly processed as accelerator keys in apps like WinAmp. This option allows those characters to be injected as Unicode characters directly.」對應鍵 `InjectBrailleKeysAsUnicodeChars=0`。同節還有 `useSendInputForContractedBrailleEntry=1`。
- `jfw.exe` 匯入表有 `SendInput`、`MapVirtualKeyW`、`VkKeyScanW`、`VkKeyScanExA`、`SendMessageTimeoutW`、`PostMessageW`、`AttachThreadInput`、`BlockInput`；**沒有** `keybd_event`。字串 `VkKeyScanExA failed for keyValue [{}]` 證實字元反查虛擬鍵。
- jfw.exe 的追蹤字串（UTF-16）列出整條鏈：`CJAWSBrailleManager::OnDisplayDots` → `DotsToChar` → `TypeKeys`（旁邊就是 `Space`、`Shift+Apostrophe`、`US_Unicode.jbt`）→ `SimulateQwertyKey`（旁邊 `+Space`）／`SimulateSingleCharacterQwertyScriptKey`；另有 `GetBackTranslationForTyping`、`ProcessBrailleCommandKeys`、`GetKeyScriptBinding`。
- `jhook.dll`（JAWS 的鍵盤鉤子）匯入 `SendInput` 與 `keybd_event` 兩者；`UnifiedKeyboardClient.dll` 匯入 `SendInput`；`C:\Program Files\Freedom Scientific\Shared\UnifiedKeyboard\8\UnifiedKeyboard.exe` 有 `SendKeys::InjectInputs`、`SendKeys::InjectDownAndUp`、`SendInput error:%d`、`UISI_JAWSCompatible`、`IT_Inject`、`IT_Simulate`，以及記錄格式 `[keyboard: flags=0x%x vk=0x%x (%s %s) scan=0x%x EI=%p %s]`（EI 是 dwExtraInfo，代表 JAWS 這邊會看、也可能會設 ExtraInfo）。
- 我們自己的證據：`C:\Users\paul8\AppData\Local\Vispero\JAWS-CHT\diag\helper.log` 的 Shift 追蹤，實體 Shift 按下之後放開的事件是「★注入」（例如 13:06:34 那筆：實體 Shift 下、實體 Tab 下、注入 Tab 下／上、注入 VKD8／VKD9 下…）。JAWS 平常就是吞掉實體鍵再用 SendInput 放回去。
- 各驅動 .jlb（fsbrl、FsHidBraille、BI、ElBraille）本身都不匯入 SendInput／keybd_event，只有 PostMessage／SendNotifyMessage；打字合成集中在 jfw.exe。

結論（已證）：顯示器打字是 SendInput 合成，其他行程的 WH_KEYBOARD_LL 鉤子看得到，flags 帶 LLKHF_INJECTED。我們鉤子現在對所有注入鍵 `CallNextHookEx` 放行（braillekey.cpp 第 160 行）。

### 1.3 cht 的點字表與反轉譯設定

`C:\Program Files\Freedom Scientific\JAWS\2026\FsBraille.ini`：
- `[translators]` `cht=LiblouisTrans.dll|Taiwanese.jbt`、`chs=LiblouisTrans.dll|Cantonese.jbt`、`zhh=LiblouisTrans.dll|Cantonese.jbt`。
- `[backtranslators]` 只有 `chs=LiblouisTrans.dll|US_Unicode.jbt`、`zhh=LiblouisTrans.dll|Cantonese.jbt`，沒有 cht。
- 現行檔跟 `FsBraille.ini.bak_pre_cht_patch` 的差異只有 zhh 兩行（Euro_Unicode 改 Cantonese）和檔尾換行；我們的補丁沒動 cht。
- `[Braille Devices]` 只列舊 Focus 的 VID/PID（0f4e:0100／0111／0112／0114）；Focus 5／6 HID 由 Default.JKM `[HID Braille Display Keymap Sections]` 認。
- `[Unicode Braille Support]` 列出**不支援** Unicode 點字注入的廠商 GUID／驅動（APH Refreshabraille 18、EsysIris 等 13 條）；Freedom Scientific 的 Focus 不在名單上，預設是支援。

`C:\Program Files\Freedom Scientific\JAWS\2026\Liblouis\3.38.0\LiblouisTrans.xml`：cht 是 Mode 66「Chinese (Taiwan, Mandarin)」`Table="zh-tw.ctb"`，**沒有** `AllowInput="1"`；enu 的 Mode 20「U.S. English Grade 2」有 `AllowInput="1"`。→ 縮寫輸入（Contracted Braille Input）在 cht 根本不開放。

`C:\ProgramData\...\Settings\enu\default.jcf` `[Braille Profiles]`：`cht=0|66|Taiwanese|Taiwanese|0|0`、`enu=0|20|US_Unicode|US_Unicode|0|0`。Paul 的 `%APPDATA%\Freedom Scientific\JAWS\2026\Settings\cht\DEFAULT.JCF` 是 `CHT=0|20|Taiwanese|Taiwanese|0|0|0`（第二欄被改成 20，也就是英文二級模式；不影響本設計，記一筆）。

### 1.4 Taiwanese.jbt 跟 US_Unicode.jbt 的差別（決定「格變字元」時會不會出錯）

- 兩個檔的 `[ANSI]` 節開頭一模一樣：控制字元、`!=2346`、`"=5`、`#=3456`、`'=3`、`,=6`、`-=36`、`.=46`、`/=34`、數字 `1=2 … 0=356`、`:=156`、`;=56`（用 `\59=56` 寫）、小寫 `a=1 … z=1356`、大寫 `A=17 … Z=13567`（八點制，點 7 是大寫）。
- US_Unicode.jbt 多一節 `[input]`，只有一行 `_=456`，註解說明是為了輸入點 4 5 6 時得到底線而不是 ASCII 127。Taiwanese.jbt **沒有** `[input]` 節。
- Taiwanese.jbt 的 `[Chinese unicode]` 節：`U+3100` 到 `U+3104` 五個字都寫 `1456`（跟 `?=1456` 撞）；注音符號是兩格：`U+3105=34567 135`、`U+3106=34567 1234` …；中文字是多格。
- 反查（點位→字元）碰到同一點位多個字元時，JAWS 取哪一個，檔案裡沒寫，jfw.exe 字串也看不出來。

### 1.5 JAWS 內建的「Unicode 點字輸入」

- `C:\ProgramData\Freedom Scientific\JAWS\2026\Scripts\Default.JSS` 第 27425 行：
  ```
  script ToggleUnicodeBrailleInput()
  var int setting = !GetJCFOption(OPT_BRL_INPUT_AS_UNICODE_BRAILLE)
  SetJCFOption(OPT_BRL_INPUT_AS_UNICODE_BRAILLE, setting)
  ```
  `HJConst.JSH` 第 977 行 `OPT_BRL_INPUT_AS_UNICODE_BRAILLE = 210`、第 978 行 `OPT_BRL_BACKTRANSLATE_UNICODE_BRAILLE = 211`（後者對應 default.jcf 的 `BackTranslateUnicodeBraille=0`，語音反轉譯用）。
- `default.jsd` 第 12082 行：「When toggled on, braille input writes characters in the unicode range 0x2800 through 0x28ff, and characters in this range are announced as dot patterns. The braille display must support injection as Unicode characters.」版本 JAWS/26.0。
- 綁在 `[focus Keys]`、`[focus40 Keys]`、`[HID Braille Display HID keys]` 的 `Braille Dots 1 3 6 7 Chord`（空白鍵加點 1 3 6 7）。
- `SetJCFOption` 的文件說改的是記憶體中的值；會不會寫回使用者 DEFAULT.JCF、重啟 JAWS 後是否保留，實驗確認（第 5 節）。jfw.exe 裡找不到含 Unicode 字樣的第二個設定鍵名，所以 210 號選項寫回檔案時最可能就是 `InjectBrailleKeysAsUnicodeChars`（推論，見 2.3）。

### 1.6 目前用到的注入標記（dwExtraInfo）

| 來源 | 值 | 狀態 |
|---|---|---|
| braillekey.cpp `kOurMark` | 0x4B4C5242（'BRLK'） | 每一個 SendVk／TypeText 都帶（第 415、442 行） |
| ime-helper `zhuyin_keys.cpp` `kHotkeyMark` | 0x4A43485A（'JCHZ'） | **宣告了但沒用**：第 437 行 `(void)kHotkeyMark;`，實際走 ToggleImeModeFast（WM_IME_CONTROL），不注入按鍵 |
| ime-helper `main.cpp` 第 1990 行 | 0（沒設） | 組字中把實體大千鍵吞掉再 SendInput 放回，**dwExtraInfo 是 0**，只有 keydown |
| JAWS（jfw.exe／UnifiedKeyboard） | 未知 | 有 `EI=%p` 記錄格式，值要用實驗抓 |
| `ime_shared.h` `IME_SHARED_MAGIC` 0x434D4931 | 不是按鍵標記 | 共享記憶體簽章 |

所以「沒有 BRLK 標記的注入字母鍵」不只顯示器會產生，IMEHelper 組字中重放的大千鍵也是（ExtraInfo 0），這是 ASCII 路線的先天歧義（見 3.2）。

### 1.7 環境

- 現在沒有點字顯示器接著（`Get-PnpDevice -PresentOnly` 沒有 VID_0F4E 或 Braille 裝置）。實驗要先插 Focus。
- 專案裡 `focus6\source-en` 是 Focus 6 手冊，Paul 的機器是 Focus 6 的機率高：走 FsHidBraille.jlb，對照表是 Default.JKM 的 HID 三節，不是 fsbrl.ini。兩者內容相同，設計不受影響。
- braillekey 的鉤子只在「開啟且焦點在可編輯欄位」時掛著；JAWS 的 jhook 鉤子先掛，我們後掛，LL 鉤子後掛先跑，所以我們攔下的注入事件 JAWS 的鉤子看不到。
- `braille-ime\BrailleZhuyin.jss` 是 7 月的 JAWS Script 草稿（用 JKM 把空白鍵加點位綁到腳本），`use jaws script try to solve IME problems.md` 已判定 Script 路線不可行；本設計不用它。

---

## 2. 推論（有依據，但沒直接看到程式碼或還沒實測）

### 2.1 純六點格在 cht 下怎麼變字元（沒有 backtranslator 時）

`[backtranslators]` 是給縮寫輸入（CBI，Grade 2）反轉譯用的：enu 才有 `AllowInput`、`useSendInputForContractedBrailleEntry`、`ContractedBrailleInputAllowedNow`、`IsContractedBrailleInputSupported` 都在講縮寫輸入。電腦點字（六點／八點逐格）打字走 `DotsToChar`，查的是 profile 第三／四欄的 .jbt（cht 是 Taiwanese.jbt），跟 `[backtranslators]` 無關。依據：兩者在 jfw.exe 字串裡是不同函式（`DotsToChar` 對 `GetBackTranslationForTyping`），且 chs 有 backtranslator 但 chs 的 profile 表是 Cantonese.jbt，兩件事各管各的。

推論結果：cht 下點 1 3 5 反查 Taiwanese.jbt `[ANSI]` 得 'o'，用 VkKeyScan 轉成 O 鍵的虛擬鍵碼注入；點 1 3 5 加點 7 得 'O'，注入 Shift 加 O。點 1 4 5 6 可能得 '?'（ASCII 63，虛擬鍵注入 Shift 加 /）也可能得 U+3100（Unicode 注入）— 反查取捨不明。點 4 5 6 因為沒有 `[input]` 節，可能得 ASCII 127 或 '_'。

### 2.2 任務書提議的 (A)「補 `cht=LiblouisTrans.dll|US_Unicode.jbt` 到 [backtranslators]」

依 2.1，這行只影響縮寫輸入的反轉譯，而 cht 的 liblouis 模式 66 沒開 AllowInput，縮寫輸入本來就進不去；對逐格電腦點字打字沒有作用。**建議不做。** 若實驗證明我猜錯（改了之後純六點格的字元真的變了），再回頭。

### 2.3 「Unicode 點字輸入」開啟後的行為

依 jsd 描述加 default.jcf 註解：開啟後每一格不查 .jbt，直接產生 U+2800 加點位遮罩的字元；這種字元都在 127 以上，走 KEYEVENTF_UNICODE 注入。Win32 對 KEYEVENTF_UNICODE 的行為（文件明定）：LL 鉤子收到 `vkCode` 等於 VK_PACKET（0xE7）、`scanCode` 等於該 UTF-16 碼元、flags 有 LLKHF_INJECTED 的 keydown 與 keyup 兩個事件；鉤子回傳非零可以擋掉。Unicode 點字圖樣的位元排列：U+2800 的 bit0 到 bit7 依序是點 1 到點 8，跟 `brl_engine.h` 的 `kDot1=1 … kDot8=128` 相同，減去 0x2800 就是引擎要的 dots。

待實驗確認的細節：點 7 單獨、點 8 單獨、空白鍵和弦在這個模式下是否仍走 Typing 節（我猜是：Typing 節在 .jbt 之前查，而 Unicode 模式取代的是 .jbt 那一步）；JAWS 自己會不會在注入當下就唸點位（會的話要壓掉）；設定會不會保存。

### 2.4 JAWS 注入按鍵的 dwExtraInfo

不知道。UnifiedKeyboard 有印 `EI=%p`，表示 JAWS 這邊有在看 ExtraInfo 分辨自己注入的鍵。實驗第 5 節第 3 步抓值；設計上不依賴它（依賴 VK_PACKET 的碼位範圍就夠）。

---

## 3. 設計

### 3.1 主路線 U：吃 JAWS 的 Unicode 點字注入

流程：
1. 使用者在 JAWS 開「Unicode 點字輸入」（空白鍵加點 1 3 6 7）。若實驗證明設定會保存到使用者 DEFAULT.JCF，安裝程式就在 `%APPDATA%\Freedom Scientific\JAWS\2026\Settings\cht\DEFAULT.JCF`（與 enu）的 `[Braille]` 節寫入那一鍵，使用者不必自己按。
2. 顯示器打一格 → JAWS 注入 U+28xx（VK_PACKET keydown／keyup，LLKHF_INJECTED）。
3. braillekey 的 `LowLevelKeyboardProc` 在第 160 行「注入就放行」之前加一個分支：
   - 條件：`flags` 含 LLKHF_INJECTED、`vkCode == VK_PACKET`、`0x2800 <= scanCode <= 0x28FF`、`dwExtraInfo != kOurMark`、`g_editable` 為真（焦點在可編輯欄位，沿用現有判定）。
   - 動作：keydown 時 `PostThreadMessageW(main, WM_APP_CELL, scanCode - 0x2800, 1)`（lParam 1 標記「來自顯示器」）；keydown 與 keyup 都 `return 1` 吃掉，應用程式與 JAWS 的鉤子都看不到這個 U+28xx。
   - 其他注入鍵維持放行。
4. 主執行緒 `OnCell`：來自顯示器的格**不看 `g_enabled`**（這就是「繞過 Ctrl 加 F12」），只看 `g_editable`；模式仍由 `CurrentMode()` 決定（IME 中文／英文）。
   - 中文模式：`g_engine.OnCell(dots, Chinese)` → 現有 `Act()`：大千鍵用 SendVk（帶 BRLK）注入，IME 組字，IMEHelper 照常讀候選；語音由引擎的 `spoken` 走 JAWS COM。
   - 英文模式：也要由我們轉，因為 JAWS 在這個模式下不會再送字母，只會送 U+28xx；放行等於把 ⠕ 打進文件。作法：查 `US_Unicode.jbt` `[ANSI]` 反表（八點制：點 7 是大寫、`[input]` 的 `_=456`），用 `TypeText`（KEYEVENTF_UNICODE，帶 BRLK）送出字元並唸字元名。這樣英文模式的手感跟 JAWS 原生八點電腦點字相同，只是送鍵的人換成我們。反表可以編譯期內嵌（95 個可印字元加 `_=456`），不用讀 Program Files。
   - 若 Paul 堅持英文模式一定要「JAWS 原生」而不是我們模擬：替代方案是 IME 切到英文時用 JAWS COM `RunScript("ToggleUnicodeBrailleInput")` 把模式關掉、切回中文再開。缺點：只有切換沒有設定、每次切換 JAWS 會唸一句、狀態容易對不上。列為備案，不建議。
5. 鉤子的生命週期：現在是「Ctrl 加 F12 開著且在編輯框」才掛。改成「（Ctrl 加 F12 開著）或（顯示器路線開著）且在編輯框」。顯示器路線的開關預設開、存在 `HKCU\Software\Vispero\JAWS-CHT` 另一個 DWORD（例如 `BrailleDisplayInput`），跟鍵盤模式互不影響。掛著的鉤子對實體鍵完全不動（沒開 Ctrl 加 F12 時 `DotFor` 那段不執行，實體 F D S J K L A ; 照常放行）。

顯示器上點 7、點 8、和弦的處理（都不是 VK_PACKET，走 Typing 節或腳本）：

| 顯示器動作 | JAWS 注入 | 鉤子 | 引擎 |
|---|---|---|---|
| 點 7 單獨 | VK_BACK（注入） | 放行（IME 或文件收到 Backspace） | 收到 WM_APP_PLAINKEY → `Reset()`。中文模式組字中，IME 自己刪掉最後一個注音；引擎的 pending initial／awaitingTone 也清掉，跟鍵盤路線行為不同（鍵盤路線是引擎自己送 Backspace 並唸「退格」），要在鉤子放行注入 VK_BACK 時同時發 PLAINKEY，這一點現在的程式已經做（非點鍵 keydown 都發 PLAINKEY），但它擋在 `LLKHF_INJECTED` 早退之後，要把「注入的 VK_BACK／VK_RETURN／VK_SPACE／方向鍵」也納入 PLAINKEY 通知 |
| 點 8 單獨 | VK_RETURN（注入） | 放行 | `Reset()` |
| 空白鍵單獨 | VK_SPACE（注入） | 若 `g_punctWaiting`：吃掉並送 `kSpaceCommit`（現在只認實體空白鍵，要把「注入且非 BRLK 的 VK_SPACE」也認進來）；否則放行 | 逗號句號的確認 |
| 空白鍵加點 4 5 等 Typing 節和弦 | Tab／方向鍵／Escape 等（注入） | 放行 | `Reset()` |
| 空白鍵加其他點 | JAWS 腳本，通常不注入；有的會注入（例如 PasteFromClipboard 送 Ctrl 加 V） | 放行 | 不理 |
| 空白鍵加點 8 系列修飾鍵，再一格 | 修飾鍵加主鍵（注入，主鍵可能是 VK_PACKET 以外的東西；Unicode 模式下主鍵是什麼要實驗） | 有修飾鍵按著時一律放行（`ModifierHeld()` 沿用） | 不理 |
| 六點格加點 7 或點 8（例如點 1 3 5 7） | U+2800 加 0x40 或 0x80（VK_PACKET） | 攔 | 現有 `OnCell` 對含點 7／8 的格回 `Error`（嗶一聲）；英文模式改成走八點反表（大寫） |

歧義：Unicode 路線沒有。U+2800 到 U+28FF 的 VK_PACKET 只有 JAWS 的點字輸入會產生；我們自己的 TypeText 送的是注音／中文標點／希臘字母，不會落在這個範圍；IMEHelper 重放的是虛擬鍵碼不是 VK_PACKET。「顯示器有沒有接」根本不用偵測，沒接就沒有這種事件。

### 3.2 備援路線 A：吃 JAWS 反查 .jbt 後注入的 ASCII 虛擬鍵（只在路線 U 實驗失敗時用）

- 條件：注入、非 BRLK、無修飾鍵、`vkCode` 是字母／數字／OEM 標點鍵、IME 中文模式、焦點可編輯、**且顯示器接著**。
- 字元→格：`ToUnicodeEx` 取字元，再用 `kAscii` 反查 dots（`brl_engine.cpp` 第 9 行那張表）。
- 顯示器偵測：HID 列舉 Usage Page 0x41（HID Braille Display）或 VID 0x0F4E，用 `RegisterDeviceNotification` 追蹤插拔；序列埠／藍牙舊機型抓不到，只能靠登錄檔手動開關。JAWS 內建 `BrailleInUse()` 只能從腳本呼叫，COM 的 `RunFunction` 拿不到回傳值，不可用。
- 先天問題：(1) IMEHelper 組字中重放的大千鍵 ExtraInfo 是 0，跟顯示器打出來的字母分不開（1.6）；(2) Taiwanese.jbt 反查對 1456、456 等格的結果不明（1.4）；(3) JAWS 腳本用 TypeString 送出的文字會被當成格；(4) 英文模式雖能「原樣放行」，但中文模式下所有注入字母都被攔，IMEHelper 的重放機制會壞。所以只當備援。

### 3.3 不動的部分

- IMEHelper 完全不改（規則：「It shares nothing with IMEHelper and changes nothing in it」）。
- JAWS 的檔案：`FsBraille.ini` 不加 cht backtranslator；Default.JKM 不動；可能只在使用者 DEFAULT.JCF `[Braille]` 加一鍵（實驗決定）。
- 實體鍵盤的行為：顯示器路線只碰「注入的 VK_PACKET U+28xx」和「注入的 VK_SPACE（標點待確認時）」，不碰任何實體事件。

---

## 4. 待驗證的清單（實驗前不下結論）

1. 空白鍵加點 1 3 6 7 開啟後，點 1 3 5 在記事本得到的是 ⠕（U+2815）還是 'o'。
2. 該模式下點 7 單獨、點 8 單獨、空白鍵加點 4 5 是否仍是 Backspace／Enter／Tab。
3. 該模式下 JAWS 在注入當下唸什麼（點位？字元？不唸？）。
4. 設定是否寫進 `%APPDATA%\Freedom Scientific\JAWS\2026\Settings\cht\DEFAULT.JCF`（或 enu），鍵名是什麼；重啟 JAWS 是否保留。
5. JAWS 注入事件的 dwExtraInfo 值（要 trace 建置）。
6. 未開該模式時，cht 下點 1 4 5 6、點 4 5 6、點 3 4 5 6 各得到什麼（路線 A 才需要，順手記）。

---

## 5. Paul 要做的實驗（Focus 加記事本）

前置：插上 Focus，確認 JAWS 有認到（Insert 加 J 或聽開機訊息）。開記事本，新檔，IME 切到**英文**（避免 IME 吃字）。每一步做完用 JAWS 的「讀取本行」（Insert 加上鍵）聽記事本裡是什麼，記下來；不確定的字元用 Insert 加數字鍵 5 三下聽 Unicode 碼位。

第一輪（未開 Unicode 模式，記錄 cht 反查結果）：
1. 打點 1 3 5 → 應聽到 o。
2. 打點 1 3 5 加點 7 → 應聽到大寫 O。
3. 打點 1 4 5 6 → 記錄是問號還是別的（碼位）。
4. 打點 4 5 6 → 記錄是底線還是空白／怪字元。
5. 打點 3 4 5 6 → 應是井字號。
6. 打點 7 單獨 → 應刪掉一個字元。打點 8 單獨 → 應換行。
7. 空白鍵加點 4 5 → 應是 Tab（游標跳格）。

第二輪（開 Unicode 模式）：
8. 按空白鍵加點 1 3 6 7，記下 JAWS 唸的那句話。
9. 重做第 1 到 7 步，逐一記錄：得到的字元（碼位應在 U+2800 到 U+28FF；例如點 1 3 5 應是 U+2815）、點 7／點 8／Tab 和弦是否仍是編輯鍵、JAWS 在每一格注入當下唸不唸、唸什麼。
10. 再按一次空白鍵加點 1 3 6 7 關掉，聽 JAWS 那句話；然後**不要關 JAWS**，回報。我這邊會立刻讀 `%APPDATA%\Freedom Scientific\JAWS\2026\Settings\cht\DEFAULT.JCF` 與 `enu\DEFAULT.JCF` 看有沒有多出一鍵。之後 Paul 再開一次、關 JAWS 重開、打點 1 3 5，看模式有沒有保留。

第三輪（要先出 trace 建置，見第 6 節第 0 步）：
11. 設環境變數 `JAWSCHT_BRAILLE_TRACE=1` 啟動 braillekey，把 Ctrl 加 F12 保持關閉，在記事本重做第 9 步；`braillekey.log` 會記下每一個注入事件的 vk、scan、flags、ExtraInfo、時間。回報 log 即可。

回報格式：每一步一行「步驟編號：聽到什麼／碼位」，不知道就寫不知道。

---

## 6. 實作計畫（實驗通過路線 U 之後）

0. **trace 模式**（實驗第三輪要用，先做）：`braillekey.cpp` 加環境變數 `JAWSCHT_BRAILLE_TRACE`；鉤子對每個 `LLKHF_INJECTED` 事件呼叫 `Log("注入 vk=%lu scan=0x%04lX flags=0x%lX extra=0x%llX %s")`（只寫檔、不阻塞；每格最多 2 行，寫檔成本可接受）。這個模式下鉤子必須掛著：暫時用「trace 開著就在編輯框掛鉤子」的條件。
1. **鉤子**：在第 160 行之前插入 VK_PACKET 分支（3.1 第 3 點）；把注入的 VK_BACK／VK_RETURN／VK_SPACE／方向鍵納入 PLAINKEY 通知；標點待確認時接受注入的 VK_SPACE。
2. **狀態**：新增 `std::atomic<bool> g_displayRoute`（登錄檔 `BrailleDisplayInput`，預設 1）；`ApplyHookWish()` 的 want 改成 `(g_enabled || g_displayRoute) && g_editable`；`OnCell` 加來源參數，顯示器來源不檢查 `g_enabled`。
3. **引擎**：`BrlEngine` 加 `Display8Dot(unsigned dots)`（或在 braillekey 端）處理英文模式：內嵌 US_Unicode.jbt `[ANSI]` 反表加 `_=456`，回 `Text` 加字元名；中文模式沿用 `Chinese()`。含點 7／8 的格在中文模式仍回 Error（八點電腦點字之後再談）。
4. **語音**：沿用 `Act()` 的 `spoken`。若實驗第 9 步發現 JAWS 自己也會唸，先看 `SpeakBacktranslatedDotPatterns=0` 能不能壓掉，不行就在說話前 `StopSpeech()`。
5. **安裝**：實驗第 10 步若證明設定會寫進 DEFAULT.JCF，`patch` 流程加一條「寫入使用者 DEFAULT.JCF `[Braille]` 那一鍵」，並在解除安裝時還原；若不會保存，使用手冊寫「開機後按空白鍵加點 1 3 6 7 一次」，或用 JAWS COM `RunScript("ToggleUnicodeBrailleInput")` 在 braillekey 啟動時打一次（需再驗證 COM 能否叫到內建腳本，並處理它會唸一句的副作用）。
6. **測試**：`tools/brl_test.cpp` 已能離線驅動引擎；補一組「dots 來自 U+28xx 減 0x2800」的案例，確認位元排列。真機測試順序：記事本英文模式 → 記事本中文模式打「溫」（點 1 2 3 4 5 6 加聲調）→ LINE／Claude 的 Chromium 編輯框 → 有 JAWS 腳本會注入文字的情境（Word 自動更正）確認不誤攔。

不做的事：不改 FsBraille.ini 的 `[backtranslators]`；不改 IMEHelper；不偵測顯示器有無（路線 U 用不到）；不動 Ctrl 加 F12 的語意。
