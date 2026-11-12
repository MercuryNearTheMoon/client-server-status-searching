"""Parse the actual pcap without external Python packages; IPv4 TCP only."""
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
import json
import struct

root = Path('submission/evidence')
raw = (root/'capture.pcap').read_bytes()
magic = raw[:4]
assert magic in (b'\xd4\xc3\xb2\xa1', b'\xa1\xb2\xc3\xd4'), 'expected microsecond pcap'
endian = '<' if magic == b'\xd4\xc3\xb2\xa1' else '>'
linktype = struct.unpack_from(endian+'I', raw, 20)[0]
assert linktype in (1, 113, 276), f'unsupported linktype {linktype}'
linksize = {1:14, 113:16, 276:20}[linktype]
port = json.loads((root/'observations.json').read_text())['concurrency']['port']
offset = 24
counts = Counter()
streams = defaultdict(lambda: defaultdict(list))
times = []
while offset < len(raw):
    seconds, usecs, length, original = struct.unpack_from(endian+'IIII', raw, offset)
    offset += 16
    packet = raw[offset:offset+length]
    offset += length
    assert len(packet) == length
    ip = packet[linksize:]
    if not ip or ip[0] >> 4 != 4 or ip[9] != 6: continue
    ip_length = (ip[0] & 15) * 4
    tcp = ip[ip_length:struct.unpack_from('!H', ip, 2)[0]]
    if len(tcp) < 20: continue
    source, destination, seq = struct.unpack_from('!HHI', tcp)
    if port not in (source, destination): continue
    flags = tcp[13]
    counts['packets'] += 1
    for name, mask in [('SYN', 2), ('ACK', 16), ('FIN', 1), ('RST', 4)]:
        counts[name] += bool(flags & mask)
    times.append(seconds + usecs / 1_000_000)
    client_port = destination if source == port else source
    direction = 'response' if source == port else 'request'
    payload = tcp[(tcp[12] >> 4)*4:]
    streams[client_port][direction].append((seq, payload))

frames = []
for client_port, directions in streams.items():
    for direction, chunks in directions.items():
        chunks = sorted((seq, payload) for seq, payload in chunks if payload)
        assembled = b''
        next_seq = None
        for seq, payload in chunks:
            if next_seq is None: next_seq = seq
            assert seq <= next_seq, 'capture gap; cannot reconstruct stream'
            overlap = next_seq-seq
            if overlap < len(payload):
                assembled += payload[overlap:]
                next_seq += len(payload)-overlap
        while assembled:
            assert len(assembled) >= 12, 'incomplete application header'
            magic, version, kind, flags, length = struct.unpack('!IBBHI', assembled[:12])
            assert (magic, version, flags) == (0x53494e46, 1, 0) and length <= 4096
            assert len(assembled) >= 12+length
            frames.append(dict(client_port=client_port, direction=direction, type=kind,
                               header=assembled[:12].hex(' '), payload_hex=assembled[12:12+length].hex(' '),
                               payload=assembled[12:12+length]))
            assembled = assembled[12+length:]
assert counts['SYN'] and counts['FIN'] and counts['RST'] and frames
requests = [f for f in frames if f['direction'] == 'request']
responses = [f for f in frames if f['direction'] == 'response']
assert len(requests) == 16 and len(responses) == 16
commands = Counter(f['payload'][0] for f in requests)
first = next(f for f in requests if f['payload'] == b'\1')
reply = next(f for f in responses if f['client_port'] == first['client_port'])
start = datetime.fromtimestamp(min(times), timezone.utc).isoformat()
end = datetime.fromtimestamp(max(times), timezone.utc).isoformat()
text = f'''# 實際封包分析

來源：[capture.pcap](capture.pcap)。以 tcpdump 在 Linux 容器 loopback 擷取，
過濾 `tcp port {port}`，pcap linktype={linktype}。以下數字由原始 pcap 解析，非示意值。

- UTC 範圍：{start} ～ {end}
- IPv4 TCP packets：{counts['packets']}
- TCP streams（以 client port 區分）：{len(streams)}
- 含 SYN 的 packet：{counts['SYN']}（包含 SYN+ACK）
- 含 ACK：{counts['ACK']}；含 FIN：{counts['FIN']}；含 RST：{counts['RST']}
- 重組後 application requests：{len(requests)}；responses：{len(responses)}
- Request command counts：{dict(sorted(commands.items()))}

## 一筆 HOSTNAME request / response

Client port {first['client_port']} → server port {port}。

Request header：`{first['header']}`

Request payload：`{first['payload_hex']}`（command=1）。

Response header：`{reply['header']}`

Response payload：`{reply['payload'].decode()}`。

`53 49 4e 46` 是 SINF；接著 version=01、type=01/02、flags=0000，
最後4 bytes 是 big-endian payload 長度，不包含12-byte header。

## TCP 與 application 邊界

SYN、SYN+ACK、ACK 建立連線；request/response 使用同一條已建立連線。
第一個查詢連線依序傳送五個 command，不能把每個 TCP packet 當成一筆 request。
此分析先依 sequence number 合併每個方向、去除重疊重傳資料，再按 application
payload length 解析 frame。10個 HOLD 連線由不同 client ports 區分。

FIN 對應正常關閉；本次另外以 SO_LINGER(1,0) 主動產生 RST，
因此 RST 是受控斷線展示，不應解讀成 server crash。
可搭配 [tcpdump.txt](tcpdump.txt) 的實際旗標與 hex dump 檢查。

## 限制

解析器針對此次短時間 IPv4/TCP capture；不處理 IPv6、IP fragmentation、
TCP sequence wraparound，也不是通用 Wireshark 替代品。
Loopback checksum/offload 觀察不能直接推論實體網路傳輸錯誤。
封包截圖仍需使用 Wireshark/tcpdump 畫面另行取得。
'''
(root/'packet-analysis.md').write_text(text)
print(text)
