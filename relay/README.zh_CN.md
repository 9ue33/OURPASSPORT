<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# AI PASSPORT 中继服务器

这是一个面向一对 ESP32-C3 **AI PASSPORT** 掌上设备的小巧、零依赖互联网中继。两台设备先在中继上注册，再用一次性短码彼此配对，之后便一直通过中继交换「拥抱」与心跳事件。中继还会告诉每台设备其伴侣当前是否在线、此刻是否正握着他们的护照——正是这一点让跨越互联网的拥抱显得即时。

全部实现都在单个 Python 文件（`relay.py`）中，且只使用标准库：无需 `pip install`，无需数据库，无需守护进程框架。请求是携带紧凑二进制 "HL" 帧的 HTTP `POST`，响应同样是二进制帧。只有一个纯文本健康检查端点。

```
relay/
├── relay.py        the server
├── test_relay.py   end-to-end test suite (41 tests, stdlib unittest)
├── deploy/         systemd unit and Cloudflare Tunnel config templates
└── README.md       this file
```

## 环境要求与运行方式

- Python 3.9 或更高版本，仅使用标准库。

```sh
cd relay
python3 relay.py --host 0.0.0.0 --port 8787 --data-file relay-state.json
```

启动横幅会打印到 stdout，同时也会记入日志：

```
AI PASSPORT relay AIPASSPORT-Relay/1.0 listening on http://0.0.0.0:8787 (data file: relay-state.json)
```

选项：

| 参数 | 默认值 | 含义 |
| --- | --- | --- |
| `--host` | `0.0.0.0` | 绑定地址 |
| `--port` | `8787` | 绑定端口 |
| `--data-file` | `relay.py` 同目录下的 `relay-state.json` | 状态文件路径 |
| `--log-level` | `INFO` | `DEBUG`、`INFO`、`WARNING`、`ERROR`、`CRITICAL` |

如果端口已被占用，中继会记录 `cannot bind <host>:<port>` 并以状态码 1 退出，而不是崩溃。

### 停止它

在控制台中按 **Ctrl+C**（SIGINT）或 **Ctrl+Break**（SIGBREAK）。中继会走完它的关闭流程——停止接收请求、把状态刷入文件、记录 `relay stopped cleanly`——然后以状态码 0 退出。

> **Windows 注意事项：** 在 Windows 上，`os.kill(pid, signal.SIGTERM)` 和 `taskkill /F` 属于强制终止。它们会直接结束进程而不走关闭流程，因此最后几秒的状态可能不会被写入。请使用 Ctrl+C 或 Ctrl+Break 来干净地停止。（SIGTERM 处理在 POSIX 主机以及能够投递该信号的终端上仍然注册。）

## 部署到服务器上

中继是单个无依赖文件，所以所谓「部署」就是：把 `relay.py` 放到两台设备都能访问到的主机上，并让它持续运行。

```sh
sudo mkdir -p /opt/aipassport-relay
sudo cp relay.py /opt/aipassport-relay/
sudo cp deploy/aipassport-relay.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now aipassport-relay
journalctl -u aipassport-relay -f
```

`deploy/aipassport-relay.service` 绑定 `0.0.0.0:8787`，并把状态保存在 `/opt/aipassport-relay/relay-state.json`。如果你的目录布局不同，请修改 unit 中的这两个路径。对于面向公网的中继，建议采用下面的隧道方案，并改为绑定 `--host 127.0.0.1`，这样除了隧道之外没有别的来源能访问该端口。

从任意位置验证：

```sh
curl -s http://<your-host>:8787/healthz    # -> ok
```

然后把设备指向它：要么用配网页面（`Heartlink-XXXX` SoftAP → `http://192.168.4.1/`），要么用 `../main/couple_config.h` 中的编译期默认值 `COUPLE_DEFAULT_RELAY_URL`。地址必须是纯 `http://`——固件没有 TLS 栈。

### 可选：用 Cloudflare Tunnel 隐藏源站

设备不会讲 TLS，所以直接暴露中继端口就意味着在公开互联网上传输明文帧。Cloudflare Tunnel 可以在不碰固件的前提下避免这一点：Cloudflare 为你的主机名终结 TLS，而中继只会看到一条回环连接。

`deploy/cloudflared-config.yml.example` 是一份填好的模板；操作步骤写在它的头部注释里。有两处一旦弄错就会静默破坏配对：

- 主机名的 **Always Use HTTPS** 开关必须**关闭**。301 重定向不是设备能跟随的东西，于是每次 sync 都会失败。
- 即使使用者是通过 `https://` 访问该主机名，固件里的 scheme 也要保持 `http://`——这次转换发生在 Cloudflare 侧，而不是设备上。

### 健康检查

```sh
curl -s -i http://127.0.0.1:8787/healthz
```

```
HTTP/1.1 200 OK
Server: AIPASSPORT-Relay/1.0
Content-Type: text/plain; charset=utf-8
Content-Length: 2

ok
```

任何其他路径——包括对 `/v1/*` 路由的 `GET`——都会得到 `404`，响应体为 `not found`。`GET` 和 `POST` 以外的方法会得到标准库的 `501 Unsupported method`。

## 协议概览

每个 `/v1/*` 端点都以一个二进制帧作为 HTTP 请求体，并以一个二进制帧作为 HTTP 响应体。纯文本只用于 `/healthz` 和那个 `404`。

### 帧结构

```
offset  size  field        notes
0       2     magic        always ASCII "HL"
2       1     version      always 1
3       1     type         frame type, see the table below
4       2     payload_len  big-endian u16, 0..512
6       4     seq          big-endian u32, sender's frame counter
10      4     ack          big-endian u32, highest seq the sender accepted
14      N     payload      payload_len bytes
14+N    16    hmac         first 16 bytes of HMAC-SHA256 over bytes 0..14+N
```

16 字节的 HMAC 标签**仅**出现在已签名的类型 `0x07`、`0x08`、`0x09`、`0x0A`（sync 与 unpair）上。未签名的配对类型从不携带它。HMAC 密钥是 claim 时下发的 32 字节 pair key。

帧类型：

| 类型 | 名称 | 方向 |
| --- | --- | --- |
| `0x01` | `PAIR_BEGIN_REQ` | 设备 → 中继 |
| `0x02` | `PAIR_BEGIN_RSP` | 中继 → 设备 |
| `0x03` | `PAIR_CLAIM_REQ` | 设备 → 中继 |
| `0x04` | `PAIR_CLAIM_RSP` | 中继 → 设备 |
| `0x05` | `PAIR_AWAIT_REQ` | 设备 → 中继 |
| `0x06` | `PAIR_AWAIT_RSP` | 中继 → 设备 |
| `0x07` | `SYNC_REQ` | 设备 → 中继（已签名） |
| `0x08` | `SYNC_RSP` | 中继 → 设备（已签名） |
| `0x09` | `UNPAIR_REQ` | 设备 → 中继（已签名） |
| `0x0A` | `UNPAIR_RSP` | 中继 → 设备（已签名） |

校验是严格的，任何违规都以状态 `BAD_REQUEST` 报告：

- 请求体短于 14 字节、magic 不对，或版本不是 1；
- 未知的帧类型，或类型与端点不匹配；
- `payload_len > 512`，或请求体长度不等于 `14 + payload_len`（已签名类型再加 16）；
- 已签名帧的 HMAC 不匹配，或已签名帧没有密钥（报告为 `UNAUTHORIZED`）；
- sync 的 `seq` 为 0（序列号从 1 开始）。

过大的请求体（超过 4096 字节）会先被读出并丢弃，这样客户端仍能收到错误帧，而不是连接被重置。

### 端点与必需头部

| 端点 | 请求类型 | 头部（均为十六进制，名称不区分大小写） |
| --- | --- | --- |
| `POST /v1/pair/begin` | `0x01` | `X-Device-Id` — 16 字节 |
| `POST /v1/pair/claim` | `0x03` | `X-Device-Id` — 16 字节 |
| `POST /v1/pair/await` | `0x05` | `X-Device-Id` — 16 字节，`X-Pair-Token` — 32 字节 |
| `POST /v1/sync` | `0x07` | `X-Device-Id` — 16 字节，`X-Pair-Id` — 8 字节 |
| `POST /v1/unpair` | `0x09` | `X-Device-Id` — 16 字节，`X-Pair-Id` — 8 字节 |

头部缺失、格式错误或长度不对都会是 `BAD_REQUEST`。`X-Device-Id` 是设备自己的随机 16 字节身份；它是配对端点唯一依赖的凭据，因此泄露了 id 的设备在配网阶段可能被冒充。`X-Pair-Token` 是 `pair/begin` 返回的 32 字节密钥，用来证明 `pair/await` 的调用者正是发起配对的那台设备。

### 载荷

`PAIR_BEGIN_REQ`（请求体）：`u8 name_len` + UTF-8 名称（最多 64 字节）。
`PAIR_BEGIN_RSP`：`u8 status` + 6 字节 code + `u16` ttl 秒数 + 32 字节 token（共 41 字节）。出错时它保持同样的 41 字节结构，code 字段填 `------`，TTL 为 0，token 为 32 个零字节。

`PAIR_CLAIM_REQ`：6 字节 ASCII code + `u8 name_len` + UTF-8 名称。查询前会先把 code 转成大写。
`PAIR_CLAIM_RSP` 成功时：`u8 status` + 8 字节 pair id + 32 字节 pair key + 16 字节伴侣设备 id + `u8 name_len` + **正在等待**的那台设备的名称。出错时它只是一个裸的 status 字节。

`PAIR_AWAIT_REQ` 的请求体为空——身份来自 `X-Pair-Token` 头部。
`PAIR_AWAIT_RSP`：`u8 status` + `u8 state`。state 为 `0`（仍在等待，请重试）、`1`（已配对——后面跟着 pair id + key + 伴侣 id + 伴侣名称，与成功的 claim 响应完全一致）或 `2`（code 已过期）。错误时是一个裸的 status 字节。

`SYNC_REQ`：`u8 count` + `count` ×（`u8 type`、`u8 len`、`len` 字节取值）+ `u8 local_flags`。每个请求最多 12 个事件。
`SYNC_RSP` 成功时：`u8 status` + `u8 count` + 那些事件 + `u32 server_seq` + `u32 server_time`（Unix 秒）+ `u8 partner_online` + `u8 partner_holding` + `u8 pair_state`。出错时它保持这个 13 字节的结构，事件列表为空、各标志位为零。

`UNPAIR_REQ` 的请求体为空。`UNPAIR_RSP` 是一个裸的 status 字节。

每个响应帧都携带 `seq` = 中继的全局服务器计数器，以及 `ack` = 发起请求设备的 `last_accepted`（未签名端点为 0）。错误响应的 HTTP 状态码是 **200**，失败信息在帧内部报告，所以设备始终只需解析一种统一的响应结构。当 pair id 能解析到一对有效的 pair 时，已签名的错误响应会用 pair key 做 HMAC（这样设备可以验证它）；解析不到时用 32 个零字节。

### 状态码

| 代码 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `OK` | 成功 |
| 1 | `BAD_REQUEST` | 帧、头部或载荷格式错误 |
| 2 | `CODE_NOT_FOUND` | 配对码未知 |
| 3 | `CODE_EXPIRED` | 配对码已知但已超过它的 TTL |
| 4 | `UNAUTHORIZED` | HMAC 错误、token 错误，或不属于该 pair |
| 5 | `ALREADY_PAIRED` | 设备已处于一对有效的 pair 中（或该 code 已被使用） |
| 6 | `SERVER_BUSY` | 无法分配出配对码 |
| 7 | `PAIR_NOT_FOUND` | 未知的 `X-Pair-Id`，或非成员访问一对已解散的 pair |

## 配对

1. **设备 A** 带上自己的名称调用 `POST /v1/pair/begin`。中继返回一个 6 位数字码（字符集为 `0123456789`，因为设备的配对界面是个数字拨轮）、300 秒的 TTL 和一个 32 字节 token。一台设备同一时间只能持有一个待用码；再次 begin 会替换掉旧的。已经处于有效 pair 中的设备会得到 `ALREADY_PAIRED`。
2. **设备 A** 带上 token 长轮询 `POST /v1/pair/await`。它会阻塞最多 25 秒，若什么事都没发生就回答 `state=0`，所以设备只需再次调用它。token 未知或不匹配则是 `UNAUTHORIZED`。
3. **设备 B** 带上 6 位数字码和自己的名称调用 `POST /v1/pair/claim`。成功后双方都会获知 8 字节 pair id 和 32 字节 pair key。设备 A 挂起的 `await` 会带着 B 的身份返回 `state=1`。
4. 对一个仍缓存着该码的客户端，已过期的码返回 `CODE_EXPIRED`；而在中继重启之后则返回 `CODE_NOT_FOUND`（见下文 *状态文件*）。

尝试 claim 你自己的码是 `BAD_REQUEST`；claim 一个已经被用过的码，或在自己已经配对时去 claim，都是 `ALREADY_PAIRED`。一台设备最多只能处于一对有效的 pair 中；开始新的配对会遗忘此前已解散的任何 pair。

错误的码是 `CODE_NOT_FOUND`。因为数字只留下 10^6 种码，中继会统计错误的 claim：达到 8 次（`MAX_CODE_ATTEMPTS`）后待用条目会被删除——这个码被烧掉——此后它以及之后每一次 claim 都持续得到 `CODE_NOT_FOUND`。格式错误的 claim、`BAD_REQUEST`（claim 你自己的码）以及 `ALREADY_PAIRED` / `CODE_EXPIRED` 结果都不消耗尝试次数。一次 claim 不携带目标 id，所以中继无法判断一次错误猜测是针对哪个待用会话的：每次错误 claim 都会计入每一个仍然有效的待用会话。

## 同步

`POST /v1/sync` 是稳态调用。两台设备都轮询它；一台设备同一时间应保持只有一个 sync 请求在途。

**客户端事件**（在 `SYNC_REQ` 中发送）：

| 类型 | 名称 | 取值 |
| --- | --- | --- |
| `0x01` | `MISS` | 无——「我想你」 |
| `0x02` | `HUG_START` | 无——拥抱开始（tidings/grip closed） |
| `0x03` | `HUG_END` | 2 字节——拥抱时长，单位为毫秒 |

**服务器事件**（在 `SYNC_RSP` 中投递）：

| 类型 | 名称 | 取值 |
| --- | --- | --- |
| `0x04` | `HUG_SYNC` | 无——两本护照在同一时刻都被握持 |
| `0x05` | `HEART_SYNC` | 无——两台设备在心跳窗口内互相想念 |
| `0x06` | `UNPAIR` | 无——伴侣离开了该 pair |
| `0x07` | `PAIRED` | 无 |
| `0x08` | `OFFLINE_SUMMARY` | 1 字节——你不在的时候发生了多少个错过的事件 |

客户端产生的事件会被转发给伴侣；设备不得发送服务器生成的类型 `0x04`–`0x08`。

**响应字段。**

- `partner_online` 在最近 45 秒内见过伴侣时为 1，否则为 0。
- `partner_holding` 在伴侣正握着他们的护照时为 1。
- `pair_state` 为 `0`（未配对）、`1`（已配对）或 `2`（伴侣已离开）。

**握持（Holding）。** `local_flags` 的第 0 位是设备的*实时*握持状态，并且是权威值：按住按键时为 1，一旦松开为 0，它会覆盖同一批事件推导出的状态——除非**同一批**中出现 `HUG_START`，那始终表示正在握持。当两台设备都在握持且该 pair 尚未锁定（latched）时，中继向双方各广播一个 `HUG_SYNC`；任一方松手后锁存立即清除，因此一次拥抱恰好产生一个 `HUG_SYNC`。

**心跳（Heartbeat）。** 当一台设备发送 `MISS` 时，中继记下时间。如果伴侣也在 10 秒窗口内发送了 `MISS`，两个想念标记都会被清除，并向两台设备广播一个 `HEART_SYNC`。

**离线标记（Offline tagging）。** 每个入队的事件都会被打上标记，记录它产生的那一刻接收方是否看起来离线。当接收方下次 sync 且其收件箱中含有任何带离线标记的事件时，中继会先插入一个 `OFFLINE_SUMMARY` 事件，携带离线 `MISS` 事件的数量，然后按顺序投递其余全部内容。该标记能挺过重启，所以一台曾经离线的设备仍能得知在它离开期间发生过事情。

**长轮询与限制。** 一次 sync 请求会阻塞，直到调用方有东西可投递，或 25 秒过去，然后带着空事件列表返回。每个响应最多返回 12 个事件，其余留在队列里等下一次调用。每台设备的收件箱最多容纳 64 个事件——若溢出，最旧的会被丢弃，中继会记录一条 warning。

**重传（Retransmission）。** `seq` 小于或等于该设备 `last_accepted` 的 `SYNC_REQ` 被视为重传：它的事件*不会*被再次摄入，但设备仍会被标记为已见并收到一个正常响应。这使得响应丢失后的重试是安全的。

## 解除配对

`POST /v1/unpair`（已签名，带 `X-Pair-Id`）把调用者从这个 pair 中移除，并立即为伴侣排队 `UNPAIR` 以及 `pair_state=2`。伴侣可以继续 sync 那个 pair id，直到它自己也 unpair——它会收到挂起的 `UNPAIR` 事件而不是错误，而来自它的任何后续 sync 都返回 `partner_online=0`。中继会保留这条已解散的记录，直到最后一个成员开始新的配对，所以被孤儿化的一方总有机会得知发生了什么。

## 状态文件

`--data-file`（默认为 `relay.py` 同目录下的 `relay-state.json`）以 JSON 保存整个中继状态：设备（最后可见时间、`last_accepted`、holding、带离线标记的收件箱）、pair（id、key、成员、名称、状态、latch）、设备 → pair 索引，以及待用的配对会话（code、token、过期时间、错误 claim 次数）。它采用原子写入（临时文件 + `os.replace`），在脏状态下去抖动到每秒最多一次，并在干净关闭时总是再写一次。文件不存在属于正常的首次启动；文件无法读取或格式错误会记为一条 warning，中继以空状态启动，而不是拒绝启动。过期的待用会话不会跨重启保留。

> **安全提示：** 状态文件以明文保存 pair key 和待用配对 token。任何能读到它的人都可以为那些 pair 伪造已签名帧，或劫持一次待用配对。请把它当作机密对待。

由于 code 索引是从状态文件重建的，重启后一个已经过期的配对码会报告为 `CODE_NOT_FOUND`，即使设备仍缓存着该码——「已过期」这个区分只在签发该码的中继进程仍在运行时才存在。

## 测试

```sh
cd relay
python3 test_relay.py
```

`test_relay.py` 会在一个临时端口上用临时状态文件启动一个真实的中继，并通过真实 HTTP socket 以手工构造的帧驱动它。它覆盖配对的正常路径及其各种失败模式、sync 拒绝、重传、事件投递、`HEART_SYNC`、离线摘要、拥抱重叠、unpair、重启后的持久化，以及帧结构校验——总计 41 个测试。由于 sync 会长时间轮询，整个测试套件大约需要 40 秒。

## 已知限制

- **没有 TLS。** 帧走明文 HTTP；如果设备与中继之间的链路不可信，请把它跑在终结 TLS 的代理之后。
- **配网认证薄弱。** 配对只由 6 位数字码加 32 字节 token，以及头部中的设备 id 来保护。没有账号、没有签名，也没有通用的限流：唯一的限制是待用码在 8 次错误 claim 后失效，所以远程攻击者仍可随意猛打这些端点——并烧掉仍然有效的待用码。
- **每台设备只能有一个 pair。** 一台设备不能同时属于两个 pair。
- **收件箱在内存中。** 排队的事件存在于 RAM 中且上限为 64；一段很长的离线会丢掉最旧的那些（`OFFLINE_SUMMARY` 的计数仍会告诉设备它错过了东西）。
- **每个请求一个线程。** `ThreadingHTTPServer` 加上阻塞式长轮询意味着每个在途 sync 占用一个线程；对少量设备没问题，对一支设备队伍则不行。
- **未做浸泡测试（soak testing）。** 持续负载、时钟变化，或状态文件所在磁盘故障时的行为尚未测量。