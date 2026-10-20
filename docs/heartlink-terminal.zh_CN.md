<p align="right">
  <strong>简体中文</strong> · <a href="heartlink-terminal.md">English</a>
</p>

# Heartlink 情侣心动终端

本文说明替换上游 Demo 菜单、位于 `main/` 的 Heartlink 情侣终端应用。内容包括：双板系统做什么、代码如何分层、设备如何与 relay 通信、闪存上存了什么、如何构建与烧录，以及哪些部分是已知未完成的。

## 1. 它是什么

Heartlink 把两块 AI Passport 板子变成一台常驻配对的情侣设备。每块板子只通过 HTTP 与一台 relay 服务器通信，没有蓝牙或点对点 Wi-Fi 模式。短按 `OK` 会把「想你」发给对方板子，对方以全屏心形动画和一声短音回应。按住 `OK` 是远程抱抱：在本机按住的时长决定对方板子上抱抱动画的时长。两块板子同时按住时，relay 判定 `HUG CONNECTED`，两块板子一起播放同步心跳。双方在同一个 10 秒窗口内各发一次「想你」时，relay 判定 `HEART SYNC`，两块板子播放全屏特别动画。每次互动都会累加到持久的 Love Energy 总量，互动总数达到 9 / 18 / 99 / 100 / 365 / 520 / 999 / 1314 时触发隐藏彩蛋，每日面板显示今天的想你次数、抱抱次数、当天首次互动时间和连续互动天数。

```text
                    互联网
   设备 A  <-------- relay -------- >  设备 B
  (ESP32-C3)     不使用 HTTPS，走 HTTP  (ESP32-C3)
     板子              长轮询              板子
```

relay 是两块板子之间唯一的通路。设备以最长约 25 秒的 HTTP 长轮询访问它（`COUPLE_SYNC_LONGPOLL_MS`），不使用 WebSocket 或 MQTT。对方离线时互动不会丢失：relay 把它们排队，并在对方恢复在线后交给它一条 `OFFLINE_SUMMARY` 事件，其中带有离线期间收到了多少次「想你」。

固件版本号为 `1.0.0`（`COUPLE_FW_VERSION`，`main/couple_config.h:10`）。

## 2. 你需要什么

- 两块 AI Passport 板子（ESP32-C3，8 MB Flash，无 PSRAM，ESP-IDF 5.5.3）。
- 一台两块板子都能通过网络访问的 relay 服务器。relay 服务端就在本仓库的 `relay/` 目录里，服务端契约的权威文档是 [relay/README.zh_CN.md](../relay/README.zh_CN.md)。本固件所依赖的设备侧概述见第 5 节。
- 板子要接入的 Wi-Fi 凭据，在每块板子上通过配网页填写（见第 10 节）。
- 一台已激活 ESP-IDF 5.5.3 环境的宿主机器用于构建固件。

编译期默认 relay 地址是占位符 `http://relay.example.com`（`COUPLE_DEFAULT_RELAY_URL`，`main/couple_config.h:15`），因此你必须把每块板子指向自己的 relay 主机。也可以在配网页运行期设置：该页把地址存进 NVS，运行期覆盖编译期默认值，因此改地址不必重新构建。地址必须以 `http://` 开头——固件里没有 TLS 栈。

## 3. 架构

应用按关注点分层，每一层只由一个文件拥有，且任何慢操作都不会出现在会拖住渲染或输入的位置。

| 层 | 归属文件 | 职责 |
| --- | --- | --- |
| 入口与输入 | `main/main.c` | 启动 I2C、显示、LVGL、背光、按键驱动，并创建一个独立的输入任务。 |
| 编排 | `main/couple_app.c`、`main/couple_app.h` | 唯一「知道全部模块」的层，也是它们的唯一所有者。把按键事件翻译成互动，运行节拍定时器，持有配对与配网流程，并负责 Love Energy 与里程碑的记账。 |
| 关系状态 | `main/couple_state.c`、`main/couple_state.h` | 纯 C 状态机：能量与互动计数、日界切换、连续天数、里程碑判定，以及本地文案格式化。不依赖 ESP-IDF，也不依赖 LVGL，可在宿主机上测试。 |
| 线协议 | `main/couple_proto.c`、`main/couple_proto.h` | 帧的编解码与字段布局。纯 C，有宿主测试。 |
| 密码学 | `main/couple_sha256.c`、`main/couple_sha256.h` | 用于帧认证的 SHA-256 与 HMAC-SHA256。纯 C，有宿主测试。 |
| 网络 | `main/couple_net.c`、`main/couple_net.h` | 唯一的 relay 客户端：同步循环、长轮询、重试与退避、离线待发队列、配对与解绑请求，以及时钟。只有一个 worker 任务，零 LVGL 依赖。 |
| Wi-Fi | `main/couple_wifi.c`、`main/couple_wifi.h` | Wi-Fi 驱动封装：站点关联、自实现的重新连接退避、AP 客户端计数。 |
| 配网 | `main/couple_prov.c`、`main/couple_prov.h` | SoftAP 加上给手机浏览器看的 HTTP 配置页。 |
| radio 准备 | `main/couple_radio.c`、`main/couple_radio.h` | 幂等的 NVS 与网络协议栈准备步骤，在 Wi-Fi 与持久化之前共享。 |
| 持久化 | `main/couple_store.c`、`main/couple_store.h`、`main/couple_store_blob.c`、`main/couple_store_blob.h` | 存了什么、存在哪些 NVS 键下，以及批量与延迟写入策略。 |
| 音频 | `main/couple_audio.c`、`main/couple_audio.h` | BSP 音频通路的唯一所有者：格式、音量与 PCM 写入都在它自己的 worker 任务里。 |
| 音效合成 | `main/couple_tone.c`、`main/couple_tone.h` | 所有音效都在软件里生成；固件不附带任何音频文件。 |
| 界面 | `main/couple_ui.c`、`main/couple_ui.h` | LVGL 页面。纯被动渲染器：从不读按键、网络、NVS 或音频。 |
| 界面文案 | `main/couple_ui_strings.h` | 全部用户可见字符串，含中文分支与纯 ASCII 英文回退分支。 |
| 可调参数 | `main/couple_config.h` | 全部编译期常量，并注明哪些可在运行期被覆盖。 |
| 共享类型 | `main/couple_types.h` | 帧类型、事件类型、状态码、抱抱档位与协议上限，供其他模块共享。 |
| 字体别名 | `main/couple_fonts.h` | 把 small/body/title/number 四个角色映射到子集字体或 Montserrat 回退。 |

纯 C 模块的宿主测试：

| 测试 | 覆盖内容 |
| --- | --- |
| `tests/test_couple_sha256.c` | SHA-256 与 HMAC 测试向量。 |
| `tests/test_couple_proto.c` | 帧的编解码与校验。 |
| `tests/test_couple_state.c` | 能量、互动计数、日界切换、连续天数、里程碑。 |
| `tests/test_couple_store_blob.c` | 持久化 blob 的编解码与 schema 兼容性。 |
| `tests/test_couple_tone.c` | 音效合成。 |

字体流程使用的工具：

| 工具 | 作用 |
| --- | --- |
| `tools/ui_codepoints.py` | 从 `main/` 的字符串字面量里抽出真正用到的码点并刷新清单。 |
| `tools/check_font_glyphs.py` | 证明生成字体覆盖了指定区间；门禁对两个字号都跑它。 |

承担关键约束的是这条规则：任何慢操作都不跑在 LVGL 或按键回调里。按键驱动的回调跑在共享的 `esp_timer` 任务上，因此 `main/main.c:83` 的 `on_key` 只投递一条输入事件就返回；独立的 `couple_input` 任务排空该队列并调用 `couple_app_handle_key()`。这个任务本身也只往应用任务的输入队列投递；`couple_app_handle_key()` 从不阻塞，队列满时丢弃该按键并计数。音频、存储与网络都跑在各自的任务上，对外只暴露非阻塞入口。

## 4. 互动模型

设备识别以下互动。计分规则只在一处定义，即 `main/couple_state.h:9` 的头注释，下面的数字都来自它。

- **想你**。首页短按 `OK` 发出想你信号，发送方 Love Energy +1，接收方 +1。
- **远程抱抱**。按住 `OK` 进入抱抱页并告诉 relay 本机正在按住。松手时按住时长决定档位：700 ms 起为小抱抱，3000 ms 起为大抱抱，8000 ms 起为不撒手抱抱（`COUPLE_HUG_TIER_SMALL_MS`/`BIG_MS`/`CLINGY_MS`，`main/couple_config.h:53-55`）。上报时长上限为 60000 ms（`COUPLE_HUG_MAX_MS`，`main/couple_config.h:56`）。对方板子播放对应时长的抱抱。
- **心有灵犀**。双方在同一个 10 秒窗口内各发一次想你（`COUPLE_MISS_WINDOW_MS`，`main/couple_config.h:52`）时，relay 判定 `HEART SYNC`，两块板子播放全屏动画，Love Energy +5。
- **双向抱抱**。两块板子同时按住时，relay 判定 `HUG CONNECTED`，两块板子播放同步心跳，Love Energy +5。
- **离线汇总**。对方恢复在线时，relay 发来一条 `OFFLINE_SUMMARY` 事件，带有它离线期间收到的想你次数，本板把该数字显示出来。互动不会丢失。
- **Love Energy**。一个持久的成长总量。奖励为：发出想你 +1、收到想你 +1、心有灵犀 +5、双向抱抱 +5、续上连续互动 +3（`main/couple_config.h:73-77`）。
- **里程碑**。互动总数跨越 9、18、99、100、365、520、999、1314 时触发一次性彩蛋。每个里程碑只发放一次，且加成数值等于阈值本身（`couple_milestone_energy_bonus()`，`main/couple_state.c:248`）。520 与 1314 使用刻意更华丽的动画（`couple_milestone_is_romantic()`，`main/couple_state.c:261`）。
- **每日面板**。显示今天的想你次数、抱抱次数、当天首次互动时间和连续互动天数。两个计数都按双向统计，因此两块板子显示同一个数字。

计数与压力是刻意设计的。互动总数只统计真实的想你与抱抱：任一方向的想你 +1，达到至少小抱抱档位的抱抱 +1。没到档位的误触根本不算互动——连当日首次互动与连续天数都不记。心有灵犀与双向抱抱只加能量、不重复计入互动数，因为它们本来就是同一次互动的确认。

状态头文件里有条明确的设计红线：不施加压力。日界刷新只清空当日计数，别的什么都不做。断联、隔天、连续天数归零都不产生任何责备性文案，也没有惩罚。

## 5. 网络

设备只持有一个 worker 任务，对 relay 跑同步循环。

**设备与 relay 的契约。** 帧格式是 14 字节头加 16 字节截断 HMAC（`main/couple_proto.h:11`）：魔术字 `H` `L`、版本字节、帧类型、大端 16 位 payload 长度、大端 32 位单调递增 `seq`、大端 32 位 `ack`，之后是 payload。标签是用共享密钥对头与 payload 做 HMAC-SHA256 后的前 16 字节。身份走 HTTP 头而不进帧：所有请求带 `X-Device-Id`，SYNC/UNPAIR 带 `X-Pair-Id`，PAIR_AWAIT 带 `X-Pair-Token`（`main/couple_proto.h:31-33`）。端点是 `POST /v1/pair/begin`、`POST /v1/pair/claim`、`POST /v1/pair/await`、`POST /v1/sync`、`POST /v1/unpair`（`main/couple_proto.h:35-39`）。状态码 0 到 7 对应 `main/couple_types.h:44-51` 里的各条件。事件类型为：客户端发起 0x01 想你、0x02 抱抱开始、0x03 抱抱结束（2 字节毫秒值）；服务端发起 0x04 双向抱抱、0x05 心有灵犀、0x06 解绑、0x07 已配对、0x08 离线汇总（1 字节计数）（`main/couple_types.h:66-74`）。传输层上设备只支持 `http://`；relay 地址写成 `https://` 时不做 TLS，会按网络故障一直重试（`main/couple_net.h:52`）。

**同步循环与长轮询。** 每一轮 `/v1/sync` 先把待发批次发出去，再等响应；relay 会把请求挂住最长约 25 秒，因此刚入队的事件最坏要等约 25 秒才离开设备（`main/couple_net.h:39`）。成功后循环把重试间隔重置为 2000 ms，并在 outbox 里还有超过一轮（每轮 12 条）的事件时立刻再发；只有失败才退避（`main/couple_net.c:1191`）。

**重试与退避。** 失败时使用指数退避，上限 60000 ms，并叠加最多 25 % 抖动（`net_backoff()`，`main/couple_net.c:459`；基线与上限见 `main/couple_config.h:27-28`）。Wi-Fi 关联重连另有退避，基线与上限相同（`main/couple_config.h:31-32`）。

**离线待发队列。** 待发事件进入一个固定 32 条的队列（`COUPLE_OUTBOX_CAPACITY`，`main/couple_config.h:58`），入队绝不阻塞。队列满时丢弃最旧的一条并记警告，因为丢最新的会让人觉得「按了没反应」，而丢最旧的只是补发不完整（`main/couple_net.h:44`）。

**同步计数器的持久化。** `seq` 只在内存里前进；每 25 秒一次长轮询若每次都写盘，一天就是几千次擦写（`main/couple_config.h:38-46`）。加载时把计数器抬到「已落盘值 + `COUPLE_SEQ_BOOT_MARGIN`（1024）」，并立刻落盘一次；运行期每前进 `COUPLE_SEQ_PERSIST_STEP`（128）才标一次脏，由存储节拍顺带写入。不变的约束是持久化步进必须明显小于启动余量，这样异常掉电后重启的起点不会低于本次会话已用过的值，否则对端会按重放丢弃。

**在线状态的过期时间。** relay 在静默 45 秒后把对方视为离线（`COUPLE_PRESENCE_STALE_SEC`，`main/couple_config.h:34`）。设备每 300 秒与服务端重新对时一次（`COUPLE_TIME_SYNC_INTERVAL_SEC`，`main/couple_config.h:35`）。

**配对阶段的安全边界。** 配对时两端还没有共享密钥，因此配对握手帧（0x01-0x06）不带 HMAC，只做长度、魔数与类型校验。含义是：猜到 6 位配对码的人、或链路上的中间人，有机会抢先建立这段配对。配对一旦完成，之后所有帧都做 HMAC 认证，上述窗口即关闭。威胁模型是「情侣自用的两块小设备 + 自己掌握的 relay」，因此这是已记录并接受的取舍，而不是疏漏；要收紧的话，正确做法是给配对再叠一条带外信道，而不是给这些帧加签名（`main/couple_net.h:217-227`，`main/couple_proto.h:23-25`）。一个相关细节：relay 在找不到 live pair 时用 32 个零字节给错误响应签名，真实设备必然验签失败，因此客户端只把未认证的状态写日志，绝不据它改状态（`main/couple_net.c`）。

relay 自身的行为——配对码有效期、四步配对流程、sync 语义、在线与按住状态的上报、以及健康检查端点——由 relay 服务端的 README [relay/README.zh_CN.md](../relay/README.zh_CN.md) 记录。该文件就在本仓库的 `relay/` 目录里，是服务端的权威契约；本文不重述它的协议，上面这段设备侧概述才是本固件依赖的内容。

## 6. 持久化

所有设备状态存成一个平坦 blob，因此保存是一次写、加载是一次读。键是 `couple` 命名空间与 `blob` 键（`STORE_NS` 与 `STORE_KEY_BLOB`，`main/couple_store.c:18-19`）。未配对时的兜底本机名是 `PASSPORT`（`STORE_DEFAULT_NAME`，`main/couple_store.c:22`），配对完成后会被真实名字覆盖。

按键绝不直接写 flash。写入是批量的：先在内存里标一次脏，存储节拍在脏标记累计到 8 个、或距上次落盘满 20000 ms 时才落盘（`COUPLE_NVS_FLUSH_DIRTY_LIMIT` 与 `COUPLE_NVS_FLUSH_INTERVAL_MS`，`main/couple_config.h:81-82`；判定在 `couple_store_tick()`，`main/couple_store.c:392`）。这样繁忙的互动时段不会磨损 NVS。同步计数器是例外：加载时立刻落盘一次，之后通过自己的脏标志并入同一条批量写入路径。落盘失败也会重新计时，避免日志与擦写在紧循环里被反复触发（`main/couple_store.c:418`）。

存储字段包括配对身份与对方名字、配对密钥、能量与互动计数、每日计数、连续天数、已见里程碑的位图、同步计数器，以及最后确认的序号。blob 带有魔术字 `0x484C5354` 与 schema 版本 1（`COUPLE_STATE_MAGIC` 与 `COUPLE_STATE_SCHEMA_VERSION`，`main/couple_state.h:41-42`）。

存储把全部访问串行化在一把锁上。调用方是应用任务（例如每次按键检查是否已配对、以及状态刷新）与网络任务（应用确认号时）。因此持锁写入可能拖慢这两者的存储访问；实际阻塞时长尚未在硬件上测量。界面完全不碰存储。

## 7. 界面

页面是纯被动渲染器：`couple_ui.c` 从不读按键，不碰网络、NVS 或音频，且每个公开入口内部都会持 `bsp_lvgl_lock()`，因此任何任务都可以安全调用。

屏幕是 240 × 320 像素（`kScreenW`/`kScreenH`，`main/couple_ui.c:39-40`）。可见安全区纵向为 8 到 312，因为屏角圆角为 30 像素（`main/couple_ui.c:11`）。页面有：

| 页面 | 用途 |
| --- | --- |
| 首页 | 品牌标识、动画心形、在线状态、对方名字、最近收到时间、Love Energy 与总数、键位提示。 |
| 已发出想你 | 一条轻确认，表示想你信号已离开本板。 |
| 收到想你 | 全屏心形由小到大。 |
| 抱抱按住 | 本板正按住 `OK`；显示已按住秒数与进度条。 |
| 收到抱抱 | 对方的抱抱正在播放。 |
| 双向抱抱 | `HUG CONNECTED` 与同步心跳。 |
| 心有灵犀 | `HEART SYNC` 全屏动画。 |
| 里程碑 | 里程碑彩蛋。 |
| 每日 | 我们的一天面板：今日想你、今日抱抱、当天首次互动时间、连续天数。 |
| 菜单 | 三项：配对、Wi-Fi 配网、解除配对。 |
| 配对 | 配对、加入与解绑流程，含 6 位码逐位录入。 |
| 配网 | 给手机看的 Wi-Fi SoftAP 说明页。 |

配色是固定的（`main/couple_ui.c:69-78`）：背景深紫 `0x0E0B14`、卡片 `0x171226`、分隔线 `0x2A2140`、主粉 `0xFF6B9D`、浅粉 `0xFFA6C4`、深粉 `0xC9406F`、主文字 cream `0xF6E7E9`、次要文字 `0x8E86A8`、mint `0x7BE3C0`、amber `0xFFC46B`。

完全不使用位图。心形由矩形拼成：11 × 10 的像素图存成 8 条横向色带（`kHeartBands[]`，`main/couple_ui.c:95`），每条色带按当前单位格尺寸画成一个或两个矩形。单位格为 8 像素时，首页心形是 88 × 80 像素，摆放位置使其中心落在 (120, 110)（`main/couple_ui.c:905-907`）。光标、圆环与进度条同样由矩形构成。

动画使用 LVGL 对上述矩形组做缩放。有两个参数是为了适配整数格化像素刻意改过的，它们是可见的动画选择，不是修 bug。首页心形在 1600 ms 内于 92 % 与 108 % 之间呼吸并无限重复（`main/couple_ui.c:1228`）：单位格是 8 像素，约 98 % 到 104 % 的范围在两个方向四舍五入后都回到同一个 8 像素，动画根本看不出来，而更宽的范围对应单位格 7、8、9 且中心不动，读起来就是心跳。抱抱页的确认跳动幅度固定为 25 %（`main/couple_ui.c:1382`），因为基数格是 4 像素，一格恰好等于 25 %。

上游 Demo 菜单是被替换掉的，不是换皮。这遵循本仓库对衍生应用的强制界面重做规则（`AGENTS.md:40`）：衍生应用必须自建页面、状态机与动画，而不是给 Demo 外壳改名、换色或加功能。应用只复用 BSP API 与非界面逻辑。

## 8. 中文字体

LVGL 自带的 Montserrat 没有中文字形：中文会整片渲染成缺字方框，而且「编译通过」并不代表屏幕上画得出来。因此应用附带两个未压缩的 LVGL 子集字体，码点严格取自界面真正用到的集合。

清单是 `assets/fonts/ui_symbols.txt`：512 个码点，每行一个 `U+XXXX`，由 413 个汉字、89 个 ASCII 与 10 个其它码点组成（`assets/README.md`）。两个生成物是 `assets/fonts/app_font_16.c`（52,940 字节的生成字体数据，用于 `COUPLE_FONT_SMALL` 与 `COUPLE_FONT_BODY`）和 `assets/fonts/app_font_20.c`（81,528 字节，用于 `COUPLE_FONT_TITLE` 与 `COUPLE_FONT_NUM`）；体积数字按 `assets/README.md` 的记录。它们是作为组件源文件编进来的，而不是被 include，因为它们定义的是 `lv_font_t` 常量而不是头文件（`main/CMakeLists.txt:23-25`），并由 `COUPLE_USE_APP_FONT` 宏选择（`main/CMakeLists.txt:25`）。未定义该宏时 `main/couple_fonts.h:32-35` 退回 Montserrat 各字号；但本工程出厂配置无条件定义了这个宏，所以始终使用子集字体，回退分支实际不启用。

源字体是 Noto Sans SC，按 SIL Open Font License 1.1 未修改使用；转换器是 `lv_font_conv` 1.5.3，参数为 `--format lvgl --no-compress --bpp 4`（`assets/README.md`）。两个字号必须一起重生成，才能与别名保持一致。

重生成字体与刷新清单的方法见 `assets/README.md` 的字体章节和[中文字体指南](development/engineering/lvgl-chinese-fonts.zh_CN.md)；那里的命令与素材一起维护。概括地说：用 `tools/ui_codepoints.py` 刷新清单，用 `lv_font_conv` 重建两个字号，再用 `tools/check_font_glyphs.py` 证明覆盖。门禁对两个字号都跑覆盖检查，并要求负控失败。

由于子集只覆盖界面自己编译进去的字符串，固件没有编入的文本会渲染成占位方框，且没有任何警告。通过网络到达的任意 UTF-8 对方名字不在集合内，目前也没有名字录入页面（见第 11 节）。

## 9. 构建、验证与烧录

在仓库根目录运行门禁：

```bash
./tools/validate.sh --static    # 仓库检查 + 宿主测试
./tools/validate.sh --firmware  # ESP-IDF 构建 + 合并镜像验证
./tools/validate.sh             # 完整门禁
```

静态阶段会跑 `tools/check_repo.py`、检查 workflow、编译并运行全部宿主测试（含五个 couple 测试）、对两个字号跑字形检查、再跑 Python 契约测试，最后打印 `Host tests: PASS`（`tools/validate.sh:99`）。完整门禁还需要已激活的 ESP-IDF 5.5.3 环境。

在 Windows 宿主机上，静态阶段需要设置 `CC` 与 `ACTIONLINT_BIN`：

```bash
CC=<你的编译器> ACTIONLINT_BIN=<actionlint 的可执行文件路径> ./tools/validate.sh --static
```

构建请在仓库根目录、在已激活 ESP-IDF 5.5.3 环境的 shell 里运行（Windows 上即先执行 ESP-IDF 的 export 脚本）：

```bash
idf.py build
```

生成合并镜像：

```bash
idf.py merge-bin -o FoloToy-AI-Passport-full.bin
```

`merge-bin` 相对构建目录解析输出路径，所以要传裸文件名或绝对路径；传仓库相对路径不会落到你以为的地方。

预期产物是应用镜像 `build/couple/FoloToy-AI-Passport.bin`、合并镜像 `build/FoloToy-AI-Passport-full.bin`，以及归档 `build/firmware/<sha256>/`，后者还带 `manifest.json` 与 ELF 文件。作为一次记录下来的构建结果：应用镜像 `FoloToy-AI-Passport.bin` 为 1,651,440 字节（0x1932F0），约占 8 MB 应用分区的 20 %；合并镜像 `merged-binary.bin` 为 1,716,976 字节，SHA-256 为 `3386050e82639dfbd410bd74fcdfc5dc62cd7d00ff1e4dff0fbfd2aeb4e3e724`；应用镜像落在偏移 0x10000；工具链报告 ESP-IDF 5.5.3、目标 esp32c3、8 MB Flash。请把这些当作一次构建的记录结果，而不是对下一次的保证：你自己构建出来的体积与哈希都会不同。

factory 分区是 `0x7F0000`，即 8,323,072 字节（`partitions.csv`），因此记录中的应用镜像还剩下 6,671,632 字节空闲，约占该分区的 80 %。接入两个字体文件是镜像体积增长的主要原因。

烧录时要注意存储数据。已验证的合并镜像从 0x0 写入，其中包含 NVS 与 PHY 数据区，因此可能清掉已存状态。开发期若要保留现有 NVS 状态，请改用分段方式 `idf.py flash`，不要写整镜像。完整政策见 [firmware-layout.md](development/engineering/firmware-layout.zh_CN.md#flashing-and-stored-data)，依赖与构建细节见 `docs/development/engineering/build-and-test.md`。

## 10. 配对、解绑与日常使用

**配网。** 从首页按 `UP` 或 `DOWN` 打开菜单，移动到「Wi-Fi 配网」，按 `OK` 选择。板子会起一个名为 `Heartlink-XXXX` 的 SoftAP，后缀来自板子的 MAC 地址，密码是 `heartlink`（`COUPLE_PROV_AP_SSID_PREFIX` 与 `COUPLE_PROV_AP_PASS`，`main/couple_prov.h:37` 和 `main/couple_prov.h:39`）。用手机连上这个热点，浏览器打开 `http://192.168.4.1/`（`COUPLE_PROV_HTTP_PORT`，`main/couple_prov.h:41`）。页面收集 Wi-Fi 名称、Wi-Fi 密码与 relay 地址，只有三项都通过校验才交回应用；配网模块自己从不保存凭据。之后板子关闭 SoftAP 并切到站点模式——AP 与 STA 不同时运行。页面显示 SSID、密码与要打开的地址，并统计有多少台手机连上；该计数每秒轮询一次，因此最多约有一秒延迟。

**两块板配对。** 两块板都配好网并在线后，在其中一块上打开菜单选择配对。该板向 relay 申请一个 6 位配对码（`COUPLE_PAIR_CODE_LEN`，`main/couple_types.h:19`）并显示它以及剩余秒数；配对码有有效期。在另一块上选择配对并逐位输入：`UP`/`DOWN` 改当前数字，`OK` 进入下一位，按住 `OK` 退出。第二块提交有效配对码后，relay 完成配对，两块板显示已配对状态，从此它们共享密钥与这段关系。

**解绑。** 打开菜单选择解除配对。板子会请求确认，并提示能量与回忆都会留着。设备会重试几次解绑请求，然后无论结果如何都清掉本地配对与待发队列，因为用户意图优先于网络好坏：既然你点了解绑，两块板就不能继续显示成一对。除了重新配对，没有撤销。

**日常使用。** 在首页短按 `OK` 发出想你，按住 `OK` 抱抱。首页显示对方是否在线、对方名字、最近一次收到想你的时间，以及你的能量与总数。首页按 `UP`/`DOWN` 打开菜单。按住 `OK` 至少三秒会取消当前页并回首页，这是配对页与配网页的逃脱出口。

## 11. 状态与限制

```text
Build: PASS — 应用镜像 FoloToy-AI-Passport.bin 1,651,440 字节（0x1932F0），约占 8 MB 应用分区的 20 %；合并镜像 merged-binary.bin 的 SHA-256 为 3386050e82639dfbd410bd74fcdfc5dc62cd7d00ff1e4dff0fbfd2aeb4e3e724；ESP-IDF 5.5.3。这些是某一次构建的记录结果，你自己构建出来的哈希会不同。
Host tests: PASS — tests/test_install_passport_skills.py 有 3 个用例在本 Windows 宿主上失败，原因是创建目录符号链接需要符号链接权限（[WinError 1314]），属环境限制而非代码缺陷；其余全部通过。
Device tests: PASS — 两块 AI PASSPORT 真机已完成配对，双向「想你」与远程抱抱互动均已跑通。
Unverified: 抱抱的后两档时长；HUG CONNECTED（双方同时长按）从未触发验证；离线待收取队列未验证；端到端按键延迟未逐项计时；设备实际运行固件的 ELF 从未回读确认。
```

已知并接受的限制，如实列出：

1. **子集字体覆盖。** 中文字体是机械抽取 `main/` 各编译单元字符串字面量中出现的码点生成的子集，目前 512 个码点，与 `assets/fonts/ui_symbols.txt` 一致。集合之外的字符会渲染成占位方框。通过网络到达的任意 UTF-8 对方名字不在集合内，目前也没有名字录入页面。中文在真机上的渲染已在两块板子上验证通过。

2. **网络层的已知未修项。** 以下各条都未修复，作为已知开放问题记录。
   - (a) 没有针对「对端重放此前抓取的帧」的重放保护。响应帧的 `seq` 从不与本地水位比较，因此设备侧对收到的帧没有去重或防重放。
   - (b) 同步成功但服务端确认号没有前进时，重试路径没有专门退避，循环只是再发一次（`main/couple_net.c:1195`）。
   - (c) NVS 写入路径只持一把锁，因此可能拖慢应用任务与网络任务的存储访问。真实阻塞时长尚未在硬件上测量。
   - (d) `couple_net_stop()` 丢弃 `xQueueSend` 的返回值，且命令队列深度为 8（`main/couple_net.c:1279`，`NET_CMD_QUEUE_LEN` 为 8），因此最坏情况下停止可能等数十秒，直到在途请求收尾。
   - (e) 配对响应不签名。这一条是配对阶段已记录并接受的设计取舍（见第 5 节），不是疏漏。

3. **一条被丢弃的抱抱结束事件。** 在一条收尾路径里，`HUG_END` 事件入队后网络随即停止，因此这一条基本发不出去。relay 上本机的 holding 标志会自行超时。这一点在代码中有记录并被接受（`main/couple_app.c:1751-1767`）。

4. **刻意的动画选择。** 有两个动画参数是为了适配整数格化像素刻意改的：首页心形的呼吸范围改为 92 % 至 108 %，抱抱页的跳动幅度改为固定 25 %。这些是可见的动画决定，不是修 bug（见第 7 节）。

5. **镜像体积。** 接入两个字体文件显著抬高了镜像体积。记录中的数字与剩余分区余量见第 9 节。

6. **Windows 特有的测试失败。** `tests/test_install_passport_skills.py` 的三个失败是这台 Windows 宿主的环境限制——创建目录符号链接需要开发者模式或符号链接权限，表现为 `WinError 1314`——不是应用的缺陷。Linux 上的 CI 预期能通过它们。在本 Windows 宿主上运行 `./tools/validate.sh --static` 会复现这三个失败，同时仓库检查与全部 C 宿主测试均通过。

7. **真机验证只完成了一部分。** 真机上已验证基本双向互动；但抱抱档位 2/3、`HUG CONNECTED`、离线待收队列、以及端到端延迟尚未逐项验证。

## 12. 后续方向

已有若干为扩展预留的设计点：

- **对方身份。** 对方名字的解析、存储与显示都已存在；加一个名字录入页面就能让用户直接设置，而不必依赖网络传来的内容。
- **事件协议。** 帧格式本来就承载任意带类型的、带 8 字节 value 字段的事件（`COUPLE_EVENT_VALUE_MAX`，`main/couple_types.h:21`），因此新增事件类型无需改动传输层。
- **Love Energy。** 它是一个持久的单一计数器，只有一条发放路径，因此新的能量来源可以只在一处添加。
- **同步。** 同步循环、待发队列与确认号处理都隔离在网络模块里，因此可以在既有轮次上叠加更多服务端驱动的行为。

以下功能是预留的未来工作，尚未实现：双人电子宠物、我们的秘密宇宙、像素情侣小屋、未来情书、每日情侣任务、情侣默契问答、情侣命运机、恶作剧系统。