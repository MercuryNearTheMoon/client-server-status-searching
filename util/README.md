# 共用函式庫

此目錄存放 client 與 server 共用的 header，以及對應的 `.c` 骨架。
`protocol.c` 已實作 header 編解碼；`io.c` 已實作完整 socket 收送；
`logger.c` 仍是 TODO 骨架。
尚未完成整個共用函式庫。

- `common.h`：預設 port、payload 限制與共用回傳碼。
- `protocol.h`：訊息 header、指令編號與編解碼介面。
- `io.h`：完整傳送／接收資料的 socket helper 介面。
- `logger.h`：log level、logging 介面與編譯期 debug 巨集。

使用時加入 include 路徑 `-Iutil`，例如 `#include "protocol.h"`。
Makefile 將 `.c` 以 `-fPIC` 編譯，再以 `-shared` 產生 `build/lib/libcommon.so`，
client 和 server 使用 `-Lbuild/lib -lcommon` 動態連結同一份函式庫。
執行檔的 RUNPATH 設為 `$ORIGIN/lib`；容器的 `LD_LIBRARY_PATH`
也包含 `/workspace/build/lib`。

目前 protocol 只定義固定 header 與指令編號；request/response payload
格式及錯誤內容仍待實作前確認。Header 結構是主機記憶體表示，
不可直接透過 socket 傳送，必須明確編解碼為 12 bytes。
