<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# OurPassport · 双人情侣心动终端

**两块掌心大的小机器，只认对方一个人。**

你短按一下 OK，几千公里外那颗心就会亮起来，从小到大，一路跳到你眼前。

[![License: MIT](https://img.shields.io/badge/License-MIT-black.svg)](LICENSE)
[![Target: ESP32-C3](https://img.shields.io/badge/target-ESP32--C3-blue.svg)](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md)
[![ESP-IDF 5.5.3](https://img.shields.io/badge/ESP--IDF-5.5.3-red.svg)](docs/development/engineering/build-and-test.md)
[![UI: LVGL](https://img.shields.io/badge/UI-LVGL-9cf.svg)](docs/README.zh_CN.md)

---

## 这是什么

两块 FoloToy **AI PASSPORT** 掌机（ESP32-C3，240×320 屏幕，没有 PSRAM），刷上这套固件之后，它们会变成一对只有彼此能触碰的终端。

- **不是聊天软件。** 没有输入法，没有消息列表，没有已读回执。
- **只有对方能影响我的设备。** 陌生设备什么都送不进来——没配对之前，两块板互相是空气。
- **屏幕很小，但每一眼都是给 TA 的。** 240×320 的尺寸装不下朋友圈，只装得下一颗心。

你没法在它上面打字，所以它也逼着你换一种表达方式：不能解释、不能找补、不能发一长串——只能按下去，让对面亮起来。

---

## 玩法

### ♥ 想你

按一下 OK。

对面整块屏幕会亮起一颗心，从小到大撑满，配一声短短的可爱音效，然后出现：

```
        TA刚刚想你了
       距上次想念 3 小时前
```

发送方看到的是一句轻确认：`想你已送达`，能量 +1，外加一行小字 `等 TA 也想想你`。收到的那台还会记着「距离上一次收到想念过了多久」——所以你会知道，TA 到底忍了多久才按这一下。

没联网也没关系。互动不会丢，会进待发队列（容量 32 条），TA 一上线就补齐，还会附上一句：

```
你不在的时候，TA想了你 3 次。
```

### 🫂 远程抱抱

**按住 OK 不放。** 你按住多久，对面的抱抱动画就持续多久。

| 按住的时长 | 档位 |
| --- | --- |
| ≥ 0.7 秒 | 小抱抱 |
| ≥ 3 秒 | 大抱抱 |
| ≥ 8 秒 | 不撒手抱抱 |

按住的时候本机屏幕会实时数着 `已抱住 4.2 秒`，松手才送出去（上限 60 秒，再多就贪心了）。对面收的时候是 `TA抱住了你`，并且照实告诉你被抱了多久。

**两个人同时按住 OK**，两块板会一起进入：

```
      HUG CONNECTED
   双向抱抱 · LOVE ENERGY +5
        别撒手
```

同步心跳动画在两边一起跳——这是这套系统里最上头的一刻。

### ✨ 心有灵犀

**10 秒之内，两个人互相按下了「想你」。**

这不是巧合，这是两块板都认得出的事。触发时两边同时进入全屏 `HEART SYNC` / `心有灵犀`，能量 +5，动画和普通互动完全不一样。

你会开始为了撞上这 10 秒，故意按下之后盯着屏幕等三秒。

### ⚡ LOVE ENERGY

所有互动都在攒同一种东西，它是后面所有养成玩法的唯一通货：

| 发生了什么 | 得到 |
| --- | --- |
| 送出一次想你 | +1 |
| 收到一次想你 | +1 |
| 心有灵犀 | +5 |
| 双向抱抱 | +5 |
| 当天有互动（连续互动奖励） | +3 |

首页上那个数字就是它。它不是分数，更像两个人的存款——慢慢涨，谁也不掉。

### 🎁 里程碑彩蛋

互动总数踩到这几个数的时候，会有一次性彩蛋，额外送你「阈值本身」那么多能量：

`9` · `18` · `99` · `100` · `365` · `520` · `999` · `1314`

其中两个必须截屏：

- **520** —— `520 次心动，都不如你。`
- **1314** —— `一生一世，从这台小机器开始。`

前者是「我爱你」，后者是「一辈子」。屏幕只有 240×320，但这两句话配了专属动画，值得拍下来发过去。

### 🥚 随机彩蛋

收到想念的时候，文案偶尔会被替换成别的：

```
有人偷偷想你啦。
警告：检测到过量想念。
对方申请抱抱。
SYSTEM ERROR: PARTNER TOO CUTE
心跳数据已同步。
距离正在缩短中。
本条消息含糖量超标。
TA把想念藏进了这条消息。
```

还有一条是藏起来的：**当天第 7 次想你**时必定出现——

```
今天第 7 次想你，多少有点粘人了。
```

### 📅 我们的一天

往上拨一下，是当天的账本：

```
        我们的一天
今日想你        12 次
今日抱抱         4 次
今天首次互动   09:41
连续互动        26 天
```

**设计红线：这里不会出现任何让你有压力的东西。**

- 一天没互动，不会出现责备文案——没互动就显示 `今天还没互动`。
- 连续天数断了，也不会骂你，只写 `从今天开始`。
- 没有排行榜，没有「TA 比你付出得多」，没有已读未回，没有任何一条会让人产生愧疚感的机制。

这套东西的目的是让两个人更近，不是让谁证明什么。

---

## 三十秒上手

1. **刷固件**（下面两种部署方式任选一种）
2. **配网**：`菜单 → Wi-Fi 配网`，手机连上它开的热点填 Wi-Fi 和服务器地址
3. **配对**：一台 `菜单 → 配对 → OK 申请配对码`，念给另一台；另一台 `菜单 → 配对 → 加入配对`，↑/↓ 拨出那 6 个数字
4. **玩**：一台短按 OK，另一台就会亮

---

## 部署方式 A：手搓

适合手上已经有 ESP-IDF、想自己改代码的人。全程大概十五分钟（不含下载 SDK）。

### 1. 准备环境

需要 **ESP-IDF 5.5.3**。官方安装与激活流程见
[build-and-test](docs/development/engineering/build-and-test.md)。

### 2. 编译

```sh
cd ai-passport
idf.py build
```

### 3. 烧录

```sh
idf.py -p <你的串口> flash monitor
```

> ⚠️ **不要刷 `build/merged-binary.bin`。**
>
> 合并镜像是从地址 `0x0` 开始的一整段连续镜像（含 0xFF 填充），而
> [`partitions.csv`](partitions.csv) 把 NVS 放在 `0x9000`——正好落在它覆盖的范围里。
> 刷合并镜像会把 NVS 区一起涂掉，**配对关系、Wi-Fi 凭据、Love Energy 全部清零**。
> 用 `idf.py flash`（只写 bootloader / 分区表 / 应用三块），或者手动指定这三个偏移：
>
> ```sh
> python -m esptool --chip esp32c3 -p <你的串口> -b 460800 \
>   write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
>   0x0 build/bootloader/bootloader.bin \
>   0x8000 build/partition_table/partition-table.bin \
>   0x10000 build/FoloToy-AI-Passport.bin
> ```

### 4. 配网

`菜单 → Wi-Fi 配网`。板子会起一个热点：

| 项目 | 值 |
| --- | --- |
| 热点名 | `Heartlink-XXXX`（XXXX 是 MAC 后两字节，屏幕上会显示） |
| 热点密码 | `heartlink` |
| 配置页 | 手机浏览器打开 `http://192.168.4.1/` |

在配置页里填四样东西：家里 2.4 GHz 的 **Wi-Fi 名称**、**Wi-Fi 密码**、**Heartlink 服务器地址**、**你的名字**。

服务器地址必须写 `http://` 开头——固件里没有 TLS 栈，写 `https://` 会一直按网络故障重连。配好之后存 NVS，重启还在。

### 5. 搭 relay 服务器

两块板不在同一个局域网的时候，需要一台双方都能访问的机器做中转：

```
Device A  ↔  Internet Relay  ↔  Device B
```

服务端就在本仓库的 [`relay/`](relay/README.zh_CN.md)：**一个 Python 文件，只用标准库，不装依赖、不用数据库**。

```sh
cd relay
python3 relay.py --host 0.0.0.0 --port 8787 --data-file relay-state.json
```

放到自己的服务器上常驻，仓库里已经备好了 systemd 模板：

```sh
sudo mkdir -p /opt/aipassport-relay
sudo cp relay.py /opt/aipassport-relay/
sudo cp deploy/aipassport-relay.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now aipassport-relay
```

验证：

```sh
curl -s http://<你的主机>:8787/healthz    # -> ok
```

不想暴露自己服务器的真实 IP，可以拿 Cloudflare Tunnel 套一层——模板见
[`relay/deploy/cloudflared-config.yml.example`](relay/deploy/cloudflared-config.yml.example)。
两个坑写在模板头部注释里了，尤其是**域名的 Always Use HTTPS 必须关掉**（设备跟不了 301 跳转），而固件里的地址**仍然写 `http://`**（TLS 在 Cloudflare 那一侧终结，不关设备的事）。

完整的服务端契约（帧格式、端点、错误码、配对/同步/解绑流程、状态文件）在
[`relay/README.zh_CN.md`](relay/README.zh_CN.md)。

### 6. 配对

1. 板 A：`菜单 → 配对`，按 OK **申请配对码**——屏幕会显示一串 6 位数字，带倒计时。
2. 板 B：`菜单 → 配对 → 加入配对`，用 ↑/↓ 拨数字，按 OK 到下一位。
3. 6 位填完之后**再按一次 OK** 才提交（这一步最容易漏）。
4. 两台都显示 `配对成功` / `从今天开始一起记数`。

配对关系存在 NVS，重启、断网、换 Wi-Fi 都还在。想解绑：`菜单 → 解除配对`，**按住 OK** 确认。对面的板会收到 `TA 解除了绑定`——能量和回忆都留着。

---

## 部署方式 B：交给 AI

这个仓库是**为 AI agent 写的**。根目录的 [`AGENTS.zh_CN.md`](AGENTS.zh_CN.md) 是入口，里面有一张「任务 → 该读哪些文档」的路由表；[`skills/`](skills/README.zh_CN.md) 下备了五个技能：

| 技能 | 干什么 |
| --- | --- |
| `passport-setup` | 装工具链、克隆 ESP-IDF |
| `passport-build` | 编译、分区、镜像校验 |
| `passport-develop` | 改 BSP / UI / 状态机 |
| `passport-device-test` | 真机烧录与串口验证 |
| `passport-debug` | 崩溃、重启、没反应的时候用 |

所以你要做的事只有一件：**把下面这段话复制给你的 AI**。

```text
读本仓库根目录的 AGENTS.md（习惯中文的话读 AGENTS.zh_CN.md），
按它的路由表配置好开发环境，然后在本仓库把 OurPassport 这套情侣终端固件编译出来。
编译完成后告诉我固件路径、镜像大小和 SHA-256。
不要刷写设备，等我说。
```

它会自己去读文档、装环境、编译、报结果。后面想让它改文案、加动画、调抱抱档位、加新的互动玩法，直接把 `AGENTS.md` 甩给它，然后说人话就行。

这也是为什么这套固件的注释和文档写得比一般玩具项目啰嗦——**它们是写给下一个 AI 看的说明书**。

---

## 硬件与实现

| 项目 | 值 |
| --- | --- |
| 主控 | ESP32-C3 |
| Flash | 8 MB |
| PSRAM | 无 |
| 屏幕 | ST7789P3 240×320，4-line SPI |
| 框架 | ESP-IDF 5.5.3 + LVGL |
| 固件版本 | 1.0.0（`main/couple_config.h`） |

这套固件**没有**沿用官方 Demo 的测试菜单，首页、动画、菜单都是重新设计的（这是仓库的强制规则：衍生应用必须重做 UI）。BSP 那一层是复用的。

代码是按职责分层的，慢操作一律不进 LVGL 回调和按键回调：

```
main/
├── couple_ui.c        所有页面与动画（只在 LVGL 上下文里跑）
├── couple_app.c       状态机：按键事件 → 互动 → 界面
├── couple_state.c     信任的纯逻辑：能量、连续天数、里程碑、抱抱档位
├── couple_net.c       与 relay 的同步循环（长轮询）
├── couple_proto.c     HL 帧编解码、防重放
├── couple_store.c     NVS 持久化（20 秒 / 8 个脏标记批量落盘）
├── couple_wifi.c      Wi-Fi 关联与重连退避
├── couple_prov.c      SoftAP 配网 + 手机配置页
├── couple_audio.c     音效 worker（队列式，不阻塞按键）
├── couple_radio.c     电量、背光、亮度
├── couple_tone.c      音色合成（纯逻辑，可宿主机测试）
└── couple_config.h    所有可调参数集中在这里
```

- **网络**：HTTP 长轮询 + 紧凑二进制帧，离线待发队列 32 条，指数退避重连（2 秒起，最长 60 秒）。
- **持久化**：不按键就写 Flash。脏标记攒着，20 秒一次或满 8 个才落盘；同步计数器按 128 的步长持久化、开机预留 1024 的余量，重启后既单调又不会把 Flash 写穿。
- **测试**：状态机、协议、音色、存储编码全都有宿主机单元测试（`tests/`），不依赖硬件。

---

## 已知限制

- 真机上验证过的是**基本双向互动**：配对、想你、抱抱。抱抱的后两档时长、`HUG CONNECTED`（双方同时长按）、离线待收取队列、端到端延迟，都还没有逐项实测。
- 固件**没有 TLS 栈**。relay 的地址必须是 `http://`；要加密就套 Cloudflare Tunnel，别改固件里的 scheme。
- 中文界面依赖仓库里预生成的中文子集字体（[`assets/fonts/`](assets/README.zh_CN.md)）。缺字形时 LVGL 会渲染成空白而不是方框，所以改文案要跟着重跑字形检查工具。
- relay 目前是单机单进程、状态存一个 JSON 文件。够两个人用，不打算支撑规模。
- Windows 上跑完整门禁时，`tests/test_install_passport_skills.py` 有 3 个用例会因为文件系统符号链接权限（`[WinError 1314]`）失败，属环境限制，不是代码问题。

---

## 路线图

这套固件是按「以后还要长东西」的架构写的——partner identity、事件协议、Love Energy、同步机制都是可扩展的。接下来想做的：

- 🐣 **双人电子宠物** —— 两个人一起养，一起喂
- 🌌 **我们的秘密宇宙** —— 只属于两个人的小世界
- 🏠 **像素情侣小屋** —— 一块屏幕里的一个家
- 💌 **未来情书** —— 写给某个日期的信，到点才会拆
- ✅ **每日情侣任务** —— 一起完成，不是为了考核
- 🧠 **情侣默契问答** —— 两块板同时出同一题
- 🎰 **情侣命运机** —— 今天该谁洗碗
- 😈 **恶作剧系统** —— 让对方的屏幕抖一下

判断标准只有一条：

> 如果这是我对象送我的一块小设备，我会不会因为这个功能觉得很惊喜？

如果答案只是「实用」，那就重做。

---

## 致谢与许可

- 硬件与 BSP 来自 [FoloToy AI PASSPORT](https://github.com/FoloToy/ai-passport)，感谢他们把一块这么好玩的小板子开源出来。
- 本项目新增的应用层代码、UI、文案、relay 服务端：MIT，见 [LICENSE](LICENSE)。

祝你按下去的时候，对面正好也在按。