# FluxPort 协议规范（版本 1）

FluxPort 把多条 UDP 会话复用到一条 TCP 控制连接上。客户端连接时把整张隧道表交给服务端，服务端按表创建 R 型监听；连接断开后，双方释放该连接拥有的全部隧道、监听 socket 和 UDP 会话。

本文描述线上格式。所有多字节整数均为**网络字节序（big endian）**。

## 1. 帧格式

```
+----------------+----------+---------------------+------------------+
| payload_len(4) | type(1)  | stream_id(4)        | payload          |
+----------------+----------+---------------------+------------------+
```

- `payload_len`：payload 字节数，不含 9 字节头，最大值 **65535**。
- `type`：帧类型，见下表。
- `stream_id`：控制帧为 0；UDP 数据帧使用会话 ID。UDP 会话 ID 必须非 0。
- 接收端必须校验 `payload_len <= 65535`，并在缓冲区中处理**半包、粘包**；非法长度立即关闭连接。

### 帧类型

| type | 名称 | 方向 | payload |
|---|---|---|---|
| `0x01` | HELLO | C→S | `version(2) + token_len(2) + token + name` |
| `0x02` | HELLO_ACK | S→C | `version(2) + status(1) + msg` |
| `0x03` | TUNNEL_ADD | C→S | 隧道条目列表，见第 2 节 |
| `0x04` | TUNNEL_RESULT | S→C | 每条隧道的结果，见第 3 节 |
| `0x10` | UDP_OPEN | 双向 | 目标地址文本 `host:port` |
| `0x11` | UDP_DATA | 双向 | 原始 UDP payload |
| `0x12` | UDP_CLOSE | 双向 | 空，或 1 字节原因码 |
| `0x20` | PING | 双向 | 空 |
| `0x21` | PONG | 双向 | 空 |
| `0x30` | GOAWAY | 双向 | UTF-8 文本原因（可为空） |

## 2. HELLO / HELLO_ACK

### HELLO

```
version      u16    当前为 1
token_len    u16    0..512
token        bytes  共享 token，可为空
name         bytes  客户端名，剩余全部字节，最长 127
```

`name` 没有单独的长度字段，直接占用 payload 的剩余部分。

### HELLO_ACK

```
version      u16    服务端协议版本
status       u8     0 表示成功
msg          bytes  剩余全部字节，UTF-8 文本，最长 511
```

客户端必须检查 `version` 与 `status`；非 0 状态应打印原因并以退出码 2 结束。HELLO_ACK 的 `msg` 同样没有长度字段。

### 握手顺序

1. TCP 建连。
2. 客户端发送 HELLO。
3. 服务端校验版本和 token（**常量时间比较**），回 HELLO_ACK。
4. 成功后客户端发送 TUNNEL_ADD（即使隧道表为空也要发送 `count=0`）。
5. 服务端逐条处理并回 TUNNEL_RESULT。
6. 客户端为成功的 L 型隧道绑定本地 UDP 监听，然后进入转发循环。

服务端在握手阶段有 10 秒超时；失败时关闭 TCP 连接。

## 3. TUNNEL_ADD

```
count                    u16    0..256
repeat count:
  dir                    u8     0 = L，1 = R
  listen_addr_len        u16
  listen_addr            bytes  监听地址（不含端口）
  listen_port            u16
  target_len             u16
  target_host            bytes  目标主机（不含端口）
  target_port            u16
```

L 和 R 的编码结构完全一致，区别只在于**谁负责监听**：

- L：客户端监听 `listen_addr:listen_port`，服务端把数据发往 `target_host:target_port`；
- R：服务端监听 `listen_addr:listen_port`，客户端把数据发往 `target_host:target_port`。

`target_host` 必须是具体地址，不能是 `*`、`0.0.0.0` 或 `::`。地址文本最长 255 字节；条目数上限 256。服务端对重复条目、非法方向、非法端口、目标为通配地址等情况返回错误结果，不使连接崩溃。

## 4. TUNNEL_RESULT

服务端对 TUNNEL_ADD 中的**每一条**隧道返回一个结果，顺序可以不同，以 `index` 为准。

```
count                    u16
repeat count:
  index                  u16    对应 TUNNEL_ADD 中的 0-based 下标
  status                 u8
  msg_len                u16
  msg                    bytes  最长 511
```

客户端必须校验：

- `count` 与本地隧道数一致；
- `index` 在范围内且不重复；
- 每条结果只处理一次。

### 状态码

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | OK | 成功 |
| 1 | BAD_VERSION | 协议版本不匹配（HELLO_ACK） |
| 2 | BAD_TOKEN | token 错误（HELLO_ACK） |
| 3 | TOO_MANY_CLIENTS | 服务端连接数已满（HELLO_ACK） |
| 4 | MALFORMED | 请求格式错误 |
| 5 | PORT_DENIED | 服务端 `allow_ports` 策略拒绝 R 监听端口 |
| 6 | BIND_FAILED | 服务端绑定 R 监听端口失败 |
| 7 | BAD_TUNNEL | 隧道参数非法 |
| 8 | DUPLICATE | 同一连接内重复的隧道 |
| 9 | INTERNAL | 内部错误 |

L 型条目在服务端只做参数与目标匹配校验；客户端随后绑定 L 监听。若客户端本地绑定失败，该 L 隧道视为部分失败（本地错误码 `FP_STATUS_BIND_FAILED`），但协议上服务端可能已经返回 OK。

## 5. UDP 会话

### UDP_OPEN

payload 是目标地址文本：

- IPv4：`192.0.2.10:53`
- IPv6：`[2001:db8::1]:53`

监听侧在收到一个新的 UDP 源（源地址+源端口）时分配 `stream_id`，发送 UDP_OPEN 告知对端目标地址；随后同一 `stream_id` 上的 UDP_DATA 都属于该会话。

### stream_id 划分

- bit 31 = 0：L 型会话，由客户端分配；
- bit 31 = 1：R 型会话，由服务端分配；
- 低 31 位非 0。

监听侧分配 ID，且同一连接内必须唯一。这样两个方向的分配器不会冲突。收到非法或重复 ID 时应视为协议错误。

### UDP_DATA

payload 是**未修改的 UDP 数据报**。接收端按 `stream_id` 找到会话，并把它发给该会话对应的目标（L 型由服务端发往配置的远端目标；R 型由客户端发往配置的本地目标）。未知 `stream_id` 的数据应丢弃并计数，不应关闭整个控制连接。

单个 UDP 数据报建议不超过 65507 字节（IPv4 UDP 上限）；帧层允许 65535，但超出 UDP 实际上限的包无法发送。

### UDP_CLOSE

payload 为空或 1 字节：

| 值 | 含义 |
|---|---|
| 0 | 正常关闭 |
| 1 | 空闲超时 |
| 2 | 错误 |

空闲超时后，任一端可以发送 `UDP_CLOSE(1)` 并回收本地会话；对端收到后回收对应会话。双方同时超时可能产生重复 CLOSE，收到未知 stream 的 CLOSE 只记录 debug 日志。

### 会话生命周期

1. 监听侧收到新源地址的 UDP 包；
2. 分配 stream_id，发送 UDP_OPEN；
3. 发送 UDP_DATA；
4. 对端创建会话并发送到目标；
5. 目标回包沿同一 stream_id 返回；
6. 空闲超过 `udp_idle`（默认 60 秒）后发送 UDP_CLOSE 并回收；
7. 控制连接断开时，所有会话立即回收，不发送逐条 CLOSE。

## 6. 心跳与超时

- 默认每 15 秒发送 PING；
- 30 秒（2 × keepalive）未收到任何帧，判定连接失效；
- `keepalive 0` 可关闭心跳；
- 握手超时 10 秒；
- UDP 会话默认空闲超时 60 秒；
- 所有时间参数可通过配置或 CLI 覆盖。

PING/PONG 的 `stream_id` 必须为 0。收到 PING 应立即回 PONG；PONG 本身不需要回复。

## 7. GOAWAY 与关闭

任一端可以发送 GOAWAY 表示准备关闭。收到 GOAWAY 后：

- 停止创建新会话；
- 关闭本地监听和所有 UDP 会话；
- 关闭 TCP 连接；
- 正常关闭的客户端退出码为 0（存在部分隧道失败时按退出码规则处理）。

TCP FIN、读写错误、心跳超时和协议错误都会触发同样的清理流程。服务端必须释放该连接创建的 R 型监听端口，使后续客户端或进程可以重新 bind。

## 8. 背压与错误处理

- 每条控制连接维护发送队列，上限 8 MiB。
- 队列满时，普通 UDP_DATA 可以丢弃并计数/限频告警；关键控制帧（HELLO、TUNNEL_ADD、PONG 等）失败则关闭连接。
- 帧长度超过 65535 时立即关闭连接。
- 解析到一半对端断开：丢弃半包并关闭连接，不崩溃。
- 未知帧类型、非法 stream_id、握手顺序错误：协议错误，关闭连接。
- 服务端对单连接和全局 UDP 会话数设上限（4096），超限时拒绝新会话并记录日志。

## 9. 安全说明

- token 使用常量时间比较。
- token 在 TCP 上明文传输；本版本不提供 TLS。
- 服务端通过 `allow_ports` 限制 R 型监听端口，防止客户端要求服务端绑定任意敏感端口。该策略**只作用于 R 型监听端口**，L 型监听发生在客户端本地。
- 单帧、发送队列、隧道数、会话数均有上限。

## 10. 版本与兼容性

当前协议版本为 `1`。不兼容的修改必须提升 `FP_PROTO_VERSION`；客户端收到不同版本的 HELLO_ACK 应拒绝连接。
