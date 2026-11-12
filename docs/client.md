# Client 使用與測試

`client/main.c` 包含參數解析、DNS/連線、request/response 與錯誤處理。
使用現有 protocol、I/O 與 logger；Linux 產物動態連結同一份 `libcommon.so`。

## 操作

```sh
docker compose run --rm compile make
# 先啟動 server
docker compose up -d server
# Compose 預設 command 已指定 host=server、port=8080，無查詢參數時查 hostname
docker compose run --rm client
# 覆寫 command 時，需明確指定容器內 host
docker compose run --rm client /workspace/build/client --host server HOSTNAME UPTIME HOLD 2
```

Linux 主機也可以直接：

```sh
make
./build/server
# 另一個終端機
./build/client --host 127.0.0.1 --port 8080 HOSTNAME UNAME UPTIME MEMORY LOADAVG HOLD 1
```

| 參數 | 預設 | 規格 |
|---|---|---|
| `--host` / `-H` | 127.0.0.1 | hostname、IPv4 或 IPv6；依 getaddrinfo 順序嘗試 |
| `--port` / `-p` | 8080 | 1..65535 |
| `--timeout` / `-t` | 35 秒 | 1..300；連線與每筆 response 的期限 |
| `--log-level` / `-l` | warn | error / warn / info / debug |
| `--help` / `-h` | — | 顯示 usage |

Options 放在指令之前。指令不分大小寫，最多64個，省略時為 HOSTNAME。
HOLD 後面必須接1..30秒。所有指令依序使用同一條連線，收到一筆回應才送下一筆。
成功資料按原始長度輸出至 stdout，每筆加一個換行；診斷與 server error 寫 stderr。
第一個失敗即停止後續指令並關閉連線，不會自動重送可能已被 server 處理的 request。
Exit code：0成功／help，1網路、協定、server或輸出失敗，2參數錯誤。

## Timeout 與驗證

Connect 使用 nonblocking socket、poll、SO_ERROR，所有候選地址共用連線期限；
DNS 的 getaddrinfo 是同步呼叫，不包含在該期限內。
每筆 response 的 header 與 payload 共用 CLOCK_MONOTONIC deadline，
避免逐 byte 慢速輸入持續延長等待。預設35秒可容納最大30秒的 HOLD；
使用者可以刻意設定更短的 timeout。

接收前先驗證 header magic/version/type/flags/length，最大 payload 4096 bytes。
ERROR payload 至少包含 uint16 code 與一個訊息 byte。
不假設收到的字串有 NUL，不使用 server 提供的資料作為 printf format。
Send 透過共用 helper 處理 EINTR、partial write 與 SIGPIPE。

## Tests

```sh
# macOS / Linux 都可跑 client 測試；需要 C compiler、Python 3
make test UNIT=client/main
# Linux Docker，另跑真實 server 的互通測試
docker compose run --rm compile make test UNIT=client/main
```

`test/client/main.c` 檢查數值界線、command mapping 與參數解析。
`test/client/integration.py` 有13個案例：default hostname、多指令同連線、
分段回應、4096-byte 邊界、非法 header、server error、短 error payload、
截斷回應、idle timeout、drip deadline、CLI、拒絕連線、正式 server 互通。
macOS 略過最後一項 Linux server 測試；其餘測試用獨立 Python wire fixtures。
一般與 ASan/UBSan 建置都執行相同測試；`SANITIZE=0` 可明確略過 sanitizer。
