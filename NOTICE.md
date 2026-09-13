# 第三方資料來源與授權 / Third-party data and licences

程式碼（`src/*.cpp`、`src/*.h`、`tools/*.py`、`tools/brl_test.cpp`）以 MIT 授權釋出（見 LICENSE）。
下列檔案是**資料表**，由 `tools/` 裡的產生器從公開的點字標準或第三方表格整理而來，只含「哪一方對應哪個符號」這類事實，不含任何第三方程式碼：

| 檔案 | 內容 | 來源 | 授權／說明 |
|---|---|---|---|
| `src/syllables.inc` | 國語點字音節 → 大千鍵序 | 淡江大學「無字天書」BKey 的 `Phn.tbl`（`data/Phn.tbl`），加 4 個教育部《重編國語辭典》才有的音節 | 教育部國語點字（國家標準）的音節事實；Phn.tbl 原檔隨附供產生器使用 |
| `src/tw_braille.inc` | 國語點字標點、符號、多方序列 | 由 NVDA 附加元件 BrlIMEHelper（鄭博丞，GPL v3）的規則資料 `bopomofo.json` 讀出的**資料事實**重新產生 | 只取國家點字標準的事實，未複製該專案任何程式碼；產生器 `tools/gen_bopomofo_rules.py` 為本專案自寫 |
| `src/nemeth.inc` | Nemeth 數學點字符號 | Liblouis 的 `nemeth.ctb`、`nemethdefs.cti`、`en-chardefs.cti` | Liblouis 為 LGPL 2.1 或更新版；本檔為由其表格導出的資料 |
| `src/us_unicode.inc` | 8 點北美電腦點字（ASCII 一方一字） | North American Braille Computer Code（NABCC）；產生器可從 JAWS 的 `US_Unicode.jbt` 重產，並套用台灣慣例（1256 = 反斜線） | 公開的點字編碼標準 |

執行時若機器上裝有 JAWS，程式會改讀 JAWS 安裝目錄裡的 `US_Unicode.jbt`；沒有 JAWS 就用內建的 `us_unicode.inc`。

「JAWS」為 Vispero 的商標。本專案為獨立作品，與 Vispero 或 Freedom Scientific 無關；JAWS 版（隨 JAWS 起落、整合更深、更快）另行隨 JAWS 中文延伸套件發行，不在本儲存庫。

## NVDA Controller Client

`vendor/nvda/nvdaControllerClient64.dll` is NV Access's controller client,
版本 2026.2，授權 GNU LGPL 2.1（全文在同一個資料夾的 `license.txt`）。本程式
**以檔名在執行期動態載入它，沒有連結、沒有修改**，把它刪掉程式照樣執行，只是
NVDA 底下不會出聲。要換成別的版本，直接覆蓋同名檔案即可。
來源：https://www.nvaccess.org/files/nvda/releases/2026.2/nvda_2026.2_controllerClient.zip
