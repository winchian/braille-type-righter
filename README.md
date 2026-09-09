# Braille Typewriter 點字打字機

讓視障者在 Windows 上用一般鍵盤的 **F D S J K L**（點 1 到 6）加 **A ;**（點 7、8）以六點／八點點字直接打字，跨所有應用程式：中文走微軟注音輸入法（程式把點字方翻成大千鍵序送給輸入法，同音字選擇、智慧組字全部交給輸入法），英文走北美 8 點電腦點字，數學走 Nemeth 點字。每打一方由螢幕閱讀軟體唸出（目前接 JAWS；沒有 JAWS 時只打字不唸）。

An on-screen-reader-agnostic six/eight-key braille chord input for Windows: Taiwanese Mandarin braille is converted to the 大千 key sequence of the Microsoft Bopomofo IME (so the IME does the character selection), English uses 8-dot North American computer braille, and mathematics uses Nemeth code. Works in every application because it is a stand-alone low-level keyboard hook, not a screen-reader add-on.

## 使用方式

* 啟動 `BrailleTypewriter.exe`（無視窗，常駐）。**Ctrl+F12** 開關，預設關；狀態記在登錄檔，重開機仍記得。
* 只在鍵盤焦點停在可編輯的文字欄位時才接管點鍵；離開編輯框立刻放手，點鍵回到原本的字母。
* 中文／英文由**輸入法本身的模式**決定（就是你平常按的那顆 Shift）；本程式不另外發明切換鍵。
* **中文**：聲母方等韻母方，韻母方後接聲調方（點 3＝一聲＝空白、點 2＝二聲、點 4＝三聲、點 5＝四聲、點 1＝輕聲）。標點照台灣點字規則打完按空白確認：逗號 2-3、句號 3-6、頓號 6、分號 5-6、問號 1-4-5-6、驚嘆號 1-2-3；書名號、引號、括號等多方符號連打。
* **英文**：一方一字（JAWS 的 US 8 點電腦點字表）；點 7 加字母是大寫；數字直接打下移方（2＝1、2-3＝2 … 3-5-6＝0）；點 6 是逗號；1-2-5-6 是反斜線。
* **數學（Nemeth）**：3-4-5-6 接數字進入數學模式，或 4-5-6 1-4-6 開、4-5-6 1-5-6 關。乘 4 1-6、除 4-6 3-4、等於 4-6 1-3、不等於 3-4 4-6 1-3、希臘字母 4-6 加字母、分數 1-4-5-6 … 3-4 … 3-4-5-6、根號 3-4-5 … 1-2-4-5-6，共 386 個符號，完整清單見 `src/nemeth.inc` 檔頭。

## 建置

需要 MSYS2 的 ucrt64 g++（`C:\msys64\ucrt64\bin`）。

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Test
```

產物 `build\BrailleTypewriter.exe`，單一靜態連結執行檔。`-Test` 會另外建置並執行 `tools/brl_test.cpp`（離線測試 217 項，不需鍵盤、輸入法或螢幕閱讀軟體）。

## 設計要點

* `src/brl_engine.*`：純邏輯的點字引擎，沒有任何 Windows 呼叫，離線可測。
* `src/braillekey.cpp`：WH_KEYBOARD_LL 鉤子（獨立執行緒，回呼永不等待）、焦點事件訂閱（SetWinEventHook，只認前景程式自己的焦點，輸入法候選字窗與提示窗一律不理）、輸入法模式快取（切換鍵後排程重問到輸入法真的切了為止）、整個方一次 SendInput、語音走獨立執行緒信箱。八顆點鍵的處理路徑上沒有任何排隊、Sleep 或去彈跳。
* 記錄檔：`%LOCALAPPDATA%\Vispero\JAWS-CHT\diag\braillekey.log`；在同目錄放一個 `braille_timing.txt` 就會多記每一方的耗時。

## 資料來源與授權

程式碼 MIT（`LICENSE`）。點字表的來源與各自授權見 `NOTICE.md`。
