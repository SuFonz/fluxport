# FluxPort

FluxPort 是一个 **UDP over TCP tunnel**：把 UDP 数据报封装进一条 TCP 长连接并多路复用，语义对标 SSH 的 `-L` / `-R` 端口转发。它包含：

- `fluxport-server`：只监听一个 TCP 控制端口，按客户端下发的隧道表动态创建 R 型 UDP 监听。
- `fluxport-client`：保存持久化隧道表，连接服务端后下发整张表，并创建 L 型本地 UDP 监听；支持 `add` / `list` / `remove` 管理配置。

控制连接断开后，服务端会自动释放该客户端创建的所有监听端口和 UDP 会话。

## 特性

- 纯 C11，Linux 优先，兼容 Windows（Winsock2 + WSAPoll）。
- 单线程 `poll` 事件循环，非阻塞 socket，可替换事件后端。
- 9 字节帧头；TCP 粘包/半包解析；帧长上限校验。
- L / R 两种隧道；按“源地址+源端口”维护 UDP 会话；多会话复用一个 TCP 连接。
- 服务端支持多客户端，每个客户端有独立隧道集合和 stream ID 空间。
- 心跳、握手超时、UDP 会话空闲回收。
- 发送队列上限 8 MiB；超限丢包计数并限频告警。
- token 常量时间比较；服务端 `--allow-ports` 限制 R 监听端口。
- 持久化配置保持注释和顺序；解析错误带行号。
- 可选的 AddressSanitizer/UBSan 构建（POSIX）。

## 构建

项目使用 **GNU Autotools**，不使用 CMake。仓库中已包含生成好的 `configure`、`Makefile.in`、`aclocal.m4` 和 `build-aux/`，普通用户不需要安装 autoconf/automake，克隆后可以直接运行 `./configure`。

`build-aux/` 不是手工维护的源码，而是生成的 Autotools 辅助脚本；当前 `configure` 会在运行时从该目录加载 `config.guess`、`config.sub`、`install-sh` 等文件，因此直接构建时不要删除它。只有把项目改成“源码仓库模式”时，才应同时删除 `configure`、`Makefile.in`、`aclocal.m4` 和 `build-aux/`，并让用户先运行 `autoreconf -fi`。

### Linux / macOS / MSYS2

```sh
./configure
make
make test        # 构建测试工具并运行集成测试
```

`make` 会在构建根目录链接可执行文件，同时复制一份到 `bin/`：

```text
bin/fluxport-server
bin/fluxport-client
```

这样可以直接运行 `./bin/fluxport-server`，而不需要先 `make install`。

安装到 `--prefix`：

```sh
./configure --prefix=/usr/local
make
sudo make install
```

只改源码、不重新生成构建系统时不需要 autotools。若修改了 `configure.ac` 或 `Makefile.am`：

```sh
autoreconf -fi
./configure
make
```

### Windows（MSYS2 UCRT64）

在 MSYS2 UCRT64 shell 中安装：

```sh
pacman -S mingw-w64-ucrt-x86_64-gcc make
```

然后同样执行 `./configure && make`。生成的可执行文件为 `fluxport-server.exe`、`fluxport-client.exe`。

### 清理

```sh
make clean       # 删除编译产物
make distclean   # 删除 Makefile/config.status 等，保留 configure
```

### Sanitizers（仅 POSIX）

```sh
./configure --enable-sanitizers
make
make test
```

Windows/MSYS2 不支持 `--enable-sanitizers`，配置阶段会明确报错。测试脚本会覆盖正常转发、异常帧、半包、错误 token、心跳失效等路径。

## 快速开始

### 1. 启动服务端

```sh
fluxport-server -l 0.0.0.0:7000 --token my-secret
```

带配置文件：

```sh
fluxport-server -c server.conf
```

`server.conf` 示例：

```ini
listen 0.0.0.0:7000
token my-secret
allow_ports 10000-20000
max_clients 64
keepalive 15
udp_idle 60
log_level info
```

### 2. 配置客户端隧道

客户端配置文件默认路径为 `~/.config/fluxport/client.conf`（遵循 `$XDG_CONFIG_HOME`；Windows 使用 `%APPDATA%\fluxport\client.conf`）。也可以用 `-c` 显式指定。

```sh
fluxport-client -c client.conf add -L 127.0.0.1:5353 8.8.8.8:53
fluxport-client -c client.conf add -R 0.0.0.0:5353 127.0.0.1:53
fluxport-client -c client.conf list
```

生成的文件类似：

```ini
server 127.0.0.1:7000
token my-secret
keepalive 15
udp_timeout 60

L 127.0.0.1:5353 8.8.8.8:53
R 0.0.0.0:5353 127.0.0.1:53
```

`udp_timeout` 与 `udp_idle` 等价，二者都表示 UDP 会话空闲超时秒数。

### 3. 运行客户端

```sh
fluxport-client -c client.conf run
# 或省略 run（默认动作）
fluxport-client -c client.conf
```

临时覆盖：

```sh
fluxport-client -c client.conf run \
  -s 192.0.2.10:7000 \
  --token my-secret \
  --strict \
  -v
```

## CLI

### fluxport-server

```text
fluxport-server -l 0.0.0.0:7000 [-c server.conf] [-v] \
                [--max-clients N] [--allow-ports 1-65535]
```

| 选项 | 说明 |
|---|---|
| `-l, --listen HOST:PORT` | TCP 控制监听地址，默认 `0.0.0.0:7000` |
| `-c, --config PATH` | 读取服务端配置文件 |
| `--token TOKEN` | 共享 token；为空表示不认证 |
| `--allow-ports POLICY` | R 监听端口策略：`all`、`none`、`5353` 或 `10000-20000` |
| `--max-clients N` | 最大并发客户端数，默认 64 |
| `--keepalive S` | 心跳间隔，0 关闭，默认 15 |
| `--udp-idle S` | UDP 会话空闲超时，默认 60 |
| `--log-level LEVEL` | `error` / `warn` / `info` / `debug` |
| `-v, --verbose` | 提升日志级别（可重复） |
| `-h, --help` | 帮助 |
| `-V, --version` | 版本 |

### fluxport-client

```text
fluxport-client -c <config> [run] [-s server:port] [-v] [--strict]
fluxport-client -c <config> add -L <laddr:lport> <rhost:rport>
fluxport-client -c <config> add -R <raddr:rport> <lhost:lport>
fluxport-client -c <config> list
fluxport-client -c <config> remove <index|匹配前缀>
```

| 选项 | 说明 |
|---|---|
| `-c, --config PATH` | 配置文件；`add/list/remove` 必须提供 |
| `-s, --server HOST:PORT` | 覆盖服务端地址 |
| `--token TOKEN` | 覆盖 token |
| `--name NAME` | 客户端名 |
| `--strict` | 任一隧道建立失败立即退出（退出码 3） |
| `--connect-timeout S` | TCP 连接超时，默认 10 |
| `--keepalive S` | 心跳间隔，0 关闭 |
| `--udp-idle S` | UDP 会话空闲超时 |
| `--log-level LEVEL` | 日志级别 |
| `-v, --verbose` | 提升日志级别 |
| `-L, --local LISTEN TARGET` | 本次运行临时添加 L 隧道；`LISTEN=TARGET` 也可 |
| `-R, --remote LISTEN TARGET` | 本次运行临时添加 R 隧道；`LISTEN=TARGET` 也可 |

`add` 会立即写回磁盘并保持原文件中的注释、空行和其他设置。`remove` 的参数：

- 纯数字：按 **1-based 下标**删除；
- 其他文本：按前缀匹配隧道行、监听地址或目标地址；匹配多条会报错并要求更精确的前缀。

## 退出码

| 码 | 含义 |
|---|---|
| 0 | 正常结束 |
| 1 | 参数或配置文件错误 |
| 2 | TCP 连接失败、认证失败、心跳/协议错误 |
| 3 | 部分隧道建立失败。`--strict` 时立即退出；非 strict 时在进程结束时体现 |

## 配置键

### 服务端

| 键 | 默认 | 说明 |
|---|---|---|
| `listen` | `0.0.0.0:7000` | 控制端口 |
| `token` | 空 | 共享 token |
| `allow_ports` | `all` | R 监听端口策略 |
| `max_clients` | 64 | 最大客户端数 |
| `keepalive` | 15 | 心跳秒数，0 关闭 |
| `udp_idle` / `udp_timeout` | 60 | UDP 会话空闲秒数 |
| `log_level` | info | 日志级别 |

### 客户端

| 键 | 默认 | 说明 |
|---|---|---|
| `server` | 无 | 服务端 `host:port` |
| `token` | 空 | 共享 token |
| `name` | `fluxport-client` | 客户端名 |
| `strict` | off | `on`/`off` |
| `connect_timeout` | 10 | TCP 连接超时秒数 |
| `keepalive` | 15 | 心跳秒数 |
| `udp_idle` / `udp_timeout` | 60 | UDP 会话空闲秒数 |
| `log_level` | info | 日志级别 |
| `L ...` / `R ...` | — | 隧道条目 |

配置文件按行解析：`#` 开头为注释，空行忽略。未知键、非法值会报告**文件路径、行号和原因**。

## 测试

```sh
make test
```

测试脚本位于 `tests/run_tests.sh`，测试工具为 `tests/fp_testtool.c`。覆盖：

1. L 型 UDP echo 回环；
2. R 型 UDP echo 回环；
3. 多源端口并发会话；
4. 空闲超时后会话回收并可重新建立；
5. 客户端退出后服务端释放 R 监听端口，重新启动客户端可再次 bind；
6. `add` / `list` / `remove` 和注释保留；
7. 错误 token、错误协议版本、超大帧长、半包断开；
8. 静默对端的心跳超时；
9. `--strict` 部分隧道失败退出码 3；
10. 非 strict 部分失败的 GOAWAY 退出码 3。

测试使用 `39010`–`39040` 区间端口；运行前请确保这些端口未被占用。

## 目录结构

```text
src/common/   协议、帧编解码、地址、网络、事件循环、配置、日志、工具
src/server/   服务端 main、连接/事件循环、隧道、UDP 会话
src/client/   客户端 main、控制连接、转发器
tests/        fp_testtool.c、run_tests.sh
docs/         protocol.md
configure.ac  Autotools 输入
Makefile.am   Autotools 输入
```

## 限制与注意事项

- **UDP 语义**：FluxPort 只负责封装和转发，不提供重传、排序或去重；底层 UDP 的丢包、乱序、重复会原样保留。
- **TCP 队头阻塞**：所有 UDP 会话共享一条 TCP 连接；一个大包或网络抖动可能影响其他会话的时延。这是 UDP over TCP 的固有限制。
- **MTU / IP 分片**：FluxPort 不主动分片。超过路径 MTU 的 UDP 数据报可能被 IP 层分片或丢弃；请让应用层控制数据报大小。
- **无 TLS**：token 和控制数据在 TCP 上明文传输；需要加密时应放在 SSH/VPN 等安全通道内，或后续增加 TLS。
- **`allow_ports` 只限制 R 型监听端口**；L 型监听发生在客户端本地。
- **单帧上限 65535 字节**，发送队列上限 8 MiB，单连接最多 256 条隧道、4096 个 UDP 会话。
- **Windows** 使用 WSAPoll；大量 socket 时性能与 Linux `poll` 类似，不适合超大规模连接。
- 服务端 token 为空时会打印警告；生产环境请配置 token，并用防火墙限制控制端口。

## 协议文档

帧格式、字段编码、状态码、stream ID 划分和会话生命周期的完整说明见 [docs/protocol.md](docs/protocol.md)。
