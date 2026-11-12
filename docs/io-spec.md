# Socket I/O helper 規格與實作提示

`util/io.c` 已依 `util/io.h` 實作；以下記錄介面契約與測試範圍。

## 使用範圍

- 使用 blocking stream socket。正式應用是 TCP，unit test 用 `socketpair()`。
- buffer 是 binary bytes，不是 C string；不能使用 `strlen()` 決定長度。
- length > 0 時，呼叫端須提供至少 length bytes 的有效 buffer；NULL 回傳 ERROR，errno=EINVAL。
- length == 0 時立即回傳 OK，不存取 buffer、不操作 fd；允許 NULL 與 fd=-1。
- fd 由呼叫端持有，helper 不負責 close、shutdown、建立連線或解析 protocol。
- 不配置動態記憶體，不輸出收到的原始資料。

## sysinfo_send_all(fd, buffer, length)

| 情況 | 回傳值／行為 |
|---|---|
| 完整送出 length bytes | `SYSINFO_IO_OK` |
| 單次 send 僅送出部分資料 | 推進 offset，繼續送剩下的資料 |
| send 回傳 -1，errno 是 EINTR | 重試，保留已完成進度 |
| 其他 send 錯誤 | `SYSINFO_IO_ERROR`，保留該錯誤的 errno |
| 尚有資料但 send 回傳 0 | 不可無限迴圈；建議 errno=EIO 並回傳 ERROR |
| 對端關閉連線 | 回傳 ERROR，不能讓 SIGPIPE 終止程序 |

成功只表示 bytes 已交給本機 socket，並不保證對端應用已處理。
失敗前可能已送出部分資料；此 API 沒有回傳已送出長度，呼叫端不應盲目重送整份封包。

## sysinfo_recv_all(fd, buffer, length)

| 情況 | 回傳值／行為 |
|---|---|
| 收齊 length bytes | `SYSINFO_IO_OK` |
| 尚未收到任何 byte，recv 回傳 0 | `SYSINFO_IO_EOF` |
| 已收到部分資料，recv 回傳 0 | `SYSINFO_IO_TRUNCATED` |
| recv 回傳 -1，errno 是 EINTR | 重試，保留已完成進度 |
| 其他 recv 錯誤 | `SYSINFO_IO_ERROR`，保留該錯誤的 errno |

只讀取要求的長度，不可把下一個 frame 的資料也讀走。
EOF/TRUNCATED/ERROR 時 buffer 可能已包含接收前綴，不保證維持原狀。
已收齊所需資料即成功，不需再讀一次確認對端是否關閉。

## Timeout 與 SIGPIPE

目前 helper 不提供整個操作的 deadline。呼叫端可設定 `SO_RCVTIMEO` /
`SO_SNDTIMEO`，timeout 所產生的 `EAGAIN` / `EWOULDBLOCK` 要回傳 ERROR，
不能無限重試。Socket timeout 並不等於整個 frame 的總時間上限。

Linux 可在 `send()` 使用 `MSG_NOSIGNAL`；macOS 可透過 `setsockopt()`
設定 `SO_NOSIGPIPE`。可用 `#ifdef` 區分平台，並處理 setsockopt 失敗。
避免在共用 helper 中把整個程序的 SIGPIPE disposition 改為忽略。

## 一點提示

| 函式／型別 | Header | 用途 |
|---|---|---|
| `send()`、`recv()` | `<sys/socket.h>` | socket 收送；回傳 `ssize_t` |
| `setsockopt()` | `<sys/socket.h>` | macOS SIGPIPE 控制、呼叫端 timeout |
| `errno`、`EINTR` 等 | `<errno.h>` | 分辨錯誤；只有回傳 -1 時才查看 errno |
| `size_t` | `<stddef.h>` | buffer 長度與累計 offset |
| `ssize_t` | `<sys/types.h>` | 可表示 -1 的收送結果 |

維護 `size_t done = 0`；下一次收送的位置是 `bytes + done`，
長度是 `length - done`。先判斷 n > 0，才把 n 轉成 size_t 累加。
不要直接對 void pointer 做位移；send 用 `const unsigned char *`，
recv 用 `unsigned char *`。回傳錯誤前若記錄 log，注意它可能改變 errno。

## 測項與執行

```sh
make test UNIT=util/io
```

此指令執行 log level 矩陣與 sanitizer 測試。
測試檔本身可獨立檢查語法：

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -I. -fsyntax-only test/util/io.c
```

16 個測項：

1. 零長度 send。
2. 零長度 recv。
3. 無效 send fd，保留 EBADF。
4. 無效 recv fd，保留 EBADF。
5. 含 NUL/high-bit bytes 的 send，且 helper 不關閉 socket。
6. 精確長度 recv、buffer 前後界線與下一次接收。
7. 無資料 EOF。
8. 部分資料後 EOF。
9. 資料剛好收齊，對端隨後 EOF。
10. 對端關閉後 send，不被 SIGPIPE 終止。
11. recv timeout 回傳 ERROR。
12. peer 分段寫入，接收程序同時受到不帶 SA_RESTART 的重複 signal。
13. 256 KiB send，搭配小 socket buffer 與獨立讀取程序。
14. 非零長度的 NULL buffer 回傳 EINVAL。
15. 對端不讀取造成 send timeout，保留 EAGAIN/EWOULDBLOCK。
16. recv 已讀部分資料後 timeout，回傳 ERROR 並保留已讀前綴。

每個測項在獨立程序中執行，parent watchdog 約 6 秒，結束時清理 peer。
讀寫對照端使用原生 read/write，避免 send_all/recv_all 相同的 bug 互相掩蓋。

限制：signal 測試確認收到 signal 且資料完整，不會攔截系統呼叫來證明每次
都產生 EINTR；大資料測試也不保證每個平台會回傳 short send。
目前尚無強制 send-side EINTR 與零進度回傳的故障注入測試。
