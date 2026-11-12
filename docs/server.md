# Server 與 wire protocol

Server 為 Linux 程式，全部流程保留在 `server/main.c`。每個 TCP connection
由一個 fork child 處理，支援同一連線多次請求，沒有建立 thread。

## 建置與執行

```sh
docker compose build
docker compose run --rm compile make server
docker compose up -d server
# Linux 容器內的 unit / integration / sanitizer 測試
docker compose run --rm compile make test UNIT=server/main
```

Linux 主機可直接 `make server`、`make test UNIT=server/main`。
Server 動態連結 `build/lib/libcommon.so`，使用 `$ORIGIN/lib` 尋找函式庫。
Client 已實作；`make` 同時建置 client/server，也可用 `make server` 單獨建置。
macOS `make test` 會明確略過 Linux server；指定 server unit 則提示用 Docker。

| CLI | 預設 | 範圍／意義 |
|---|---|---|
| `--port` | 8080 | 0..65535；0 自動選 port，stdout 印出 READY 與實際 port |
| `--cache-ttl` | 1 | 0..3600 秒；0 每次查詢更新 |
| `--timeout` | 10 | 1..300 秒；每筆完整 header+payload 的接收期限 |
| `--max-clients` | 64 | 1..256 child processes |
| `--log-level` | info | error / warn / info / debug |

監聽 `0.0.0.0`。Debug 仍須編譯期開啟，參考 README 的 logger 說明。
`--timeout` 同時限制 idle connection 與慢速逐 byte 輸入，不包含 HOLD 執行時間。
回應 socket 的每次 send timeout 為 1 秒。

## Frame

固定 12-byte header，欄位沿用 `util/protocol.h`：
magic `SINF`、version=1、type、flags=0、payload length（上限4096）。
多位元組整數皆為 network byte order。Request type=1，成功 type=2，錯誤 type=3。

| Command byte | Request payload | Response UTF-8 text |
|---|---|---|
| 1 HOSTNAME | `01` | hostname |
| 2 UNAME | `02` | sysname nodename release version machine |
| 3 UPTIME | `03` | `uptime_seconds=N` |
| 4 MEMORY | `04` | `total_bytes=N free_bytes=N` |
| 5 LOADAVG | `05` | `load1=N load5=N load15=N` |
| 6 HOLD | `06` + uint32 秒數 | `held_seconds=N` |

一般 request payload 恰為 1 byte，HOLD 恰為 5 bytes，秒數1..30。
Response payload 不包含結尾 NUL。MEMORY 的 free 是 sysinfo.freeram，
不是 Linux 的 MemAvailable；容器內數值反映 Linux API 視圖，不保證等同 cgroup 配額。

Error payload 是 uint16 error code + UTF-8 訊息：

| Code | 意義 |
|---|---|
| 1 | 無效 header、截斷 frame |
| 2 | 未知 command |
| 3 | 無效參數或 payload 長度 |
| 4 | request 接收超時 |
| 5 | 系統資訊讀取或 fork 失敗 |
| 6 | concurrent client 上限已滿 |

無效 header、截斷 frame、timeout 回應錯誤後關閉連線；完整但 command/argument
錯誤的 request 回應後可繼續下一筆。對端已斷線時不保證能收到錯誤回應。

## Cache 與程序生命週期

Parent 在 fork 前 mmap 共享快取與 process-shared robust mutex。
Handler 鎖住後檢查 CLOCK_MONOTONIC timestamp，TTL 過期才重新讀取全部五種
資訊，成功後一次更新快取與 timestamp。失敗回傳錯誤，不把舊快取冒充新資料。
資料複製到 child 的區域 buffer 後立即解鎖，再寫 socket；HOLD 不持有 cache lock。
持鎖 child 死亡時，下一個 handler 處理 EOWNERDEAD，作廢快取並重新更新。

Parent 關閉 accepted fd；child 關閉 listener。Parent 以 poll 的 100 ms 週期
執行 waitpid(WNOHANG)，回收 child，減少 signal handler 內的共享狀態。
SIGTERM/SIGINT 停止 accept，終止並回收所有 child，再移除 cache。
若 child 未退出，短暫寬限後 SIGKILL，避免 HOLD 或 stopped child 阻擋關閉。

## Tests

`test/server/main.c`：CLI 數值範圍、首次採樣、TTL cache hit/expiry/disable、
跨程序快取、持鎖 child 死亡恢復、所有五種 sysinfo。

`test/server/integration.py`：12 個真實 TCP 測試，涵蓋查詢／pipelining、分段
封包、非法 header、指令參數、截斷 frame、idle/drip timeout、10 個同時 HOLD
加上額外查詢、disconnect/reaping、client 上限、關閉清理與 CLI/bind 失敗。
透過 Linux /proc 獨立檢查 child PIDs 與回收，不使用正式 client 實作互相驗證。

測試先跑一般建置，再以 ASan/UBSan 重跑；`SANITIZE=0` 可明確略過。
目前不包含 tcpdump 證據與整份作業的 robustness before/after demo。

## 本次驗證

已在 Debian bookworm / Linux ARM64 Docker 環境以 GCC 執行：

- Server unit tests（CLI/cache/robust mutex）。
- 12 個 TCP integration tests，一般與 ASan/UBSan 建置皆通過。
- Protocol 28 測項、I/O 16 測項、logger 等級矩陣與 sanitizer。
- `ldd build/server` 確認載入 `/workspace/build/lib/libcommon.so`。

測試紀錄在忽略提交的 `build/server-tests.log` 與
`build/server-linux-validation.log`；沒有產生封包擷取證據。
