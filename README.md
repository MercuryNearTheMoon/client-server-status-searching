# Midterm Linux 開發環境

## 本機 unit test / CI

共用函式庫測試可在 macOS 或 Linux 執行；Linux 的 `make test` 也包含 server。
macOS 會明確略過 server，請用 Docker 執行 server 測試：

```sh
make test
# 只測一個 unit
make test UNIT=util/protocol
make test UNIT=util/logger
make test UNIT=util/io
make test UNIT=client/main
# 等同於
sh scripts/ci-local.sh
sh scripts/ci-local.sh util/protocol
```

腳本以 C11 與 `-Werror` 編譯。Protocol unit 包含 12 個 encode 測項、
15 個 decode 測項與 1 個 round-trip 測項，共 28 個。
涵蓋固定 wire bytes、邊界值、非法欄位、NULL 指標、不對齊輸入，
以及拒絕輸入時 output 不變。失敗時回傳非零 exit code。
Protocol、I/O、logger、client 與 Linux server 均已實作。

測試依照 source 路徑拆分：`test/util/protocol.c` 測試 `util/protocol.c`，
`test/util/logger.c` 驗證正式 logger、巨集過濾、runtime level、截斷、errno 與 fork 輸出。
每個測試檔案有自己的 `main()`，可獨立編譯執行。
`test/util/io.c` 有 16 個 socket I/O 測項，規格與提示見 [docs/io-spec.md](docs/io-spec.md)。
`io.c` 已實作完整收送、EOF、EINTR、errno 保留與跨平台 SIGPIPE 防護。
可用 `make test UNIT=util/io` 單獨驗證。Server 測試使用 Linux：
`docker compose run --rm compile make test UNIT=server/main`。
Server 指令、協定與測試規格見 [docs/server.md](docs/server.md)。
Client 參數、多指令連線與測試見 [docs/client.md](docs/client.md)。
Client 測試在 macOS 會略過真實 Linux server 的互通案例，Docker 內則全部執行。

測試矩陣包含 `ENABLE_DEBUG=0/1` 與五種編譯期 log level；logger unit 驗證 stderr
的等級過濾。最後執行 AddressSanitizer 與 UndefinedBehaviorSanitizer。
各組 stdout/stderr 保存在 `build/tests/<unit>/`。若編譯器不支援 sanitizer，可明確使用
`SANITIZE=0 make test`；可用 `make test CC=clang` 選擇編譯器。
CI runner 也能執行同一個腳本，目前沒有部署步驟。

### Logger

`util/logger.c` 已實作，log 巨集預設直接使用正式 backend，不需 `SYSINFO_USE_LOGGER`。
輸出格式為 `[LEVEL] UTC時間 pid=程序編號 訊息`，目的地為 stderr。
每筆最多 512 bytes（含換行），過長訊息以 `...` 結尾；訊息內 CR/LF 轉成空白。
每筆使用一次 bounded write（EINTR 時重試），避免多次 fprintf 交錯。
此設計對 pipe/FIFO 的 512-byte 寫入可提供原子性；一般終端與其他裝置不保證。
輸出採 best effort，失敗不遞迴記錄或回傳錯誤；logger 保留呼叫前的 errno。
不要從 signal handler 呼叫，也不保證對多執行緒的同步。

`SYSINFO_COMPILE_LOG_LEVEL` 是編譯期上限：
`-1` 關閉、`0` error、`1` warn、`2` info（預設）、`3` debug。
Runtime 預設 info，可用 `sysinfo_log_set_level(SYSINFO_LOG_DEBUG)` 調整。
Debug 訊息必須同時滿足 `ENABLE_DEBUG=1`、編譯期上限為 3，以及 runtime level 為 debug。
直接呼叫 `sysinfo_log()` 也會遵守過濾規則。無效 level 與 NULL format 會被忽略。
fork 後 child 繼承當時的 runtime level，各 process 後續獨立修改。

```c
sysinfo_log_set_level(SYSINFO_LOG_DEBUG);
SYSINFO_INFO_LOG("server starting on port %d", port);
SYSINFO_DEBUG("connection fd=%d", fd);
```

Connection ID 尚由呼叫端放入訊息，例如 `"connection=%d ..."`。
Logger 不會接管 SIGPIPE；若 stderr 導向已關閉的 pipe，仍依應用程式的 signal 設定處理。

單獨編譯 protocol 測試時，需一起連結 logger：

```sh
mkdir -p build
cc -std=c11 -Wall -Wextra -Wpedantic -I. \
  test/util/protocol.c util/protocol.c util/logger.c -o build/test-protocol
./build/test-protocol
```

目前包含容器環境、Makefile、共用函式庫、client 與 Linux server。

使用 Docker Compose 提供 `compile`、`server`、`client` 三個容器服務。
三者使用相同 Debian 開發映像，包含 GCC、Make、CMake、GDB、
`readelf`、`ps`、`ss` 與 `tcpdump`，並將專案掛載至 `/workspace`。
Client 與 server 均可透過容器執行。

程式結構：`client/main.c` 放 client 流程，`server/main.c` 放 server 流程、
sysinfo 查詢與 shared-memory lazy TTL cache；共用介面及實作放在 `util/`。
可用 `make client`、`make server` 分別建置，或用 `make` 一次建置兩者。

## 建立環境與編譯

先啟動 Docker Desktop，再執行：

```sh
docker compose build
docker compose run --rm compile bash
```

上面的指令可直接進入 Linux 編譯環境。也可以直接執行：

```sh
docker compose run --rm compile
# 或傳入 Makefile 參數
docker compose run --rm compile make ENABLE_DEBUG=1
```

預設 `make` 會建置 client、server 與共用函式庫。
可先執行 `docker compose run --rm compile make objects`，只編譯 object files。
Makefile 以 Linux 為目標，產物為 `build/client`、`build/server` 與
`build/lib/libcommon.so`；執行檔透過 `$ORIGIN/lib` 尋找共用函式庫。
使用 `make clean` 清除產物。更改 `ENABLE_DEBUG` 或編譯旗標時，請先
`docker compose run --rm compile make clean` 再重新編譯。

`compile` 是一次性服務，完成編譯即退出。編譯產物保留在主機專案目錄，
server 與 client 可直接使用。請在容器內編譯 Linux 執行檔；macOS 執行檔
無法直接在這些容器執行。預設沿用主機 CPU 架構（包含 Apple Silicon）。

## 執行 server / client

Compose 預設約定如下；實作程式時可遵循約定，或修改 `command` 配合實際 CLI：

- Server 執行檔：`build/server`，預設監聽 `0.0.0.0:8080`。
- Client 執行檔：`build/client`，接受 `--host server --port 8080`。
- 共用 header 與函式庫原始碼放在 `util/`；介面說明見 `util/README.md`。
- 共用動態函式庫：放在 `util/`、`build/lib/` 或 `lib/`；已設定 `LD_LIBRARY_PATH`。

程式與 Makefile 就緒並編譯成功後：

```sh
docker compose up -d server
docker compose logs -f server
# 在另一個終端機執行
docker compose run --rm client
docker compose down
```

client 透過 Compose 預設網路的 DNS 名稱 `server` 連線，不能使用 `localhost`。
server 必須監聽 `0.0.0.0`，才能接受其他容器連線。
主機也可透過 `127.0.0.1:8080` 連線；若主機 port 被占用，可用
`SERVER_PORT=9090 docker compose up -d server` 更改主機 port，容器內仍使用 8080。
請等待 server 日誌顯示已開始監聽再啟動 client；目前尚無應用程式健康檢查。

尚無執行檔時，也可以分別開啟 shell：

```sh
docker compose run --rm --service-ports server bash
docker compose run --rm client bash
```

`client` 與 `compile` 放在 `tools` profile，避免一般 `docker compose up`
自動執行它們。使用 `docker compose run` 指定服務時不需要額外開啟 profile。

## 除錯與封包擷取

```sh
docker compose exec server ps -ef
docker compose exec server ss -tnp
docker compose exec server ldd /workspace/build/server
docker compose exec server tcpdump -i any -nn -s 0 -w /workspace/capture.pcap 'tcp port 8080'
```

按 Ctrl-C 結束擷取。跨容器流量走虛擬網卡，因此使用 `-i any`。
這些是開發容器，預設以 root 執行；不使用 privileged 模式。
`init: true` 負責容器 init 行為，server 仍必須自行 `waitpid()` 回收其子程序。
