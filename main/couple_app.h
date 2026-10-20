#pragma once

/*
 * Heartlink 情侣心动终端 —— 应用编排层。
 *
 * 这是唯一一个「知道全部模块」的地方：它把按键、网络、音频、存储、界面
 * 串成一条产品流程，并且是 these 模块的唯一所有者。
 *
 *     按键 ISR/回调 --(队列)--> couple_app 任务 --(+---)--> couple_ui   (画)
 *     couple_net 回调 --(队列)-->                 (+---)--> couple_audio(响)
 *     esp_timer 1 Hz  --(队列)-->                 (+---)--> couple_store(存)
 *
 * 职责边界：
 *   - 决定「这一刻该看哪一页、该响什么音、该记什么分」
 *   - 解码按键：短按 OK = 想你，长按 OK = 抱抱，UP/DOWN = 翻页与录入
 *   - 维护抱抱的实时进程（按住多久、当前档位）
 *   - 维护配对 / 配网 / 解绑这三条低频流程的用户可见状态
 *
 * 不负责：
 *   - 任何 LVGL 对象（那是 couple_ui）、任何 HTTP/HMAC（那是 couple_net）、
 *     任何 NVS 键值（那是 couple_store）、任何波形合成（那是 couple_audio）
 *
 * ============================ 线程模型（硬约束）============================
 *
 * 本模块内部只有一个任务（"couple_app"，栈 4096、优先级 4）。所有会对
 * 慢资源动手的事情都发生在这个任务里，别的上下文只许入队：
 *
 *   - couple_app_handle_key() 由 main.c 的输入任务调用，只做一次非阻塞 send，
 *     队列满就丢并计数。绝不在里面碰 LVGL / Flash / 网络 / 音频。
 *   - couple_net 的三个回调（on_state / on_sync / on_pair）跑在 net worker
 *     任务里，属于「禁止阻塞」的上下文，同样只把数据拷进队列。
 *     所以本模块对回调参数只做值拷贝（数组、定长字符串），不保存任何指针。
 *   - 内部 esp_timer 只负责往队列里投 TICK，不直接画界面。
 *
 * 反过来，couple_ui / couple_audio / couple_store 的公开接口都能被任意任务
 * 调用（各自内部加锁），所以 app 任务直接调它们是安全的。
 *
 * ============================ 按键语义 ============================
 *
 * 官方 bsp_button 在按住超过阈值时先发 LONG、松手时再发 RELEASE，
 * 短按则会在松手后给 CLICK。因此：
 *
 *   OK  CLICK              -> 想你（仅在首页/空闲时）
 *   OK  LONG               -> 进入抱抱：开始 HUG_HOLD，并 set_holding(true)
 *   OK  RELEASE(抱抱中)    -> 结束抱抱：按 held_ms 定档位、上报 HUG_END
 *   OK  RELEASE(非抱抱)    -> 忽略（短按已由 CLICK 处理，避免双发）
 *   OK  长按(≥3s)          -> 取消当前页并回首页（配对/配网页的逃脱出口）
 *   UP / DOWN              -> 首页：打开菜单；菜单：移动高亮；配对录入：加减数字
 *
 * 「想你」只在确定没有进入长按时才发，因此短按不会误报抱抱、长按也不会
 * 顺带发一条想你。
 *
 * ============================ 生命周期 ============================
 *
 * main.c 的调用顺序（每一步都依赖前一步成功）：
 *
 *   bsp_display_init() -> bsp_lvgl_init() -> bsp_display_backlight()
 *     -> 输入队列 + 输入任务 + bsp_button_init(on_key)
 *     -> couple_audio_init()        （音频是 bsp_audio 的唯一所有者）
 *     -> bsp_battery_init()
 *     -> couple_ui_init()           （建页面，必须在 lvgl_init 之后）
 *     -> couple_app_init()          （本模块：接管 store/wifi/net/prov）
 *
 * couple_app_init() 内部按序完成：couple_store_init() -> couple_wifi_begin()
 * -> couple_net_start() -> 建任务/定时器 -> 推一次 UI 状态。
 * Wi-Fi 凭据、relay 地址都从 NVS 里读；没有凭据时网络停在 IDLE，
 * 界面引导用户走配网，设备本身仍然完整可用（本地统计、里程碑、动画）。
 *
 * 深睡前必须调 couple_app_shutdown()：它会停掉网络与轮询、等音频静音、
 * 把 NVS 落盘。返回非 ESP_OK 表示「还没排空，最好别进深睡」。
 */

#include <stdbool.h>

#include "esp_err.h"

#include "bsp_button.h"

/*
 * 启动编排层。幂等：已经启动时返回 ESP_OK。
 *
 * 前置条件：couple_store 的 NVS 分区可用（由本函数自己 init）、
 * couple_ui_init() 与 couple_audio_init() 已经成功返回。
 * 本函数持锁调用 LVGL 的只是「推一次状态」，不会长时间占用。
 */
esp_err_t couple_app_init(void);

/* 是否已经成功启动。 [任意任务] */
bool couple_app_started(void);

/*
 * 输入任务的唯一入口。任何 btn/ev 组合都可以传，本函数自己判断当前页面要不要理它。
 * 非阻塞、不做任何慢操作，可以在按键回调里直接调。 [任意任务]
 *
 * 队列满时丢弃并按 uart 日志计数——按键属于「丢了也不会坏状态」的输入，
 * 宁可丢键也不许在这里阻塞输入任务。
 */
void couple_app_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev);

/*
 * 收尾：停网络、等音频静音、落盘、可选地停配网 AP。
 * 只做一次，重复调用返回 ESP_OK。 [任意任务，不能在 couple_net 回调里调]
 *
 * 返回 ESP_ERR_TIMEOUT 表示某个慢操作没能及时收尾（例如长轮询还在路上），
 * 调用方此时不应直接进深睡，稍后再试或放弃本次深睡。
 */
esp_err_t couple_app_shutdown(void);