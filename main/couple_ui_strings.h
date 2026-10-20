#pragma once

/*
 * Heartlink 情侣心动终端 —— 界面文案表。
 *
 * 为什么要单独一个头文件：
 *   1. 中文子集字体的生成器需要一份「界面到底用到哪些字」的权威列表。
 *      文案集中在本文档，生成器只要扫这一个文件就能收集全部码点。
 *   2. 没有中文字体时（COUPLE_USE_APP_FONT 未定义）整个界面退化成英文，
 *      不会出现 LVGL 的 missing glyph 方框——那种方框在真机上才看得见，
 *      编译期完全无声。
 *
 * 规则：
 *   - 两条分支的宏名必须一一对应；%s / %u 占位符的个数与顺序也必须一致。
 *   - 英文分支只允许 ASCII：官方 Montserrat 没有 ↑ ↓ · – 「」…… 这些字形，
 *     缺字形时渲染出来是「空白」而不是方框，最难发现。
 *   - 中文分支的标点一律用全角（，。？），子集生成器会一并收集。
 *   - 中文分支里保留的 OUR LINK / LOVE ENERGY / HUG CONNECTED / HEART SYNC /
 *     Wi-Fi / http:// 是有意的品牌与术语，不要翻译；生成器必须把 ASCII
 *     一并收进子集字体，否则这些行会整行消失。
 */

#ifdef COUPLE_USE_APP_FONT

/* ---------- 首页 ---------- */

#define COUPLE_STR_BRAND "OUR LINK"
#define COUPLE_STR_HOME_ENERGY_CAPTION "LOVE ENERGY"
#define COUPLE_STR_HOME_TOTAL_FMT "%u 次心动"
#define COUPLE_STR_STATUS_ONLINE "对方在线"
#define COUPLE_STR_STATUS_OFFLINE "对方不在线"
#define COUPLE_STR_STATUS_UNPAIRED "还没有绑定"
#define COUPLE_STR_HOME_AGO_FMT "距上次想你 %s"
#define COUPLE_STR_HOME_HINT_MISS "[OK] 想你"
#define COUPLE_STR_HOME_HINT_HUG "按住 OK 抱抱"
#define COUPLE_STR_HOME_HINT_MORE "↑↓ 更多"
#define COUPLE_STR_BATTERY_FMT "%u%%"

/* ---------- 想你的相对时间（couple_format_ago 是中文专用，英文分支另行实现） ---------- */

#define COUPLE_STR_AGO_NOW "刚刚"
#define COUPLE_STR_AGO_MIN_FMT "%u 分钟前"
#define COUPLE_STR_AGO_HOUR_FMT "%u 小时前"
#define COUPLE_STR_AGO_DAY_FMT "%u 天前"

/* ---------- 想你（发出） ---------- */

#define COUPLE_STR_MISS_SENT_TITLE "想你已送达"
#define COUPLE_STR_MISS_SENT_SUB "LOVE ENERGY +1"
#define COUPLE_STR_MISS_SENT_HINT "等 TA 也想想你"

/* ---------- 想你（收到） ---------- */

#define COUPLE_STR_MISS_RECV_TITLE "TA刚刚想你了"
#define COUPLE_STR_MISS_RECV_AGO_FMT "距上次想念 %s"
#define COUPLE_STR_MISS_RECV_FLAVOR_DEFAULT "想你想到现在就告诉你"
#define COUPLE_STR_MISS_RECV_HINT "按住 OK 抱回去"

/* ---------- 抱抱 ---------- */

#define COUPLE_STR_HUG_HOLD_TITLE_IDLE "抱住…"
#define COUPLE_STR_HUG_HOLD_HINT "松开 OK 送出抱抱"
#define COUPLE_STR_HUG_HELD_SECONDS_FMT "已抱住 %u.%u 秒"

/*
 * 档位中文标签。
 * 中文分支实际由 couple_state.h 的 couple_hug_tier_label() 提供（唯一权威），
 * 这里保留同样的字面量，只为让子集生成器一定能扫到这几个字。
 */
#define COUPLE_STR_TIER_SMALL "小抱抱"
#define COUPLE_STR_TIER_BIG "大抱抱"
#define COUPLE_STR_TIER_CLINGY "不撒手抱抱"

#define COUPLE_STR_HUG_RECV_TITLE "TA抱住了你"
#define COUPLE_STR_HUG_RECV_DURATION_FMT "TA抱了你 %u.%u 秒"
#define COUPLE_STR_HUG_RECV_HINT "按住 OK 也抱一个"
#define COUPLE_STR_HUG_SYNC_TITLE "HUG CONNECTED"
#define COUPLE_STR_HUG_SYNC_SUB "双向抱抱 · LOVE ENERGY +5"
#define COUPLE_STR_HUG_SYNC_FOOTER "别撒手"
#define COUPLE_STR_HEART_SYNC_TITLE "HEART SYNC"
#define COUPLE_STR_HEART_SYNC_SUB "心有灵犀"
#define COUPLE_STR_HEART_SYNC_ENERGY "LOVE ENERGY +5"

/* ---------- 里程碑 ---------- */

#define COUPLE_STR_MILESTONE_BONUS_FMT "额外 +%u 能量"
#define COUPLE_STR_MILESTONE_ROMANTIC_520 "520 次心动，都不如你。"
#define COUPLE_STR_MILESTONE_ROMANTIC_1314 "一生一世，从这台小机器开始。"
#define COUPLE_STR_MILESTONE_HINT "↑↓ 更多"
#define COUPLE_STR_MILESTONE_OOR_TOAST "新的里程碑就在前面"

/* ---------- 每日统计 ---------- */

#define COUPLE_STR_DAILY_TITLE "我们的一天"
#define COUPLE_STR_DAILY_MISSES_LABEL "今日想你"
#define COUPLE_STR_DAILY_HUGS_LABEL "今日抱抱"
#define COUPLE_STR_DAILY_FIRST_LABEL "今天首次互动"
#define COUPLE_STR_DAILY_STREAK_LABEL "连续互动"
#define COUPLE_STR_DAILY_TIMES_FMT "%u 次"
#define COUPLE_STR_DAILY_NONE "今天还没互动"
#define COUPLE_STR_DAILY_STREAK_DAYS_FMT "%u 天"
#define COUPLE_STR_DAILY_STREAK_ZERO "从今天开始"
#define COUPLE_STR_DAILY_HINT "↑↓ 更多"

/* ---------- 菜单 ---------- */

#define COUPLE_STR_MENU_TITLE "菜单"
#define COUPLE_STR_MENU_ITEM_PAIR "配对"
#define COUPLE_STR_MENU_ITEM_PROV "Wi-Fi 配网"
#define COUPLE_STR_MENU_ITEM_UNPAIR "解除配对"
#define COUPLE_STR_MENU_HINT "[OK] 选择  ↑↓ 移动"

/* ---------- 配对 ---------- */

#define COUPLE_STR_PAIR_OFFER_TITLE "把下面这串数字给对方"
#define COUPLE_STR_PAIR_TTL_FMT "剩余 %u 秒"
#define COUPLE_STR_PAIR_OFFER_HINT "把码念给 TA 听"
#define COUPLE_STR_PAIR_ENTRY_TITLE "输入对方屏幕上的数字"
#define COUPLE_STR_PAIR_ENTRY_HINT "↑↓ 换数字  [OK] 下一位  按住 OK 退出"
#define COUPLE_STR_PAIR_CONFIRMED_TITLE "配对成功"
#define COUPLE_STR_PAIR_CONFIRMED_HINT "从今天开始一起记数"
#define COUPLE_STR_UNPAIR_TITLE "要解除和 TA 的绑定吗？"
#define COUPLE_STR_UNPAIR_DETAIL "能量和回忆都会留着"
#define COUPLE_STR_UNPAIR_HINT "按住 OK 确认解除"

/* ---------- 配网 ---------- */

#define COUPLE_STR_PROV_AP_TITLE "手机连接这个 Wi-Fi"
#define COUPLE_STR_PROV_SSID_FMT "名称  %s"
#define COUPLE_STR_PROV_PASS_CAPTION "密码"
#define COUPLE_STR_PROV_IP_CAPTION "在手机浏览器打开"
#define COUPLE_STR_PROV_IP_FMT "http://%s"
#define COUPLE_STR_PROV_CLIENTS_FMT "已有 %u 台设备连上"
#define COUPLE_STR_PROV_CLIENTS_WAITING "等待设备连接"
#define COUPLE_STR_PROV_HINT "在手机上完成即可"

/* ---------- 顶部横幅（对方离线期间的汇总） ---------- */

#define COUPLE_STR_OFFLINE_SUMMARY_FMT "你不在的时候，TA想了你%u次。"

/* ---------- 配对流程的过程提示（由 couple_app 传给 UI） ---------- */

#define COUPLE_STR_PAIR_HEADLINE "配对"
#define COUPLE_STR_PAIR_CHOICE_DETAIL "OK 申请配对码  ↑↓ 输入 TA 的码"
#define COUPLE_STR_PAIR_APPLY_HEADLINE "配对码"
#define COUPLE_STR_PAIR_APPLY_DETAIL "正在申请…"
#define COUPLE_STR_PAIR_JOIN_HEADLINE "配对"
#define COUPLE_STR_PAIR_JOIN_WAIT_DETAIL "等 TA 输入那 6 个数字"
#define COUPLE_STR_PAIR_SUBMIT_HEADLINE "确认中"
#define COUPLE_STR_PAIR_SUBMIT_DETAIL "正在和 TA 牵线…"
#define COUPLE_STR_PAIR_ERR_NOT_FOUND "没找到这个配对码"
#define COUPLE_STR_PAIR_ERR_EXPIRED "这个配对码过期了"
#define COUPLE_STR_PAIR_ERR_WRONG "配对码不对"
#define COUPLE_STR_PAIR_ERR_ALREADY "已经配对过了"
#define COUPLE_STR_PAIR_ERR_BUSY "服务器有点忙"
#define COUPLE_STR_PAIR_ERR_NETWORK "连不上服务器"
#define COUPLE_STR_PAIR_ERR_GENERIC "这次没成功，再试一次"
#define COUPLE_STR_PAIR_EXIT_HINT "按住 OK 退出"

/* ---------- 解绑 ---------- */

#define COUPLE_STR_UNPAIRED_TOAST "已解除绑定"
#define COUPLE_STR_PEER_LEFT_TOAST "TA 解除了绑定"
#define COUPLE_STR_NEED_PAIR_TOAST "先和 TA 配对"

/* ---------- 想你（暂时送不出去，先收着） ---------- */

#define COUPLE_STR_MISS_QUEUED_TITLE "想你已收好"
#define COUPLE_STR_MISS_QUEUED_SUB "TA 一上线就送到"

/* ---------- 配网状态 ---------- */

#define COUPLE_STR_PROV_STARTING_HEADLINE "配网"
#define COUPLE_STR_PROV_STARTING_DETAIL "正在打开热点…"
#define COUPLE_STR_PROV_SAVED_HEADLINE "已保存"
#define COUPLE_STR_PROV_CONNECTING "正在连接 Wi-Fi…"
#define COUPLE_STR_PROV_DONE_HEADLINE "连上了"
#define COUPLE_STR_PROV_DONE_DETAIL "网络已经通了"
#define COUPLE_STR_PROV_FAIL_HEADLINE "没连上"
#define COUPLE_STR_PROV_FAIL_DETAIL "检查一下 Wi-Fi 密码"
#define COUPLE_STR_PROV_START_FAIL_TOAST "配网没能启动"
#define COUPLE_STR_PROV_EXIT_TOAST "已退出配网"

/* ---------- 系统提示 ---------- */

#define COUPLE_STR_NVS_RESET_TOAST "存储被重置过一次"

/* ---------- 随机彩蛋（收到想你时偶尔替换默认文案） ---------- */

#define COUPLE_STR_EGG_1 "有人偷偷想你啦。"
#define COUPLE_STR_EGG_2 "警告：检测到过量想念。"
#define COUPLE_STR_EGG_3 "对方申请抱抱。"
#define COUPLE_STR_EGG_4 "SYSTEM ERROR: PARTNER TOO CUTE"
#define COUPLE_STR_EGG_5 "心跳数据已同步。"
#define COUPLE_STR_EGG_6 "距离正在缩短中。"
#define COUPLE_STR_EGG_7 "本条消息含糖量超标。"
#define COUPLE_STR_EGG_8 "TA把想念藏进了这条消息。"
#define COUPLE_STR_EGG_BURST_FMT "今天第 %u 次想你，多少有点粘人了。"

#else /* !COUPLE_USE_APP_FONT —— 无中文字体，整界面退化成纯 ASCII 英文 */

/* ---------- 首页 ---------- */

#define COUPLE_STR_BRAND "OUR LINK"
#define COUPLE_STR_HOME_ENERGY_CAPTION "LOVE ENERGY"
#define COUPLE_STR_HOME_TOTAL_FMT "%u hearts"
#define COUPLE_STR_STATUS_ONLINE "Partner online"
#define COUPLE_STR_STATUS_OFFLINE "Partner offline"
#define COUPLE_STR_STATUS_UNPAIRED "Not paired yet"
#define COUPLE_STR_HOME_AGO_FMT "Last miss %s"
#define COUPLE_STR_HOME_HINT_MISS "[OK] Miss"
#define COUPLE_STR_HOME_HINT_HUG "Hold OK to hug"
#define COUPLE_STR_HOME_HINT_MORE "UP/DN more"
#define COUPLE_STR_BATTERY_FMT "%u%%"

/* ---------- 相对时间 ---------- */

#define COUPLE_STR_AGO_NOW "just now"
#define COUPLE_STR_AGO_MIN_FMT "%u min ago"
#define COUPLE_STR_AGO_HOUR_FMT "%u h ago"
#define COUPLE_STR_AGO_DAY_FMT "%u d ago"

/* ---------- 想你（发出） ---------- */

#define COUPLE_STR_MISS_SENT_TITLE "Miss delivered"
#define COUPLE_STR_MISS_SENT_SUB "LOVE ENERGY +1"
#define COUPLE_STR_MISS_SENT_HINT "Waiting for TA"

/* ---------- 想你（收到） ---------- */

#define COUPLE_STR_MISS_RECV_TITLE "TA misses you"
#define COUPLE_STR_MISS_RECV_AGO_FMT "Last miss %s"
#define COUPLE_STR_MISS_RECV_FLAVOR_DEFAULT "Thinking of you right now"
#define COUPLE_STR_MISS_RECV_HINT "Hold OK to hug back"

/* ---------- 抱抱 ---------- */

#define COUPLE_STR_HUG_HOLD_TITLE_IDLE "Holding..."
#define COUPLE_STR_HUG_HOLD_HINT "Release OK to send"
#define COUPLE_STR_HUG_HELD_SECONDS_FMT "Held %u.%u s"

#define COUPLE_STR_TIER_SMALL "Small hug"
#define COUPLE_STR_TIER_BIG "Big hug"
#define COUPLE_STR_TIER_CLINGY "Clingy hug"

#define COUPLE_STR_HUG_RECV_TITLE "TA hugged you"
#define COUPLE_STR_HUG_RECV_DURATION_FMT "TA held %u.%u s"
#define COUPLE_STR_HUG_RECV_HINT "Hold OK to hug back"
#define COUPLE_STR_HUG_SYNC_TITLE "HUG CONNECTED"
#define COUPLE_STR_HUG_SYNC_SUB "Both holding - LOVE ENERGY +5"
#define COUPLE_STR_HUG_SYNC_FOOTER "Don't let go"
#define COUPLE_STR_HEART_SYNC_TITLE "HEART SYNC"
#define COUPLE_STR_HEART_SYNC_SUB "Hearts in sync"
#define COUPLE_STR_HEART_SYNC_ENERGY "LOVE ENERGY +5"

/* ---------- 里程碑 ---------- */

#define COUPLE_STR_MILESTONE_BONUS_FMT "Bonus +%u energy"
#define COUPLE_STR_MILESTONE_ROMANTIC_520 "520 hearts, and none beats you."
#define COUPLE_STR_MILESTONE_ROMANTIC_1314 "A whole lifetime, starting here."
#define COUPLE_STR_MILESTONE_HINT "UP/DN more"
#define COUPLE_STR_MILESTONE_OOR_TOAST "The next milestone is ahead"

/* ---------- 每日统计 ---------- */

#define COUPLE_STR_DAILY_TITLE "Our day"
#define COUPLE_STR_DAILY_MISSES_LABEL "Misses today"
#define COUPLE_STR_DAILY_HUGS_LABEL "Hugs today"
#define COUPLE_STR_DAILY_FIRST_LABEL "First today"
#define COUPLE_STR_DAILY_STREAK_LABEL "Streak"
#define COUPLE_STR_DAILY_TIMES_FMT "%u times"
#define COUPLE_STR_DAILY_NONE "Not yet today"
#define COUPLE_STR_DAILY_STREAK_DAYS_FMT "%u days"
#define COUPLE_STR_DAILY_STREAK_ZERO "Starts today"
#define COUPLE_STR_DAILY_HINT "UP/DN more"

/* ---------- 菜单 ---------- */

#define COUPLE_STR_MENU_TITLE "Menu"
#define COUPLE_STR_MENU_ITEM_PAIR "Pair"
#define COUPLE_STR_MENU_ITEM_PROV "Wi-Fi setup"
#define COUPLE_STR_MENU_ITEM_UNPAIR "Unpair"
#define COUPLE_STR_MENU_HINT "[OK] pick  UP/DN move"

/* ---------- 配对 ---------- */

#define COUPLE_STR_PAIR_OFFER_TITLE "Give this code to TA"
#define COUPLE_STR_PAIR_TTL_FMT "%u s left"
#define COUPLE_STR_PAIR_OFFER_HINT "Read the code out loud"
#define COUPLE_STR_PAIR_ENTRY_TITLE "Enter TA's code"
#define COUPLE_STR_PAIR_ENTRY_HINT "UP/DN digit  [OK] next  hold OK exit"
#define COUPLE_STR_PAIR_CONFIRMED_TITLE "Paired"
#define COUPLE_STR_PAIR_CONFIRMED_HINT "Counting together from today"
#define COUPLE_STR_UNPAIR_TITLE "Unpair from TA?"
#define COUPLE_STR_UNPAIR_DETAIL "Energy and memories stay"
#define COUPLE_STR_UNPAIR_HINT "Hold OK to confirm"

/* ---------- 配网 ---------- */

#define COUPLE_STR_PROV_AP_TITLE "Join this Wi-Fi"
#define COUPLE_STR_PROV_SSID_FMT "SSID  %s"
#define COUPLE_STR_PROV_PASS_CAPTION "Password"
#define COUPLE_STR_PROV_IP_CAPTION "Open in phone browser"
#define COUPLE_STR_PROV_IP_FMT "http://%s"
#define COUPLE_STR_PROV_CLIENTS_FMT "%u device(s) connected"
#define COUPLE_STR_PROV_CLIENTS_WAITING "Waiting for device"
#define COUPLE_STR_PROV_HINT "Finish it on your phone"

/* ---------- 顶部横幅（对方离线期间的汇总） ---------- */

#define COUPLE_STR_OFFLINE_SUMMARY_FMT "While you were away, TA missed you %u times."

/* ---------- pairing flow progress (handed to the UI by couple_app) ---------- */

#define COUPLE_STR_PAIR_HEADLINE "Pair"
#define COUPLE_STR_PAIR_CHOICE_DETAIL "OK request a code  UP/DN enter TA's"
#define COUPLE_STR_PAIR_APPLY_HEADLINE "Pair code"
#define COUPLE_STR_PAIR_APPLY_DETAIL "Requesting..."
#define COUPLE_STR_PAIR_JOIN_HEADLINE "Pair"
#define COUPLE_STR_PAIR_JOIN_WAIT_DETAIL "Waiting for TA to enter it"
#define COUPLE_STR_PAIR_SUBMIT_HEADLINE "Confirming"
#define COUPLE_STR_PAIR_SUBMIT_DETAIL "Linking with TA..."
#define COUPLE_STR_PAIR_ERR_NOT_FOUND "No such pair code"
#define COUPLE_STR_PAIR_ERR_EXPIRED "This code expired"
#define COUPLE_STR_PAIR_ERR_WRONG "Wrong pair code"
#define COUPLE_STR_PAIR_ERR_ALREADY "Already paired"
#define COUPLE_STR_PAIR_ERR_BUSY "Server is busy"
#define COUPLE_STR_PAIR_ERR_NETWORK "Cannot reach server"
#define COUPLE_STR_PAIR_ERR_GENERIC "Not this time, try again"
#define COUPLE_STR_PAIR_EXIT_HINT "Hold OK to exit"

/* ---------- unbinding ---------- */

#define COUPLE_STR_UNPAIRED_TOAST "Unpaired"
#define COUPLE_STR_PEER_LEFT_TOAST "TA unpaired"
#define COUPLE_STR_NEED_PAIR_TOAST "Pair with TA first"

/* ---------- miss you (cannot be delivered just yet, kept safe) ---------- */

#define COUPLE_STR_MISS_QUEUED_TITLE "Miss kept safe"
#define COUPLE_STR_MISS_QUEUED_SUB "Delivered once TA is back"

/* ---------- provisioning status ---------- */

#define COUPLE_STR_PROV_STARTING_HEADLINE "Setup"
#define COUPLE_STR_PROV_STARTING_DETAIL "Starting hotspot..."
#define COUPLE_STR_PROV_SAVED_HEADLINE "Saved"
#define COUPLE_STR_PROV_CONNECTING "Connecting to Wi-Fi..."
#define COUPLE_STR_PROV_DONE_HEADLINE "Connected"
#define COUPLE_STR_PROV_DONE_DETAIL "Network is up"
#define COUPLE_STR_PROV_FAIL_HEADLINE "No connection"
#define COUPLE_STR_PROV_FAIL_DETAIL "Check the Wi-Fi password"
#define COUPLE_STR_PROV_START_FAIL_TOAST "Setup did not start"
#define COUPLE_STR_PROV_EXIT_TOAST "Setup closed"

/* ---------- system notices ---------- */

#define COUPLE_STR_NVS_RESET_TOAST "Storage was reset"

/* ---------- random flavor (occasionally replaces the default miss text) ---------- */

#define COUPLE_STR_EGG_1 "Someone is missing you in secret."
#define COUPLE_STR_EGG_2 "Warning: too much missing detected."
#define COUPLE_STR_EGG_3 "TA is requesting a hug."
#define COUPLE_STR_EGG_4 "SYSTEM ERROR: PARTNER TOO CUTE"
#define COUPLE_STR_EGG_5 "Heartbeat data synced."
#define COUPLE_STR_EGG_6 "Distance is shrinking."
#define COUPLE_STR_EGG_7 "Sugar level over the limit."
#define COUPLE_STR_EGG_8 "TA hid a miss in this message."
#define COUPLE_STR_EGG_BURST_FMT "Miss #%u today, a bit clingy."

#endif /* COUPLE_USE_APP_FONT */