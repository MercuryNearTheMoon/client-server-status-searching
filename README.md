# Remote System Information Service

以 C 實作的 Linux TCP client–server，提供系統資訊查詢。Server 採用
**fork-per-connection**，各 child 共用具有 TTL 的 shared-memory cache；
client 與 server 動態連結同一份 `libcommon.so`。

## 功能

- 查詢 `HOSTNAME`、`UNAME`、`UPTIME`、`MEMORY`、`LOADAVG`。
- `HOLD 1..30` 保持連線，支援並行服務展示。
- 同一 TCP 連線可依序執行多個命令。
- 固定12-byte protocol header、長度驗證與結構化錯誤回應。
- Request deadline、連線數限制、child 回收與 robust mutex 恢復。
- Compile-time / runtime logging，以及 unit、integration、sanitizer 測試。

## 快速開始：Docker

需要 Docker Desktop 或 Docker Engine 與 Compose。Clone repository 後，
在專案根目錄執行：

```sh
docker compose build
docker compose run --rm compile make
docker compose up -d server
```

執行 client：

```sh
# 預設查詢 HOSTNAME
docker compose run --rm client

# 同一連線依序查詢多個項目
docker compose run --rm client /workspace/build/client --host server HOSTNAME UNAME UPTIME MEMORY LOADAVG

# 並行展示用
docker compose run --rm client /workspace/build/client --host server HOLD 5
```

查看 log 與停止：

```sh
docker compose logs --tail 30 server
docker compose down
```

Client 在 Compose 網路中使用 hostname `server`。主機可透過
`127.0.0.1:8080` 連線；若port被占用：

```sh
SERVER_PORT=9090 docker compose up -d server
```

容器內仍使用8080。請等待 server 顯示 `READY` 後執行 client。
`compile` 與 `client` 是按需執行的 tools profile，不會隨一般 `compose up` 啟動。

## 本機建置

完整服務需要 **Linux**、C11 compiler、Make 與 POSIX/pthread library。
測試另需 **Python 3**。macOS 可跑共用函式庫與client測試；server請用Docker。

```sh
make
./build/server
# 另一個終端機
./build/client --host 127.0.0.1 --port 8080 HOSTNAME UPTIME
```

產物為 `build/client`、`build/server`、`build/lib/libcommon.so`。
Makefile以 `$ORIGIN/lib` 設定函式庫搜尋路徑；也可用 `make client`、
`make server`、`make objects` 分別建置。`make clean` 清理應用程式編譯產物。
切換主機／容器環境或編譯旗標時先clean，避免混用不同平台的object files。

## 測試

```sh
# Linux Docker：完整測試，包含真實 client/server 互通
docker compose run --rm compile make test

# 本機測試；macOS 會明確略過 Linux server
make test

# 單一 unit
make test UNIT=util/protocol
make test UNIT=util/io
make test UNIT=util/logger
make test UNIT=client/main
docker compose run --rm compile make test UNIT=server/main
```

| 測試 | 範圍 |
|---|---|
| Protocol | 28案例：編解碼、邊界、NULL、對齊與round-trip |
| I/O | 16案例：partial I/O、EOF、EINTR訊號、timeout、SIGPIPE |
| Logger | 編譯／runtime等級、格式、截斷、errno與多程序輸出 |
| Client | CLI unit tests與13個integration cases |
| Server | CLI/cache unit tests與12個integration cases，包括10個並行client |

腳本以 `-Werror` 建置，預設也執行 ASan/UBSan。macOS client 測試略過正式
Linux server 互通案例。編譯器不支援sanitizer時可明確使用 `SANITIZE=0 make test`；
可用 `make test CC=clang` 選擇編譯器。測試失敗回傳非零exit code。
腳本位於 `scripts/ci-*.sh`，可供本機或CI runner呼叫，沒有部署步驟。

## 專案結構

```text
client/main.c          CLI、連線與request/response
server/main.c          accept/fork、系統資訊、cache與清理
util/                  protocol、I/O與logger共用函式庫
test/client/           client unit/integration tests
test/server/           server unit/integration tests
test/util/             共用函式庫測試
test/demo/             獨立受限baseline，非正式server
scripts/               測試、證據收集、封包分析與打包工具
docs/                  使用方式與技術規格
```

- [Client 使用與協定驗證](docs/client.md)
- [Server 架構、CLI 與 wire protocol](docs/server.md)
- [I/O 契約與測項](docs/io-spec.md)
- [共用函式庫](util/README.md)

## Logging

輸出到stderr，包含UTC時間、PID與等級；每筆最多512 bytes，過長截斷。
預設compile-time ceiling為info，runtime可用 `--log-level` 調整。
要啟用DEBUG，編譯期必須同時設定 `ENABLE_DEBUG=1` 與
`SYSINFO_COMPILE_LOG_LEVEL=3`，再於執行時選擇debug：

```sh
docker compose run --rm compile make clean
docker compose run --rm compile make ENABLE_DEBUG=1 \
  CFLAGS='-O2 -g -std=c11 -Wall -Wextra -Wpedantic -pthread -DSYSINFO_COMPILE_LOG_LEVEL=3'
docker compose run --rm client /workspace/build/client --host server --log-level debug HOSTNAME
```

Logger保留errno，不提供signal-handler safety或防竄改稽核。
程序fork後繼承runtime level，後續各自獨立設定。

## 重現展示證據

Repository保留展示程式與工具；**`submission/` 是本機交付資料，已由Git忽略，
不隨repository提供**。需要證據時可自行產生：

```sh
docker compose run --rm compile sh scripts/collect-evidence.sh
```

腳本會建立 `submission/evidence/`，收集10連線展示、debug四組比較、受限
baseline before/after、loopback pcap及分析。需要容器具備tcpdump擷取權限；
僅擷取此次測試server的port。Baseline只在localhost執行，有連線數與時間上限。

來源碼打包工具也可使用：

```sh
python3 scripts/package-source.py
```

輸出至被忽略的 `submission/`。`build/`、編譯產物、`.env`、log及Python cache
同樣不納入版本控制。報告與截圖另行提交，不是執行程式或測試的必要檔案。
