/*
 * Heartlink 情侣心动终端 —— 界面层实现（纯渲染器）。
 *
 * 这个文件里没有任何输入、网络、NVS、音频：所有内容都由 couple_app 通过
 * couple_ui.h 的 push_* / show_* 推过来。好处是 LVGL 回调里永远不会有慢操作，
 * 而且 couple_app 可以脱离 LVGL 做宿主测试。
 *
 * 视觉语言（像素掌机风）：
 *   - 不用任何位图。心形、圆环、进度条、光标全部由矩形对象拼出来，
 *     所以不占 Flash，也不依赖字体里的特殊字形（↑ ↓ · 这些一律不进英文文案）。
 *   - 深紫底 + 粉/薄荷/琥珀三色点缀，屏角圆角 30px 由 BSP 的面板层负责，
 *     这里只保证内容落在 8..312 的安全区里。
 *
 * 字体：所有字号都走 couple_fonts.h 的别名；是否显示中文由 COUPLE_USE_APP_FONT
 * 编译开关决定，文案来自 couple_ui_strings.h。
 *
 * 页面生命周期（懒构建 + 离开即销毁）：couple_ui_init() 只建首页和顶部横幅；其余
 * 11 页首次进入时才创建，离开该页时立即销毁，下次进入再由 screen_ensure() 重建。
 * 只有首页常驻（每秒的 tick 都要刷它）。这不是优化而是必须的：LVGL 用的是自己的
 * 静态内存池（CONFIG_LV_MEM_SIZE_KILOBYTES，与系统堆完全分开），12 页建完要
 * 49,096 B，加上 6,348 B 基线共 55,444 B，超过 48 KB 的池；而稳态下同时驻留的
 * 只有首页 + 当前页（约 19.4 KB）。详见 screen_ensure() 与 page_release()。
 *
 * 线程模型：每个公开函数自己拿 bsp_lvgl_lock()；本文件内部的 static 辅助函数
 * 一律假设调用方已经持锁，名字里不带锁语义，只靠这个约定。
 */

#include "couple_ui.h"

#include "couple_config.h"
#include "couple_fonts.h"
#include "couple_state.h"
#include "couple_ui_strings.h"

#include "bsp_display.h"

#include "lvgl.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

/* ---------- 常量 ---------- */

enum {
    kLockTimeoutMs = 200, /* 与 couple_ui.h 的注释一致：任何调用最多等 200ms */
    kScreenW = 240,
    kScreenH = 320,
    kSafeX = 8,
    kMsgWidth = kScreenW - 2 * kSafeX, /* 居中长文案的宽度 */
    kMenuCount = 3,
    kPairDigits = (int)COUPLE_PAIR_CODE_LEN,
    kHeartCols = 11,
    kHeartRows = 10,
    kHugBarX = 24,
    kHugBarY = 214,
    kHugBarW = 192,
    kHugBarH = 8,
    kMenBarX = 22,
    kMenBarW = 196,
    kMenBarH = 40,
    kMenBarY0 = 80,
    kMenBarDY = 50,
    kBatteryX = 200,
    kBatteryY = 16,
    kBatteryW = 24,
    kBatteryH = 12,

    /* LVGL 池低内存守卫的阈值，见 screen_ensure()。
     * free_biggest_size 是「池里最大的那块连续空闲」：圆角掩码这类整块分配失败时
     * lv_malloc 返回 NULL，紧接着就是 LV_ASSERT_HANDLER 的 while(1)，整个 LVGL
     * 任务永久挂住。free_size 用来兜住「碎到总量也不够」的情况。 */
    kUiMinFreeBlock = 4096,
    kUiMinFreeTotal = 6144,

    /* 池余量周期打点的间隔（tick_cb 是 1 Hz，所以这里就是秒数）。 */
    kMemLogEveryTicks = 10,
};

/* 唤醒后恢复的背光亮度：与 main.c 开机时设的值一致。 */
static const uint8_t kBacklightOn = 100;

/* ---------- 配色 ----------
 *
 * 用宏而不是 const 变量：const 变量在只被一个分支引用时会触发
 * -Wunused-const-variable，而宏不会。lv_color_hex() 是内联函数，可以在
 * 任何用到颜色的地方直接展开。
 */

#define COL_BG lv_color_hex(0x0E0B14)        /* 背景深紫 */
#define COL_PANEL lv_color_hex(0x171226)     /* 卡片/输入格 */
#define COL_LINE lv_color_hex(0x2A2140)      /* 分隔线、描边 */
#define COL_PINK lv_color_hex(0xFF6B9D)      /* 主粉 */
#define COL_PINK_SOFT lv_color_hex(0xFFA6C4) /* 浅粉：文案强调 */
#define COL_PINK_DEEP lv_color_hex(0xC9406F) /* 深粉：光晕、底色 */
#define COL_CREAM lv_color_hex(0xF6E7E9)     /* 主文字 */
#define COL_MUTED lv_color_hex(0x8E86A8)     /* 次要文字 */
#define COL_MINT lv_color_hex(0x7BE3C0)      /* 成功/在线/电量充足 */
#define COL_AMBER lv_color_hex(0xFFC46B)     /* 电量偏低 */

/* 心形像素图：11 x 10，每行把连续像素合并成矩形，共 10 个矩形。
 *
 * 原始位图（# = 实心）：
 *     .##.....##.     .####.####.     ###########     ###########     ###########
 *     .#########.     ..#######..     ...#####...     ....###....     .....#.....
 * 第 2..4 行完全相同，因此在这里合并成一条 3 单位高的色带。
 */
typedef struct {
    uint8_t y;      /* 起始行（单位格） */
    uint8_t h;      /* 高度（单位格） */
    uint8_t count;  /* 本行矩形数 */
    uint8_t x0[2];  /* 各矩形左边界（含） */
    uint8_t x1[2];  /* 各矩形右边界（含） */
} heart_band_t;

static const heart_band_t kHeartBands[] = {
    {0, 1, 2, {1, 8}, {2, 9}},
    {1, 1, 2, {1, 6}, {4, 9}},
    {2, 3, 1, {0, 0}, {10, 0}},
    {5, 1, 1, {1, 0}, {9, 0}},
    {6, 1, 1, {2, 0}, {8, 0}},
    {7, 1, 1, {3, 0}, {7, 0}},
    {8, 1, 1, {4, 0}, {6, 0}},
    {9, 1, 1, {5, 0}, {5, 0}},
};

/* ---------- 常驻状态 ---------- */

static const char *TAG = "couple_ui";

static bool s_inited;
static bool s_status_pushed; /* 首页是否已经被推过一次 */
/* 背光是否已熄灭。只在 app 任务与 LVGL 任务之间传一个字节，不加锁：
 * 最坏情况是一次多余的 ledc 写入，不会碰到任何对象。 */
static volatile bool s_sleeping;
static couple_view_t s_view = COUPLE_VIEW_HOME;
static couple_ui_status_t s_status; /* 最近一次 push 的快照 */
static couple_hug_tier_t s_tier = COUPLE_HUG_TIER_NONE; /* 抱抱页当前档位 */
static lv_timer_t *s_tick;        /* 每秒刷新（相对时间、电量） */
static lv_timer_t *s_toast_timer; /* 横幅停留计时 */

static struct {
    lv_obj_t *scr;
    lv_obj_t *frame;
    lv_obj_t *fill;
    lv_obj_t *text;
    lv_obj_t *status;
    lv_obj_t *name;
    lv_obj_t *ago;
    lv_obj_t *energy;
    lv_obj_t *total;
    lv_obj_t *heart;
} s_home;

static lv_obj_t *s_miss_sent_scr;

static struct {
    lv_obj_t *scr;
    lv_obj_t *heart;
    lv_obj_t *ago;
    lv_obj_t *flavor;
} s_miss_recv;

static struct {
    lv_obj_t *scr;
    lv_obj_t *title;
    lv_obj_t *bar_fill;
    lv_obj_t *secs;
    lv_obj_t *heart;
    int bar_w;          /* 已设过的进度条宽度，避免 20Hz 重复布局 */
    unsigned secs_key;  /* 已显示的「十分之一秒」值，避免重复格式化 */
} s_hug_hold;

static struct {
    lv_obj_t *scr;
    lv_obj_t *heart_a;
    lv_obj_t *heart_b;
    lv_obj_t *duration;
    lv_obj_t *tier;
} s_hug_recv;

static struct {
    lv_obj_t *scr;
    lv_obj_t *ring_a;
    lv_obj_t *ring_b;
    lv_obj_t *heart_a;
    lv_obj_t *heart_b;
} s_hug_sync;

static struct {
    lv_obj_t *scr;
    lv_obj_t *ring;
    lv_obj_t *heart;
} s_heart_sync;

static struct {
    lv_obj_t *scr;
    lv_obj_t *ring;
    lv_obj_t *value;
    lv_obj_t *title;
    lv_obj_t *bar;
    lv_obj_t *heart_a;
    lv_obj_t *heart_b;
    lv_obj_t *bonus;
    lv_obj_t *romantic;
    lv_obj_t *hint;
} s_ms;

static struct {
    lv_obj_t *scr;
    lv_obj_t *v_miss;
    lv_obj_t *v_hug;
    lv_obj_t *v_first;
    lv_obj_t *v_streak;
} s_daily;

static struct {
    lv_obj_t *scr;
    lv_obj_t *item[kMenuCount];
    lv_obj_t *text[kMenuCount];
} s_menu;

static struct {
    lv_obj_t *scr;
    lv_obj_t *dot;
    lv_obj_t *title;
    lv_obj_t *detail;
    lv_obj_t *ttl;
    lv_obj_t *hint;
    lv_obj_t *name;
    lv_obj_t *heart;
    lv_obj_t *cell[kPairDigits];
    lv_obj_t *digit[kPairDigits];
    lv_obj_t *cursor_cell; /* 正在闪描边的格子 */
} s_pair;

static struct {
    lv_obj_t *scr;
    lv_obj_t *dot;
    lv_obj_t *title;
    lv_obj_t *detail;
    lv_obj_t *ssid;
    lv_obj_t *pass_cap;
    lv_obj_t *pass_a;
    lv_obj_t *pass_b;
    lv_obj_t *ip_cap;
    lv_obj_t *ip;
    lv_obj_t *clients;
    lv_obj_t *hint;
} s_prov;

static struct {
    lv_obj_t *box;
    lv_obj_t *text;
} s_toast;

/* ---------- 缩放注册表 ----------
 *
 * 这台设备上不能用 LVGL 的样式缩放属性：只要缩放值 != 256，LVGL 就把对象升级成
 * LV_LAYER_TYPE_TRANSFORM，在 lv_obj_refr() 里为「整个缩放后的包围盒」一次性开一块
 * 4 字节/像素的 layer。150px 的圆环放大到 300% 就是 450x450x4 ≈ 810KB，而 LVGL 的
 * 堆只有几十 KB；分配失败在 CONFIG_LV_USE_ASSERT_MALLOC 下直接断言重启。
 *
 * 所以缩放一律改真实几何：圆环改尺寸、圆心不动；像素心改单位格边长。
 * 调用点的签名和语义不变，只是实现换成「查表 + 重排几何」。
 */
enum {
    /* 稳态下同时驻留的只有首页 + 当前页：最多是首页的 1 颗心加 hug_sync 的
     * 2 环 + 2 心（共 5 个），所以 20 个槽位是很宽的余量。页面离开即销毁，
     * 它登记的对象由 scalables_drop_owner() 一并摘掉，不会越用越多。 */
    kScalableSlots = 20,
    kRingPulsePeak = 165, /* 圆环脉冲峰值（%）：小环放大到这么大 */
    /* 单帧重绘面积上限：环的包围盒不超过屏幕的 60%（240x320 的 60% = 46080 px²）。
     * 脉冲每帧都要重绘整个包围盒，300% 那种整屏重绘 SPI 带宽撑不住。 */
    kRingPulseMaxArea = kScreenW * kScreenH * 6 / 10,
};

enum { SC_KIND_NONE = 0, SC_KIND_HEART, SC_KIND_CIRCLE };

typedef struct {
    lv_obj_t *obj;
    uint8_t kind;
    int16_t base;   /* CIRCLE: 直径; HEART: 单位格边长 */
    int16_t cx, cy; /* CIRCLE: 屏幕坐标里的圆心（缩放时保持不动）；HEART 不用 */
    uint8_t owner;  /* 登记时正在构建的页面（couple_view_t），用于页面释放时整批摘除 */
} ui_scalable_t;

/*
 * 「不属于任何会被释放的页面」的哨兵归属：首页与 toast 在 couple_ui_init() 里直接
 * 构建，登记的对象永远不会被回收。
 */
#define kOwnerNone ((couple_view_t)0xFF)

static ui_scalable_t s_scalables[kScalableSlots];
static int s_scalable_count;

/* 只在 page_build() 的 *_build() 调用期间有效，其余时间恒为 kOwnerNone。 */
static couple_view_t s_build_owner = kOwnerNone;

/* 表满就静默降级成「这个对象不缩放」：动画回调里既不能断言也不能打日志。 */
static void scalable_add(lv_obj_t *obj, uint8_t kind, int base, int cx, int cy)
{
    ui_scalable_t *slot = NULL;

    if (obj == NULL || base <= 0) return;

    for (int i = 0; i < s_scalable_count; i++) {
        if (s_scalables[i].obj == obj) {
            slot = &s_scalables[i];
            break;
        }
    }

    if (slot == NULL) {
        if (s_scalable_count >= kScalableSlots) return;
        slot = &s_scalables[s_scalable_count];
        s_scalable_count++;
    }

    slot->obj = obj;
    slot->kind = kind;
    slot->base = (int16_t)base;
    slot->cx = (int16_t)cx;
    slot->cy = (int16_t)cy;
    slot->owner = (uint8_t)s_build_owner;
}

/*
 * 页面被销毁时把它的登记整批摘掉：否则表里留下指向已释放 lv_obj 的悬垂记录，
 * 下次重建时新对象地址不同、旧记录不会被覆盖，表满（kScalableSlots）之后新对象
 * 会被静默丢弃，缩放动画就悄悄失效了。kOwnerNone 的条目永不删除。
 */
static void scalables_drop_owner(couple_view_t view)
{
    int keep = 0;

    for (int i = 0; i < s_scalable_count; i++) {
        if (s_scalables[i].owner == (uint8_t)view) continue;
        if (keep != i) s_scalables[keep] = s_scalables[i];
        keep++;
    }
    s_scalable_count = keep;
}

static ui_scalable_t *scalable_find(lv_obj_t *obj)
{
    if (obj == NULL) return NULL;

    for (int i = 0; i < s_scalable_count; i++) {
        if (s_scalables[i].obj == obj) return &s_scalables[i];
    }
    return NULL;
}

/* ---------- 基础构件 ---------- */

static lv_obj_t *ui_screen_new(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(scr, kScreenW, kScreenH);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    return scr;
}

static lv_obj_t *ui_box(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t color, lv_opa_t opa,
                        int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

static lv_obj_t *ui_frame(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t color, int border_px,
                          int radius)
{
    lv_obj_t *o = ui_box(parent, x, y, w, h, color, LV_OPA_TRANSP, radius);
    lv_obj_set_style_border_color(o, color, 0);
    lv_obj_set_style_border_width(o, border_px, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    return o;
}

/* 底层氛围光：一个很大的半透明圆，先建，保证永远在最下层。 */
static void ui_glow(lv_obj_t *parent, int cx, int cy, int d, lv_color_t color)
{
    ui_box(parent, cx - d / 2, cy - d / 2, d, d, color, LV_OPA_10, LV_RADIUS_CIRCLE);
}

static lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text,
                          int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* 整屏宽度、水平居中的一行文案（可以换行，宽度固定在安全区内）。 */
static lv_obj_t *ui_label_mid(lv_obj_t *parent, int y, const lv_font_t *font, lv_color_t color,
                              const char *text)
{
    lv_obj_t *l = ui_label(parent, font, color, text, 0, y);
    lv_obj_set_width(l, kMsgWidth);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static lv_obj_t *ui_label_right(lv_obj_t *parent, int x_from_right, int y, const lv_font_t *font,
                                lv_color_t color, const char *text)
{
    lv_obj_t *l = ui_label(parent, font, color, text, 0, y);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -x_from_right, y);
    return l;
}

/* 同心圆环：只有描边，用来做脉冲。 */
static lv_obj_t *ui_ring(lv_obj_t *parent, int cx, int cy, int d, lv_color_t color, int border_px)
{
    lv_obj_t *o = ui_frame(parent, cx - d / 2, cy - d / 2, d, d, color, border_px, LV_RADIUS_CIRCLE);
    /* 缩放靠改尺寸 + 反推左上角，所以圆心要记下来。 */
    scalable_add(o, SC_KIND_CIRCLE, d, cx, cy);
    return o;
}

/* 像素心：返回容器（缩放动画作用在容器上），单位格边长 unit_px。 */
static lv_obj_t *heart_build(lv_obj_t *parent, int unit_px, lv_color_t color)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(cont, kHeartCols * unit_px, kHeartRows * unit_px);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);

    for (size_t b = 0; b < sizeof(kHeartBands) / sizeof(kHeartBands[0]); b++) {
        const heart_band_t *band = &kHeartBands[b];
        for (uint8_t i = 0; i < band->count; i++) {
            ui_box(cont, band->x0[i] * unit_px, band->y * unit_px,
                   (band->x1[i] - band->x0[i] + 1) * unit_px, band->h * unit_px, color, LV_OPA_COVER,
                   0);
        }
    }
    /* 缩放时容器自己改尺寸并保持中心不动，中心是每帧现算的，这里不用记；
     * 登记只是为了 base（初始格边长）和 kind，cx/cy 对 HEART 不使用。 */
    scalable_add(cont, SC_KIND_HEART, unit_px, 0, 0);
    return cont;
}

/* ---------- 动画 ---------- */

/*
 * 像素心按 unit 像素一格重铺。容器跟着改尺寸，但先算出当前视觉中心再反推左上角，
 * 所以中心钉在原地。子矩形用容器自身坐标铺满，永远不越出容器边界 —— 也不能指望
 * LV_OBJ_FLAG_OVERFLOW_VISIBLE 兜底：它靠「扩展绘制尺寸」，而该值只统计阴影、
 * 外描边和变换宽度，lv_obj_remove_style_all() 之后的容器是 0，超出的子对象照样被裁。
 *
 * 位置和尺寸一律读 style 值（lv_obj_set_pos / lv_obj_set_size 是同步写 local style 的），
 * 不用 coords：coords 要等下一次 layout 才更新，而动画回调可能在 layout 之前就跑起来，
 * 那时读到的是创建时的默认几何，中心一旦算错就再也纠不回来。style 值是本函数的输出，
 * 读回来的中心恒等于上一次写进去的中心（x + w/2 在整数下是不动点），所以不会累积漂移。
 */
static void heart_apply_unit(lv_obj_t *cont, int unit)
{
    int old_w = lv_obj_get_style_width(cont, LV_PART_MAIN);
    int old_h = lv_obj_get_style_height(cont, LV_PART_MAIN);
    int cx = lv_obj_get_x_aligned(cont) + old_w / 2;
    int cy = lv_obj_get_y_aligned(cont) + old_h / 2;
    int w = kHeartCols * unit;
    int h = kHeartRows * unit;
    size_t idx = 0;

    lv_obj_set_size(cont, w, h);
    lv_obj_set_pos(cont, cx - w / 2, cy - h / 2);

    /* 子对象就是 heart_build() 按 band 顺序建的那些，创建序 == child 序。 */
    for (size_t b = 0; b < sizeof(kHeartBands) / sizeof(kHeartBands[0]); b++) {
        const heart_band_t *band = &kHeartBands[b];
        for (uint8_t i = 0; i < band->count; i++) {
            lv_obj_t *child = lv_obj_get_child(cont, (int32_t)idx);
            idx++;
            if (child == NULL) return;
            lv_obj_set_size(child, (band->x1[i] - band->x0[i] + 1) * unit, band->h * unit);
            lv_obj_set_pos(child, band->x0[i] * unit, band->y * unit);
        }
    }
}

/* 百分比缩放统一入口：几何重排，没有 transform，也就不会产生 layer。 */
static void set_scale_percent(lv_obj_t *obj, int32_t percent)
{
    ui_scalable_t *e = scalable_find(obj);

    if (e == NULL) return; /* 未登记（例如 label）：静默不动，绝不崩 */
    if (percent < 1) percent = 1;

    if (e->kind == SC_KIND_CIRCLE) {
        int d = (int)((e->base * percent) / 100);
        if (d < 2) d = 2;
        lv_obj_set_size(obj, d, d);
        lv_obj_set_pos(obj, e->cx - d / 2, e->cy - d / 2);
        return;
    }

    if (e->kind == SC_KIND_HEART) {
        int unit = (int)((e->base * percent + 50) / 100);
        if (unit < 1) unit = 1;
        heart_apply_unit(obj, unit);
    }
}

static void anim_scale_percent(void *var, int32_t percent)
{
    set_scale_percent((lv_obj_t *)var, percent);
}

/*
 * 首页主心形的心跳波形。
 *
 * 为什么不用 anim_run 的 from/to + ease 曲线：像素心的档位是按整数格算的，首页心形
 * base = 8px，percent 只有落进 82..93 / 94..106 / 107..118 三段才分别对应 7 / 8 / 9 格，
 * 其余区间四舍五入后完全静止。原来那版「92..108 往返 1.6s」实际是 3.2 秒一个来回的
 * 缓慢呼吸，只在两端各蹭到一档，所以看起来不像心跳。
 *
 * 现在改成：时间轴交给 lv_anim_path_linear（波形由我们自己给，ease 曲线会把它拉变形），
 * percent 由下面的关键帧表按分段线性插值算出，做真正的「lub-dub」双拍。
 * 首尾关键帧同为 100（8 格，与 set_scale_percent 的复位值一致），
 * 所以 LV_ANIM_REPEAT_INFINITE 绕回起点时看不出接缝。
 */
#define HOME_BEAT_PERIOD_MS 1000

typedef struct {
    uint16_t t;      /* 距周期起点的毫秒数 */
    int16_t percent; /* 该时刻的缩放百分比 */
} home_beat_key_t;

static const home_beat_key_t kHomeBeatKeys[] = {
    {0, 100},    /* 静息 */
    {60, 100},   /* 短停：让循环的接缝落在静止帧上，不要停在半途 */
    {140, 116},  /* 第一拍「lub」：70ms 内胀开，要快要狠 */
    {250, 116},  /* 保持 110ms —— 峰值停得住才像一次搏动，一路滑过去就只是呼吸 */
    {340, 84},   /* 越过静息继续收紧，做出「泵」的顿挫 */
    {430, 84},   /* 舒张相短暂保持 */
    {560, 114},  /* 第二拍「dub」：幅度略小、过程更短，跟第一拍区分开 */
    {660, 114},  /* 保持 100ms */
    {780, 100},  /* 回到静息 */
    {1000, 100}, /* 长静息，收尾 */
};

static int32_t home_beat_percent(int32_t t)
{
    const size_t count = sizeof(kHomeBeatKeys) / sizeof(kHomeBeatKeys[0]);
    int32_t prev_t = (int32_t)kHomeBeatKeys[0].t;
    int32_t prev_p = (int32_t)kHomeBeatKeys[0].percent;

    for (size_t i = 1; i < count; i++) {
        int32_t t1 = (int32_t)kHomeBeatKeys[i].t;
        int32_t p1 = (int32_t)kHomeBeatKeys[i].percent;

        if (t <= t1) {
            int32_t span = t1 - prev_t;
            if (span <= 0) return p1;
            return prev_p + ((p1 - prev_p) * (t - prev_t)) / span;
        }
        prev_t = t1;
        prev_p = p1;
    }
    return prev_p; /* 越过最后一帧就停在结束值，不外推 */
}

static void anim_home_beat(void *var, int32_t progress)
{
    set_scale_percent((lv_obj_t *)var, home_beat_percent(progress));
}

/*
 * 脉冲峰值：环越大，能放大的余量越小 —— 每帧要重绘整个包围盒，所以峰值取
 * 「不超过 kRingPulsePeak 且包围盒（按屏幕裁剪后）不超过屏幕 60%」的最大档位。
 * 200px 的环 100% 时就已占掉屏幕一半，只能涨到 105%，脉冲主要靠 opa 淡出读出来。
 */
static int32_t ring_pulse_peak(const ui_scalable_t *e)
{
    for (int32_t p = kRingPulsePeak; p > 100; p -= 5) {
        int d = (e->base * p) / 100;
        int x0 = e->cx - d / 2;
        int y0 = e->cy - d / 2;
        int x1 = x0 + d;
        int y1 = y0 + d;

        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > kScreenW) x1 = kScreenW;
        if (y1 > kScreenH) y1 = kScreenH;
        if (x1 > x0 && y1 > y0 && (x1 - x0) * (y1 - y0) <= kRingPulseMaxArea) return p;
    }
    return 100;
}

/*
 * 圆环脉冲：progress 0..100 同时放大并淡出，然后重头再来。
 * 放大是改真实尺寸 + 反推左上角（圆心不动），褪色才是主效果。
 */
static void anim_ring_pulse(void *var, int32_t progress)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    ui_scalable_t *e = scalable_find(obj);

    if (e != NULL) {
        int32_t peak = ring_pulse_peak(e);
        set_scale_percent(obj, 100 + (progress * (peak - 100)) / 100);
    }
    lv_obj_set_style_opa(obj, (lv_opa_t)((255 * (100 - progress)) / 100), 0);
}

/* 描边呼吸：配对页当前输入格的提示。 */
static void anim_set_border_opa(void *var, int32_t value)
{
    lv_obj_set_style_border_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

/*
 * 透明度强调：给无法改几何的对象（label）用。普通 opa 不参与 calculate_layer_type()
 * 的判断，不会产生 layer，所以这条路是安全的。路径曲线可能给出区间外的值，钳一下。
 */
static void anim_opa_emphasis(void *var, int32_t value)
{
    if (value < LV_OPA_TRANSP) value = LV_OPA_TRANSP;
    if (value > LV_OPA_COVER) value = LV_OPA_COVER;
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

/*
 * 统一入口：先删掉同一个对象上同一个 exec_cb 的旧动画，避免叠加。
 * reverse_ms != 0 时往返播放；repeat_count = LV_ANIM_REPEAT_INFINITE 时循环。
 */
static void anim_run(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t dur_ms,
                     uint32_t delay_ms, uint32_t reverse_ms, uint32_t repeat_count,
                     uint32_t repeat_delay_ms, lv_anim_path_cb_t path)
{
    lv_anim_delete(obj, exec);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, dur_ms);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_path_cb(&a, path);
    if (reverse_ms > 0) lv_anim_set_reverse_duration(&a, reverse_ms);
    if (repeat_count > 0) {
        lv_anim_set_repeat_count(&a, repeat_count);
        lv_anim_set_repeat_delay(&a, repeat_delay_ms);
    }
    lv_anim_start(&a);
}

/*
 * 迷你弹跳：从 (目标 + 幅度) 开始，越过目标再回落。
 * 比「从小到大」更像「跳一下」，用于档位变化这种需要强调的瞬间。
 */
static void anim_pop(lv_obj_t *obj, int32_t target, int32_t amount, uint32_t dur_ms)
{
    anim_run(obj, anim_scale_percent, target + amount, target, dur_ms, 0, 0, 0, 0, lv_anim_path_overshoot);
}

/* ---------- 文案（跨 couple_state 的中文硬编码做兼容） ---------- */

/*
 * 抱抱档位标签。
 * 接入中文字体时用 couple_state.h 的 couple_hug_tier_label()（唯一权威）；
 * 纯英文构建下那个函数只会返回中文，所以走本地的英文表。
 */
static const char *hug_tier_text(couple_hug_tier_t tier)
{
    if (tier == COUPLE_HUG_TIER_NONE) return COUPLE_STR_HUG_HOLD_TITLE_IDLE;
#ifdef COUPLE_USE_APP_FONT
    return couple_hug_tier_label(tier);
#else
    switch (tier) {
    case COUPLE_HUG_TIER_SMALL:
        return COUPLE_STR_TIER_SMALL;
    case COUPLE_HUG_TIER_BIG:
        return COUPLE_STR_TIER_BIG;
    case COUPLE_HUG_TIER_CLINGY:
        return COUPLE_STR_TIER_CLINGY;
    default:
        return COUPLE_STR_HUG_HOLD_TITLE_IDLE;
    }
#endif
}

/* couple_format_ago() 只有中文文案；英文构建下由界面自己拼。 */
static void ago_text(char *out, size_t cap, uint32_t delta_sec)
{
    if (cap == 0) return;
#ifdef COUPLE_USE_APP_FONT
    if (couple_format_ago(out, cap, delta_sec) == 0) out[0] = '\0';
#else
    if (delta_sec < COUPLE_RELATIVE_JUST_NOW_SEC)
        snprintf(out, cap, "%s", COUPLE_STR_AGO_NOW);
    else if (delta_sec < 3600u)
        snprintf(out, cap, COUPLE_STR_AGO_MIN_FMT, (unsigned)(delta_sec / 60u));
    else if (delta_sec < 86400u)
        snprintf(out, cap, COUPLE_STR_AGO_HOUR_FMT, (unsigned)(delta_sec / 3600u));
    else
        snprintf(out, cap, COUPLE_STR_AGO_DAY_FMT, (unsigned)(delta_sec / 86400u));
#endif
}

/* 同理：couple_format_offline_summary() 也是中文专用。 */
static void offline_text(char *out, size_t cap, uint16_t miss_count)
{
    if (cap == 0) return;
#ifdef COUPLE_USE_APP_FONT
    if (couple_format_offline_summary(out, cap, miss_count) == 0) out[0] = '\0';
#else
    snprintf(out, cap, COUPLE_STR_OFFLINE_SUMMARY_FMT, (unsigned)miss_count);
#endif
}

/* ---------- 局部刷新 ---------- */

static void battery_refresh(void)
{
    int pct = s_status.battery_percent;

    if (!s_status_pushed || pct < 0) {
        /* 读不到电量：只留一个空描边，不要假装是 0%。 */
        lv_obj_add_flag(s_home.fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_home.text, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (pct > 100) pct = 100;

    int inner = kBatteryW - 4; /* 描边 2px */
    int w = (inner * pct) / 100;
    if (pct > 0 && w < 2) w = 2;

    lv_obj_set_width(s_home.fill, w);
    lv_obj_set_style_bg_color(s_home.fill,
                              pct > 60 ? COL_MINT : (pct >= 30 ? COL_AMBER : COL_PINK), 0);
    if (w > 0) lv_obj_remove_flag(s_home.fill, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_home.fill, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text_fmt(s_home.text, COUPLE_STR_BATTERY_FMT, (unsigned)pct);
    if (s_status_pushed) lv_obj_remove_flag(s_home.text, LV_OBJ_FLAG_HIDDEN);
}

static void home_refresh(void)
{
    /* 首页还没建（tick 定时器与 push_status 都可能先于首次进入就跑到）时什么都不碰。 */
    if (s_home.scr == NULL) return;

    battery_refresh();

    if (!s_status.paired) {
        lv_label_set_text(s_home.status, COUPLE_STR_STATUS_UNPAIRED);
        lv_obj_set_style_text_color(s_home.status, COL_MUTED, 0);
    } else if (s_status.partner_online) {
        lv_label_set_text(s_home.status, COUPLE_STR_STATUS_ONLINE);
        lv_obj_set_style_text_color(s_home.status, COL_MINT, 0);
    } else {
        lv_label_set_text(s_home.status, COUPLE_STR_STATUS_OFFLINE);
        lv_obj_set_style_text_color(s_home.status, COL_MUTED, 0);
    }

    if (s_status.partner_known && s_status.partner_name[0] != '\0') {
        lv_label_set_text(s_home.name, s_status.partner_name);
        lv_obj_remove_flag(s_home.name, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_home.name, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_status.last_miss_received_utc != 0 && s_status.now_utc != 0) {
        char ago[48];
        ago_text(ago, sizeof ago,
                 couple_seconds_since(s_status.now_utc, s_status.last_miss_received_utc));
        lv_label_set_text_fmt(s_home.ago, COUPLE_STR_HOME_AGO_FMT, ago);
        lv_obj_remove_flag(s_home.ago, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_home.ago, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text_fmt(s_home.energy, "%04u", (unsigned)s_status.energy);
    lv_label_set_text_fmt(s_home.total, COUPLE_STR_HOME_TOTAL_FMT,
                          (unsigned)s_status.total_interactions);
}

static void miss_recv_refresh(void)
{
    /* 未建页时全部指针都是 NULL：直接返回，等这页真的被打开时再刷。 */
    if (s_miss_recv.scr == NULL) return;

    if (s_status.last_miss_received_utc != 0 && s_status.now_utc != 0) {
        char ago[48];
        ago_text(ago, sizeof ago,
                 couple_seconds_since(s_status.now_utc, s_status.last_miss_received_utc));
        lv_label_set_text_fmt(s_miss_recv.ago, COUPLE_STR_MISS_RECV_AGO_FMT, ago);
        lv_obj_remove_flag(s_miss_recv.ago, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* 还没对时或还没收到过：不显示这一行，而不是显示 "0 分钟前"。 */
        lv_obj_add_flag(s_miss_recv.ago, LV_OBJ_FLAG_HIDDEN);
    }
}

static void daily_refresh(void)
{
    /* 未建页时直接返回，理由同 miss_recv_refresh()。 */
    if (s_daily.scr == NULL) return;

    lv_label_set_text_fmt(s_daily.v_miss, COUPLE_STR_DAILY_TIMES_FMT, (unsigned)s_status.day_misses);
    lv_label_set_text_fmt(s_daily.v_hug, COUPLE_STR_DAILY_TIMES_FMT, (unsigned)s_status.day_hugs);

    if (s_status.day_first_interaction_utc == 0 || s_status.now_utc == 0) {
        lv_label_set_text(s_daily.v_first, COUPLE_STR_DAILY_NONE);
    } else {
        char hhmm[16];
        if (couple_format_hhmm(hhmm, sizeof hhmm, s_status.day_first_interaction_utc,
                               COUPLE_UTC_OFFSET_MINUTES) > 0)
            lv_label_set_text(s_daily.v_first, hhmm);
        else
            lv_label_set_text(s_daily.v_first, COUPLE_STR_DAILY_NONE);
    }

    if (s_status.streak_days == 0)
        lv_label_set_text(s_daily.v_streak, COUPLE_STR_DAILY_STREAK_ZERO);
    else
        lv_label_set_text_fmt(s_daily.v_streak, COUPLE_STR_DAILY_STREAK_DAYS_FMT,
                              (unsigned)s_status.streak_days);
}

/* ---------- 抱抱进度条的几何 ---------- */

/* 档位对应的缩放百分比（100 = 原始大小）。索引就是 couple_hug_tier_t。 */
static const int32_t kTierScale[4] = {220, 250, 300, 340};

/*
 * 按住时长 → 进度条宽度（像素）。
 * 以「不撒手抱抱」的门槛为满格，所以三段刻度分别落在 8.75% / 37.5% / 100%。
 * 同时也被当作两个刻度的水平偏移使用，两个含义共用同一个比例。
 */
static int hug_bar_x(uint32_t held_ms)
{
    if (held_ms >= COUPLE_HUG_TIER_CLINGY_MS) return kHugBarW;
    return (int)(((uint32_t)kHugBarW * held_ms) / COUPLE_HUG_TIER_CLINGY_MS);
}

/*
 * 进度条宽度只在真正变化时才动：show_hug_hold() 是 20Hz 调用的，
 * 每帧都调 lv_obj_set_width() 会让 LVGL 每帧重排一次。
 * 宽度 0 的对象不画，直接藏起来，省掉一次「宽度为 0 的矩形」的边界情况。
 */
static void hug_bar_set(int w)
{
    if (w == s_hug_hold.bar_w) return;
    s_hug_hold.bar_w = w;
    if (w <= 0) {
        lv_obj_add_flag(s_hug_hold.bar_fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_width(s_hug_hold.bar_fill, w);
    lv_obj_remove_flag(s_hug_hold.bar_fill, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- 配对页的小工具 ---------- */

static void pair_cell_default(int i)
{
    lv_obj_set_style_bg_opa(s_pair.cell[i], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_pair.cell[i], COL_LINE, 0);
    lv_obj_set_style_border_width(s_pair.cell[i], 1, 0);
    lv_obj_set_style_border_opa(s_pair.cell[i], LV_OPA_COVER, 0);
}

static void pair_digit_set(int i, char c, lv_color_t color)
{
    char buf[2];
    buf[0] = c;
    buf[1] = '\0';
    lv_label_set_text(s_pair.digit[i], buf);
    lv_obj_set_style_text_color(s_pair.digit[i], color, 0);
    lv_obj_remove_flag(s_pair.digit[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_pair.cell[i], LV_OBJ_FLAG_HIDDEN);
}

/* 停掉当前位的描边呼吸，并把描边恢复到「看得见」，避免停在半透明上。 */
static void pair_cursor_stop(void)
{
    lv_obj_t *cell = s_pair.cursor_cell;
    if (cell == NULL) return;
    lv_anim_delete(cell, anim_set_border_opa);
    lv_obj_set_style_border_opa(cell, LV_OPA_COVER, 0);
    s_pair.cursor_cell = NULL;
}

/* screen_ensure() 定义在 12 个 *_build() 的后面（它要调用它们），这里先用一下它的声明。 */
static lv_obj_t *screen_ensure(couple_view_t view);

/*
 * 把配对页擦成空白：每个 show_pair_* 都先调它，只点亮自己要用的那几个对象。
 * 位置不在这里恢复（各 show_* 自己 lv_obj_align），因为各状态的行距不同。
 */
static void pair_hide_all(void)
{
    /* 配对页没建时下面的 s_pair.cell[i] 全是 NULL，pair_cell_default() 会直接踩空指针。 */
    if (screen_ensure(COUPLE_VIEW_PAIR) == NULL) return;

    lv_obj_t *objs[] = {
        s_pair.title, s_pair.detail, s_pair.ttl, s_pair.hint,
        s_pair.name,  s_pair.heart,  s_pair.dot,
    };

    for (size_t i = 0; i < sizeof objs / sizeof objs[0]; i++) {
        if (objs[i] != NULL) lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 顶部转圈点在这里只藏不动：busy 状态被反复推时它的动画必须一直在跑
     * （见 show_pair_status 的 lv_anim_get 判断），复位由离开 busy 的分支负责。 */

    for (int i = 0; i < kPairDigits; i++) {
        pair_cell_default(i);
        lv_obj_add_flag(s_pair.cell[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pair.digit[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------- 配网页的小工具 ---------- */

/* 密码格：返回格子里那个居中的标签；要把整格藏起来时用 lv_obj_get_parent()。 */
static lv_obj_t *prov_pass_cell(lv_obj_t *scr, int x)
{
    lv_obj_t *box = ui_box(scr, x, 122, 108, 32, COL_PANEL, LV_OPA_COVER, 6);
    lv_obj_t *l = ui_label(box, COUPLE_FONT_NUM, COL_CREAM, "", 0, 0);
    lv_obj_center(l);
    return l;
}

/*
 * 密码分两格显示，每格最多 5 个字符、两格合计 10 个。
 * 当前固定密码是 "heartlink"（9 字符）：以前每格 4 个、总共只显示前 8 个，
 * 最后一位被吞掉，用户看到 "hear tlin" 反而以为密码不对。
 */
static void prov_set_pass(const char *pass)
{
    char a[6];
    char b[6];
    size_t n = (pass != NULL) ? strlen(pass) : 0u;
    size_t half;

    if (n > 10u) n = 10u;
    half = (n + 1u) / 2u;
    if (half > 5u) half = 5u;

    for (size_t i = 0; i < 5u; i++) {
        a[i] = (i < half) ? pass[i] : '\0';
        b[i] = (half + i < n) ? pass[half + i] : '\0';
    }
    a[5] = '\0';
    b[5] = '\0';

    lv_label_set_text(s_prov.pass_a, a);
    lv_label_set_text(s_prov.pass_b, b);
}

/* 密码相关的三个对象（说明文字 + 两个格子）一起显隐。 */
static void prov_pass_hide(bool hide)
{
    lv_obj_t *labels[2] = {s_prov.pass_a, s_prov.pass_b};

    if (s_prov.pass_cap != NULL) {
        if (hide) lv_obj_add_flag(s_prov.pass_cap, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_prov.pass_cap, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < 2; i++) {
        lv_obj_t *l = labels[i];
        lv_obj_t *box;
        if (l == NULL) continue;
        box = lv_obj_get_parent(l);
        if (hide) {
            lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
            if (box != NULL) lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
            if (box != NULL) lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* AP 信息一整组（SSID / 密码 / 地址 / 连接数 / 底部提示）的显隐。 */
static void prov_info_show(bool show)
{
    lv_obj_t *objs[] = {s_prov.ssid, s_prov.ip_cap, s_prov.ip, s_prov.clients, s_prov.hint};

    for (size_t i = 0; i < sizeof objs / sizeof objs[0]; i++) {
        if (objs[i] == NULL) continue;
        if (show) lv_obj_remove_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
    prov_pass_hide(!show);
}

/* ---------- 各页面的构建 ----------
 *
 * 每个 **_build() 都可能被调用多次：页面离开时被销毁，下次进入由 page_build() 重建。
 * 因此 build 里不能依赖任何「上一次构建留下的状态」——调用前 page_struct_clear()
 * 已经把本页结构体整块清零。反过来说，任何要碰本页 widget 的入口都必须先过一遍
 * screen_ensure()：页面没建时那些指针就是 NULL，页面已被释放时同样如此。
 */

static void home_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_home.scr = scr;

    ui_glow(scr, 120, 110, 190, COL_PINK_DEEP);

    /* 品牌 */
    ui_box(scr, 12, 19, 6, 6, COL_PINK, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    ui_label(scr, COUPLE_FONT_TITLE, COL_CREAM, COUPLE_STR_BRAND, 24, 14);

    /* 电量：描边框 + 内部填充 + 右上角的小尖，读不到电量时只留空框。 */
    s_home.frame = ui_frame(scr, kBatteryX, kBatteryY, kBatteryW, kBatteryH, COL_LINE, 2, 3);
    ui_box(scr, kBatteryX + kBatteryW, kBatteryY + 3, 2, 6, COL_LINE, LV_OPA_COVER, 0);
    s_home.fill = ui_box(scr, kBatteryX + 2, kBatteryY + 2, 0, kBatteryH - 4, COL_MINT, LV_OPA_COVER, 1);
    s_home.text = ui_label(scr, COUPLE_FONT_SMALL, COL_MUTED, "", 0, 0);
    lv_obj_align_to(s_home.text, s_home.frame, LV_ALIGN_OUT_LEFT_MID, -5, 0);

    /* 主心形：11 x 10 单位 x 8px = 88 x 80，中心正好落在 (120, 110)。 */
    s_home.heart = heart_build(scr, 8, COL_PINK);
    lv_obj_set_pos(s_home.heart, 76, 70);

    s_home.status = ui_label_mid(scr, 158, COUPLE_FONT_BODY, COL_MINT, COUPLE_STR_STATUS_UNPAIRED);
    s_home.name = ui_label_mid(scr, 178, COUPLE_FONT_BODY, COL_PINK_SOFT, "");
    s_home.ago = ui_label_mid(scr, 196, COUPLE_FONT_SMALL, COL_MUTED, "");
    lv_obj_add_flag(s_home.name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_home.ago, LV_OBJ_FLAG_HIDDEN);

    ui_label_mid(scr, 210, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_HOME_ENERGY_CAPTION);
    s_home.energy = ui_label_mid(scr, 224, COUPLE_FONT_NUM, COL_CREAM, "0000");
    s_home.total = ui_label_mid(scr, 254, COUPLE_FONT_SMALL, COL_PINK_SOFT, "");

    /* 键位提示贴着安全区底部（屏角圆角 30px，8..312 是安全高度）。 */
    ui_label(scr, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_HOME_HINT_MISS, kSafeX + 6, 278);
    ui_label_right(scr, kSafeX + 6, 278, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_HOME_HINT_HUG);
    ui_label_mid(scr, 294, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_HOME_HINT_MORE);
}

static void miss_sent_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_miss_sent_scr = scr;

    ui_glow(scr, 120, 116, 170, COL_PINK);

    lv_obj_t *heart = heart_build(scr, 4, COL_PINK);
    lv_obj_set_pos(heart, 98, 96);

    ui_label_mid(scr, 152, COUPLE_FONT_TITLE, COL_CREAM, COUPLE_STR_MISS_SENT_TITLE);
    ui_label_mid(scr, 182, COUPLE_FONT_BODY, COL_MINT, COUPLE_STR_MISS_SENT_SUB);
    ui_label_mid(scr, 236, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_MISS_SENT_HINT);
}

static void miss_recv_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_miss_recv.scr = scr;

    ui_glow(scr, 120, 92, 170, COL_PINK);

    s_miss_recv.heart = heart_build(scr, 6, COL_PINK);
    lv_obj_set_pos(s_miss_recv.heart, 87, 62);

    ui_label_mid(scr, 156, COUPLE_FONT_TITLE, COL_CREAM, COUPLE_STR_MISS_RECV_TITLE);
    s_miss_recv.ago = ui_label_mid(scr, 190, COUPLE_FONT_SMALL, COL_MUTED, "");
    s_miss_recv.flavor = ui_label_mid(scr, 216, COUPLE_FONT_BODY, COL_PINK_SOFT,
                                      COUPLE_STR_MISS_RECV_FLAVOR_DEFAULT);
    lv_obj_add_flag(s_miss_recv.ago, LV_OBJ_FLAG_HIDDEN);

    ui_label_mid(scr, 272, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_MISS_RECV_HINT);
}

static void hug_hold_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_hug_hold.scr = scr;

    ui_glow(scr, 120, 160, 180, COL_PINK_DEEP);

    s_hug_hold.title = ui_label_mid(scr, 16, COUPLE_FONT_TITLE, COL_CREAM, COUPLE_STR_HUG_HOLD_TITLE_IDLE);

    /* 心形只有 4px 单位格：抱抱页的缩放是持续动画，越小的对象越省渲染开销。 */
    s_hug_hold.heart = heart_build(scr, 4, COL_PINK);
    lv_obj_set_pos(s_hug_hold.heart, 98, 100);

    ui_box(scr, kHugBarX, kHugBarY, kHugBarW, kHugBarH, COL_LINE, LV_OPA_COVER, 4);
    s_hug_hold.bar_fill = ui_box(scr, kHugBarX, kHugBarY, 1, kHugBarH, COL_PINK, LV_OPA_COVER, 4);
    lv_obj_add_flag(s_hug_hold.bar_fill, LV_OBJ_FLAG_HIDDEN);

    /* 两根刻度：小抱抱 / 大抱抱的门槛。 */
    ui_box(scr, kHugBarX + hug_bar_x(COUPLE_HUG_TIER_SMALL_MS), kHugBarY - 2, 2, kHugBarH + 4, COL_MUTED,
           LV_OPA_COVER, 0);
    ui_box(scr, kHugBarX + hug_bar_x(COUPLE_HUG_TIER_BIG_MS), kHugBarY - 2, 2, kHugBarH + 4, COL_MUTED,
           LV_OPA_COVER, 0);

    s_hug_hold.secs = ui_label_mid(scr, 236, COUPLE_FONT_BODY, COL_PINK_SOFT, "");
    ui_label_mid(scr, 290, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_HUG_HOLD_HINT);

    /* 两个「上一次设过的值」缓存：故意设成不可能的值，保证首次刷新一定生效。 */
    s_hug_hold.bar_w = -1;
    s_hug_hold.secs_key = 0xFFFFFFFFu;
}

static void hug_recv_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_hug_recv.scr = scr;

    ui_glow(scr, 120, 110, 170, COL_PINK);

    s_hug_recv.heart_a = heart_build(scr, 6, COL_PINK);
    lv_obj_set_pos(s_hug_recv.heart_a, 57, 66);
    s_hug_recv.heart_b = heart_build(scr, 4, COL_PINK_SOFT);
    lv_obj_set_pos(s_hug_recv.heart_b, 139, 86);

    ui_label_mid(scr, 160, COUPLE_FONT_TITLE, COL_CREAM, COUPLE_STR_HUG_RECV_TITLE);
    s_hug_recv.duration = ui_label_mid(scr, 196, COUPLE_FONT_BODY, COL_PINK_SOFT, "");
    s_hug_recv.tier = ui_label_mid(scr, 222, COUPLE_FONT_SMALL, COL_MINT, "");
    ui_label_mid(scr, 258, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_HUG_RECV_HINT);
}

static void hug_sync_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_hug_sync.scr = scr;

    ui_glow(scr, 120, 110, 170, COL_PINK);

    /* 两圈同心环错开相位往外扩，做出「两台设备同时心跳」的感觉。 */
    s_hug_sync.ring_a = ui_ring(scr, 120, 110, 150, COL_PINK_DEEP, 3);
    s_hug_sync.ring_b = ui_ring(scr, 120, 110, 150, COL_PINK, 2);

    s_hug_sync.heart_a = heart_build(scr, 5, COL_PINK);
    lv_obj_set_pos(s_hug_sync.heart_a, 50, 85);
    s_hug_sync.heart_b = heart_build(scr, 5, COL_PINK_SOFT);
    lv_obj_set_pos(s_hug_sync.heart_b, 135, 85);

    ui_label_mid(scr, 196, COUPLE_FONT_TITLE, COL_CREAM, COUPLE_STR_HUG_SYNC_TITLE);
    ui_label_mid(scr, 228, COUPLE_FONT_BODY, COL_PINK_SOFT, COUPLE_STR_HUG_SYNC_SUB);
    ui_label_mid(scr, 288, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_HUG_SYNC_FOOTER);
}

static void heart_sync_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_heart_sync.scr = scr;

    ui_glow(scr, 120, 110, 190, COL_PINK);

    s_heart_sync.ring = ui_ring(scr, 120, 110, 180, COL_PINK, 3);
    s_heart_sync.heart = heart_build(scr, 7, COL_PINK);
    lv_obj_set_pos(s_heart_sync.heart, 82, 75);

    ui_label_mid(scr, 232, COUPLE_FONT_TITLE, COL_CREAM, COUPLE_STR_HEART_SYNC_TITLE);
    ui_label_mid(scr, 258, COUPLE_FONT_BODY, COL_PINK_SOFT, COUPLE_STR_HEART_SYNC_SUB);
    ui_label_mid(scr, 282, COUPLE_FONT_SMALL, COL_MINT, COUPLE_STR_HEART_SYNC_ENERGY);
}

static void milestone_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_ms.scr = scr;

    ui_glow(scr, 120, 120, 200, COL_PINK_DEEP);

    s_ms.ring = ui_ring(scr, 120, 120, 200, COL_PINK_DEEP, 3);

    s_ms.value = ui_label_mid(scr, 34, COUPLE_FONT_NUM, COL_CREAM, "");
    s_ms.title = ui_label_mid(scr, 68, COUPLE_FONT_TITLE, COL_PINK_SOFT, "");
    s_ms.bar = ui_box(scr, 20, 100, 200, 10, COL_PINK, LV_OPA_COVER, 5);

    s_ms.heart_a = heart_build(scr, 5, COL_PINK);
    lv_obj_set_pos(s_ms.heart_a, 24, 130);
    s_ms.heart_b = heart_build(scr, 5, COL_PINK_SOFT);
    lv_obj_set_pos(s_ms.heart_b, 161, 130);

    s_ms.bonus = ui_label_mid(scr, 196, COUPLE_FONT_BODY, COL_MINT, "");
    s_ms.romantic = ui_label_mid(scr, 222, COUPLE_FONT_BODY, COL_CREAM, "");
    s_ms.hint = ui_label_mid(scr, 292, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_MILESTONE_HINT);
}

static void daily_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_daily.scr = scr;

    ui_glow(scr, 120, 170, 210, COL_PINK_DEEP);

    ui_label_mid(scr, 26, COUPLE_FONT_TITLE, COL_CREAM, COUPLE_STR_DAILY_TITLE);

    /* 四行统计：左边小字标签、右边数值。没有责备、没有对比，只有「我们今天做了什么」。 */
    ui_label(scr, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_DAILY_MISSES_LABEL, 24, 84);
    s_daily.v_miss = ui_label_right(scr, 24, 84, COUPLE_FONT_BODY, COL_CREAM, "");

    ui_box(scr, 24, 118, 192, 1, COL_LINE, LV_OPA_COVER, 0);
    ui_label(scr, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_DAILY_HUGS_LABEL, 24, 128);
    s_daily.v_hug = ui_label_right(scr, 24, 128, COUPLE_FONT_BODY, COL_CREAM, "");

    ui_box(scr, 24, 162, 192, 1, COL_LINE, LV_OPA_COVER, 0);
    ui_label(scr, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_DAILY_FIRST_LABEL, 24, 172);
    s_daily.v_first = ui_label_right(scr, 24, 172, COUPLE_FONT_BODY, COL_CREAM, "");

    ui_box(scr, 24, 206, 192, 1, COL_LINE, LV_OPA_COVER, 0);
    ui_label(scr, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_DAILY_STREAK_LABEL, 24, 216);
    s_daily.v_streak = ui_label_right(scr, 24, 216, COUPLE_FONT_BODY, COL_PINK_SOFT, "");

    ui_label_mid(scr, 286, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_DAILY_HINT);
}

static void menu_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_menu.scr = scr;

    ui_glow(scr, 120, 160, 210, COL_PINK_DEEP);
    ui_label_mid(scr, 28, COUPLE_FONT_TITLE, COL_CREAM, COUPLE_STR_MENU_TITLE);

    for (int i = 0; i < kMenuCount; i++) {
        const char *label;
        lv_obj_t *item = ui_box(scr, kMenBarX, kMenBarY0 + i * kMenBarDY, kMenBarW, kMenBarH, COL_PANEL,
                                LV_OPA_COVER, 8);
        lv_obj_set_style_border_color(item, COL_LINE, 0);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_border_opa(item, LV_OPA_COVER, 0);
        s_menu.item[i] = item;

        if (i == 0) label = COUPLE_STR_MENU_ITEM_PAIR;
        else if (i == 1) label = COUPLE_STR_MENU_ITEM_PROV;
        else label = COUPLE_STR_MENU_ITEM_UNPAIR;

        s_menu.text[i] = ui_label(item, COUPLE_FONT_BODY, COL_MUTED, label, 0, 0);
        lv_obj_center(s_menu.text[i]);
    }

    ui_label_mid(scr, 286, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_MENU_HINT);
}

static void pair_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_pair.scr = scr;

    ui_glow(scr, 120, 150, 200, COL_PINK_DEEP);

    /* 顶部转圈点：busy 状态用，平时藏着。 */
    s_pair.dot = ui_box(scr, 115, 14, 10, 10, COL_PINK, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    scalable_add(s_pair.dot, SC_KIND_CIRCLE, 10, 120, 19);
    lv_obj_add_flag(s_pair.dot, LV_OBJ_FLAG_HIDDEN);

    s_pair.title = ui_label_mid(scr, 40, COUPLE_FONT_TITLE, COL_CREAM, "");
    s_pair.heart = heart_build(scr, 6, COL_PINK);
    lv_obj_set_pos(s_pair.heart, 87, 74);
    lv_obj_add_flag(s_pair.heart, LV_OBJ_FLAG_HIDDEN);

    s_pair.ttl = ui_label_mid(scr, 134, COUPLE_FONT_SMALL, COL_PINK_SOFT, "");
    s_pair.detail = ui_label_mid(scr, 152, COUPLE_FONT_BODY, COL_MUTED, "");
    s_pair.name = ui_label_mid(scr, 182, COUPLE_FONT_TITLE, COL_PINK, "");
    s_pair.hint = ui_label_mid(scr, 246, COUPLE_FONT_SMALL, COL_MUTED, "");

    /* 6 个输入格：x = 27 + i*32，整体水平中心正好是 120。 */
    for (int i = 0; i < kPairDigits; i++) {
        s_pair.cell[i] = ui_frame(scr, 27 + i * 32, 86, 26, 34, COL_LINE, 1, 6);
        s_pair.digit[i] = ui_label(s_pair.cell[i], COUPLE_FONT_NUM, COL_CREAM, "-", 0, 0);
        lv_obj_center(s_pair.digit[i]);
    }
    s_pair.cursor_cell = NULL;
}

static void prov_build(void)
{
    lv_obj_t *scr = ui_screen_new();
    s_prov.scr = scr;

    ui_glow(scr, 120, 150, 200, COL_PINK_DEEP);

    s_prov.dot = ui_box(scr, 115, 14, 10, 10, COL_PINK, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    scalable_add(s_prov.dot, SC_KIND_CIRCLE, 10, 120, 19);
    lv_obj_add_flag(s_prov.dot, LV_OBJ_FLAG_HIDDEN);

    s_prov.title = ui_label_mid(scr, 32, COUPLE_FONT_TITLE, COL_CREAM, "");
    s_prov.ssid = ui_label_mid(scr, 84, COUPLE_FONT_BODY, COL_PINK_SOFT, "");

    s_prov.pass_cap = ui_label(scr, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_PROV_PASS_CAPTION, 12, 104);
    s_prov.pass_a = prov_pass_cell(scr, 12);
    s_prov.pass_b = prov_pass_cell(scr, 120);

    s_prov.ip_cap = ui_label(scr, COUPLE_FONT_SMALL, COL_MUTED, COUPLE_STR_PROV_IP_CAPTION, 12, 176);
    s_prov.ip = ui_label_mid(scr, 196, COUPLE_FONT_BODY, COL_MINT, "");
    s_prov.clients = ui_label_mid(scr, 246, COUPLE_FONT_SMALL, COL_CREAM, "");

    s_prov.detail = ui_label_mid(scr, 152, COUPLE_FONT_BODY, COL_MUTED, "");
    s_prov.hint = ui_label_mid(scr, 286, COUPLE_FONT_SMALL, COL_MUTED, "");
}

/* 顶部横幅建在 layer_top 上：切页时不会跟着一起滚掉，也不需要每页各留一条。 */
static void toast_build(void)
{
    lv_obj_t *top = lv_layer_top();

    s_toast.box = ui_box(top, 10, 22, 220, 38, COL_PANEL, LV_OPA_COVER, 19);
    lv_obj_set_style_border_color(s_toast.box, COL_PINK, 0);
    lv_obj_set_style_border_width(s_toast.box, 1, 0);
    lv_obj_set_style_border_opa(s_toast.box, LV_OPA_COVER, 0);

    s_toast.text = ui_label(s_toast.box, COUPLE_FONT_BODY, COL_CREAM, "", 0, 0);
    lv_obj_set_width(s_toast.text, 208);
    lv_obj_set_style_text_align(s_toast.text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_toast.text);

    lv_obj_add_flag(s_toast.box, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- 内存打点 ---------- */

/*
 * LVGL 池余量打点。池子耗尽时 lv_malloc 返回 NULL，而 LVGL 的默认断言处理器
 * 就是 while(1)（lv_conf_internal.h 的 LV_ASSERT_HANDLER），屏幕上的表现是
 * 「整机不响应」，连 WDT 打印都落在同一处死循环上。
 * 建页 / 删页 / 周期各留一行数字，出问题时能直接看出余量是被哪一页、在什么时候
 * 吃掉的。只在持锁上下文里调用：lv_mem_monitor 读的是池的内部结构，必须与
 * LVGL 任务互斥。
 */
static void mem_log(const char *what, int view)
{
    lv_mem_monitor_t mon;

    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "[mem] %s(%d) free=%u biggest=%u used=%u%% max=%u total=%u frag=%u%%",
             what, view, (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
             (unsigned)mon.used_pct, (unsigned)mon.max_used, (unsigned)mon.total_size,
             (unsigned)mon.frag_pct);
}

/* ---------- 页面切换 ---------- */

/*
 * 页面管线的四个小函数，各做一件事：
 *   page_scr_slot()     取「某页 screen 指针存放的位置」
 *   page_struct_clear() 清空该页状态（不碰任何 LVGL 对象）
 *   page_build()        清状态 → 标记归属页 → 调 *_build() → 复位归属页
 *   page_release()      摘缩放登记 → 停动画 → 删屏幕 → 置空槽位 → 清状态
 *
 * 为什么「离开即销毁」：LVGL 用的是自己的内存池（CONFIG_LV_MEM_SIZE_KILOBYTES），
 * 它是一块独立的静态区，和系统堆不共享。真机实测 12 个页面合计 49,096 B，加上
 * 6,348 B 基线共 55,444 B，超过 48 KB 的池；页面全驻留迟早会把池子耗尽，LVGL 内部
 * 拿到的就是 NULL，接着在 LV_ASSERT_NULL 的 while(1) 里永久挂住 LVGL 任务。
 * 首页常驻（每秒的 tick 都要刷它），其余页面离开即销毁，稳态占用约 19.4 KB。
 */

/* 返回「某页 screen 指针的槽位地址」。MISS_SENT 用的是裸指针 s_miss_sent_scr，
 * 其余页面结构体的首字段都是 lv_obj_t *scr。未知 view 返回 NULL。 */
static lv_obj_t **page_scr_slot(couple_view_t view)
{
    switch (view) {
    case COUPLE_VIEW_HOME: return &s_home.scr;
    case COUPLE_VIEW_MISS_SENT: return &s_miss_sent_scr;
    case COUPLE_VIEW_MISS_RECV: return &s_miss_recv.scr;
    case COUPLE_VIEW_HUG_HOLD: return &s_hug_hold.scr;
    case COUPLE_VIEW_HUG_RECV: return &s_hug_recv.scr;
    case COUPLE_VIEW_HUG_SYNC: return &s_hug_sync.scr;
    case COUPLE_VIEW_HEART_SYNC: return &s_heart_sync.scr;
    case COUPLE_VIEW_MILESTONE: return &s_ms.scr;
    case COUPLE_VIEW_DAILY: return &s_daily.scr;
    case COUPLE_VIEW_MENU: return &s_menu.scr;
    case COUPLE_VIEW_PAIR: return &s_pair.scr;
    case COUPLE_VIEW_PROV: return &s_prov.scr;
    default: return NULL;
    }
}

/*
 * 清空本页状态，不碰任何 LVGL 对象。
 * 页面删掉之后每个 widget 指针都是悬垂的：整块清零比逐页核对「哪个字段没被 build
 * 赋值」更稳妥，也保证下面的 NULL 早返回守卫真的能生效。
 */
static void page_struct_clear(couple_view_t view)
{
    switch (view) {
    case COUPLE_VIEW_HOME: memset(&s_home, 0, sizeof s_home); break;
    case COUPLE_VIEW_MISS_SENT: s_miss_sent_scr = NULL; break;
    case COUPLE_VIEW_MISS_RECV: memset(&s_miss_recv, 0, sizeof s_miss_recv); break;
    case COUPLE_VIEW_HUG_HOLD: memset(&s_hug_hold, 0, sizeof s_hug_hold); break;
    case COUPLE_VIEW_HUG_RECV: memset(&s_hug_recv, 0, sizeof s_hug_recv); break;
    case COUPLE_VIEW_HUG_SYNC: memset(&s_hug_sync, 0, sizeof s_hug_sync); break;
    case COUPLE_VIEW_HEART_SYNC: memset(&s_heart_sync, 0, sizeof s_heart_sync); break;
    case COUPLE_VIEW_MILESTONE: memset(&s_ms, 0, sizeof s_ms); break;
    case COUPLE_VIEW_DAILY: memset(&s_daily, 0, sizeof s_daily); break;
    case COUPLE_VIEW_MENU: memset(&s_menu, 0, sizeof s_menu); break;
    case COUPLE_VIEW_PAIR: memset(&s_pair, 0, sizeof s_pair); break;
    case COUPLE_VIEW_PROV: memset(&s_prov, 0, sizeof s_prov); break;
    default: break;
    }
}

/*
 * 构建一页。先清状态，再在 *_build() 期间把归属页记到 s_build_owner，这样经
 * heart_build()/ui_ring() 登记到缩放表里的对象会带上页面标签，释放时能整批摘掉。
 * 前后各打一条池余量：差值就是这一页吃掉的字节数，见 mem_log()。
 */
static void page_build(couple_view_t view)
{
    page_struct_clear(view);

    mem_log("build-in", (int)view);
    s_build_owner = view;
    switch (view) {
    case COUPLE_VIEW_HOME: home_build(); break;
    case COUPLE_VIEW_MISS_SENT: miss_sent_build(); break;
    case COUPLE_VIEW_MISS_RECV: miss_recv_build(); break;
    case COUPLE_VIEW_HUG_HOLD: hug_hold_build(); break;
    case COUPLE_VIEW_HUG_RECV: hug_recv_build(); break;
    case COUPLE_VIEW_HUG_SYNC: hug_sync_build(); break;
    case COUPLE_VIEW_HEART_SYNC: heart_sync_build(); break;
    case COUPLE_VIEW_MILESTONE: milestone_build(); break;
    case COUPLE_VIEW_DAILY: daily_build(); break;
    case COUPLE_VIEW_MENU: menu_build(); break;
    case COUPLE_VIEW_PAIR: pair_build(); break;
    case COUPLE_VIEW_PROV: prov_build(); break;
    default: break;
    }
    s_build_owner = kOwnerNone;
    mem_log("build-out", (int)view);
}

/*
 * 懒构建入口：页面没建就建，已建就直接返回它。
 * 建之前先查池余量（见下面的守卫）；返回 NULL 表示 view 非法、余量不足，或者
 * build 自己拿不到对象（scr 仍是 NULL）。调用方拿到 NULL 就不要再碰本页的任何
 * widget，直接解锁返回即可。
 */
static lv_obj_t *screen_ensure(couple_view_t view)
{
    lv_obj_t **slot = page_scr_slot(view);
    lv_mem_monitor_t mon;

    if (slot == NULL) return NULL;
    if (*slot != NULL) return *slot;

    /*
     * 低内存守卫：余量不够时宁可不建这一页，也不能让 LVGL 自己去撞 NULL。
     * 池子见底之后任何一次 lv_malloc 失败都会落进 LV_ASSERT_HANDLER 的 while(1)，
     * LVGL 任务就此永久挂死（真机表现是整机不响应，只剩 task_wdt 在刷）。返回
     * NULL 的路径调用方已经能正确处理：解锁返回，屏幕停在原页。
     */
    lv_mem_monitor(&mon);
    if (mon.free_biggest_size < (size_t)kUiMinFreeBlock ||
        mon.free_size < (size_t)kUiMinFreeTotal) {
        ESP_LOGE(TAG, "[mem] 池余量不足，拒绝构建第 %d 页 free=%u biggest=%u total=%u",
                 (int)view, (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
                 (unsigned)mon.total_size);
        return NULL;
    }

    page_build(view);
    return *slot;
}

static void page_anims_start(couple_view_t view)
{
    switch (view) {
    case COUPLE_VIEW_HOME:
        /* 主心形按 kHomeBeatKeys 的双拍波形跳，一个周期 1 秒（约 60 bpm）。
         * 时间轴用 linear：波形由关键帧自己给，ease 曲线会把它拉变形。
         * progress 走 0..HOME_BEAT_PERIOD_MS 毫秒，exec 里按关键帧插值成缩放百分比。 */
        anim_run(s_home.heart, anim_home_beat, 0, HOME_BEAT_PERIOD_MS, HOME_BEAT_PERIOD_MS, 0, 0,
                 LV_ANIM_REPEAT_INFINITE, 0, lv_anim_path_linear);
        break;

    case COUPLE_VIEW_HUG_SYNC:
        /* 两圈环相位差半个周期；两颗心一起跳。 */
        anim_run(s_hug_sync.ring_a, anim_ring_pulse, 0, 100, 1800, 0, 0, LV_ANIM_REPEAT_INFINITE, 0,
                 lv_anim_path_linear);
        anim_run(s_hug_sync.ring_b, anim_ring_pulse, 0, 100, 1800, 900, 0, LV_ANIM_REPEAT_INFINITE, 900,
                 lv_anim_path_linear);
        anim_run(s_hug_sync.heart_a, anim_scale_percent, 100, 112, 900, 0, 900, LV_ANIM_REPEAT_INFINITE, 0,
                 lv_anim_path_ease_in_out);
        anim_run(s_hug_sync.heart_b, anim_scale_percent, 100, 112, 900, 0, 900, LV_ANIM_REPEAT_INFINITE, 0,
                 lv_anim_path_ease_in_out);
        break;

    case COUPLE_VIEW_HEART_SYNC:
        anim_run(s_heart_sync.ring, anim_ring_pulse, 0, 100, 900, 0, 0, LV_ANIM_REPEAT_INFINITE, 0,
                 lv_anim_path_linear);
        anim_run(s_heart_sync.heart, anim_scale_percent, 100, 108, 700, 0, 700, LV_ANIM_REPEAT_INFINITE, 0,
                 lv_anim_path_ease_in_out);
        break;

    case COUPLE_VIEW_MILESTONE:
        anim_run(s_ms.ring, anim_ring_pulse, 0, 100, 1800, 0, 0, LV_ANIM_REPEAT_INFINITE, 0,
                 lv_anim_path_linear);
        break;

    default:
        break;
    }
}

/*
 * 离开页面时必须把常驻动画停掉并复位。
 * 尤其是圆环：脉冲的最后一帧把 opa 压到 0，不复位的话下次回来就是一片空白。
 */
static void page_anims_stop(couple_view_t view)
{
    switch (view) {
    case COUPLE_VIEW_HOME:
        lv_anim_delete(s_home.heart, anim_home_beat);
        set_scale_percent(s_home.heart, 100);
        break;

    case COUPLE_VIEW_HUG_HOLD:
        lv_anim_delete(s_hug_hold.heart, anim_scale_percent);
        set_scale_percent(s_hug_hold.heart, 100);
        break;

    case COUPLE_VIEW_HUG_SYNC:
        lv_anim_delete(s_hug_sync.ring_a, anim_ring_pulse);
        lv_anim_delete(s_hug_sync.ring_b, anim_ring_pulse);
        lv_obj_set_style_opa(s_hug_sync.ring_a, LV_OPA_COVER, 0);
        lv_obj_set_style_opa(s_hug_sync.ring_b, LV_OPA_COVER, 0);
        set_scale_percent(s_hug_sync.ring_a, 100);
        set_scale_percent(s_hug_sync.ring_b, 100);
        lv_anim_delete(s_hug_sync.heart_a, anim_scale_percent);
        lv_anim_delete(s_hug_sync.heart_b, anim_scale_percent);
        set_scale_percent(s_hug_sync.heart_a, 100);
        set_scale_percent(s_hug_sync.heart_b, 100);
        break;

    case COUPLE_VIEW_HEART_SYNC:
        lv_anim_delete(s_heart_sync.ring, anim_ring_pulse);
        lv_obj_set_style_opa(s_heart_sync.ring, LV_OPA_COVER, 0);
        set_scale_percent(s_heart_sync.ring, 100);
        lv_anim_delete(s_heart_sync.heart, anim_scale_percent);
        set_scale_percent(s_heart_sync.heart, 100);
        break;

    case COUPLE_VIEW_MILESTONE:
        lv_anim_delete(s_ms.ring, anim_ring_pulse);
        lv_obj_set_style_opa(s_ms.ring, LV_OPA_COVER, 0);
        set_scale_percent(s_ms.ring, 100);
        break;

    default:
        break;
    }
}

/*
 * 释放一页：摘掉本页的缩放登记 → 停本页动画 → 删屏幕对象 → 槽位置 NULL → 清空状态。
 * 顺序不能换，理由分别是：
 *   1. 首页常驻，释放它等于把每秒 tick 的目标打空。
 *   2. 页面没建时没有对象可停，但缩放表里仍可能有（理论上不会有）本页的残留登记，
 *      所以还是要摘登记、清状态。
 *   3. 动画必须停在删对象之前：动画回调持有 widget 指针，删掉之后回调会踩悬垂内存。
 *   4. LVGL 不保证删活动屏幕的行为（lv_obj_delete 会把 disp->act_scr 置 NULL），
 *      正常流程下 show_view() 已经先 lv_screen_load() 了新页，这里只是防御。
 */
static void page_release(couple_view_t view)
{
    lv_obj_t **slot;

    if (view == COUPLE_VIEW_HOME) return;

    slot = page_scr_slot(view);
    if (slot == NULL || *slot == NULL) {
        scalables_drop_owner(view);
        page_struct_clear(view);
        return;
    }

    page_anims_stop(view);
    /* PAIR 没有常驻的 page_anims case，光标的描边呼吸要单独收。幂等。 */
    if (view == COUPLE_VIEW_PAIR) pair_cursor_stop();

    if (lv_screen_active() == *slot) {
        ESP_LOGW(TAG, "page_release(%d): 仍是活动屏幕，跳过删除", (int)view);
        return;
    }

    lv_obj_delete(*slot);
    *slot = NULL;

    scalables_drop_owner(view);
    page_struct_clear(view);
    mem_log("release", (int)view);
}

static void show_view(lv_obj_t *scr, couple_view_t view)
{
    couple_view_t prev;

    if (scr == NULL) return;

    /* 任何页面切换都点亮屏幕。对方发来的互动一律走 couple_ui_show_* → show_view，
     * 所以「收到消息时屏幕是黑的」这条路在这里被兜住。 */
    couple_ui_wake();

    prev = s_view;
    if (prev != view) page_anims_stop(prev);
    /* 离开配对页时，输入光标的描边呼吸也一起收掉。 */
    if (prev == COUPLE_VIEW_PAIR && view != COUPLE_VIEW_PAIR) pair_cursor_stop();

    lv_screen_load(scr);
    s_view = view;
    page_anims_start(view);

    /* 离开的页面不再驻留：LVGL 独立池只有 48 KB，12 个页面全留在池里装不下
     * （实测 6,348 基线 + 49,096 页面 = 55,444 B）。首页常驻，其余页离开即销毁，
     * 下次进入由 screen_ensure() 重建。 */
    if (prev != view && prev != COUPLE_VIEW_HOME) page_release(prev);
}

/* ---------- 顶部横幅（内部版本，调用方必须已持锁） ---------- */

static void toast_hide(void)
{
    if (s_toast.box != NULL) lv_obj_add_flag(s_toast.box, LV_OBJ_FLAG_HIDDEN);
}

static void toast_show(const char *text)
{
    if (s_toast.box == NULL) return;

    if (text == NULL || text[0] == '\0') {
        toast_hide();
        if (s_toast_timer != NULL) lv_timer_pause(s_toast_timer);
        return;
    }

    lv_label_set_text(s_toast.text, text);
    lv_obj_remove_flag(s_toast.box, LV_OBJ_FLAG_HIDDEN);
    if (s_toast_timer != NULL) {
        lv_timer_reset(s_toast_timer);
        lv_timer_resume(s_toast_timer);
    }
}

/* ---------- 定时器 ---------- */

static void tick_cb(lv_timer_t *timer)
{
    static uint32_t ticks;

    if (timer != s_tick) return;
    if (!s_inited) return;

    /* 三页的时间文案共用一份状态，一次全刷；反正每秒只有一次。 */
    home_refresh();
    miss_recv_refresh();
    daily_refresh();

    /* 周期池余量打点：这里是全工程唯一「持着 LVGL 锁又定期跑」的地方。
     * 留的是历史最高水位 max_used，比瞬时值更能说明离见底还有多远。 */
    if (++ticks >= kMemLogEveryTicks) {
        ticks = 0u;
        mem_log("tick", (int)s_view);
    }
}

static void toast_timer_cb(lv_timer_t *timer)
{
    toast_hide();
    lv_timer_pause(timer);
}

/* ---------- 抱抱页的进入与里程碑标题 ---------- */

/* 重置成一进入就能直接用的状态，后面 show_hug_hold() 只做增量更新。 */
static void hug_hold_enter(void)
{
    /* 这一页可能是第一次进：先确保建好，再动它的 widget。 */
    if (screen_ensure(COUPLE_VIEW_HUG_HOLD) == NULL) return;

    s_tier = COUPLE_HUG_TIER_NONE;
    s_hug_hold.bar_w = -1;
    s_hug_hold.secs_key = 0xFFFFFFFFu;

    lv_label_set_text(s_hug_hold.title, COUPLE_STR_HUG_HOLD_TITLE_IDLE);
    lv_label_set_text_fmt(s_hug_hold.secs, COUPLE_STR_HUG_HELD_SECONDS_FMT, 0u, 0u);
    hug_bar_set(0);

    /* 心跳一样跳一下，告诉用户「已经按住了」。
     * 幅度固定 25%：像素心只能整格缩放（基数 4px，一格合 25%），少了就看不出跳。 */
    anim_run(s_hug_hold.heart, anim_scale_percent, kTierScale[0] + 25, kTierScale[0], 260, 0, 0, 0, 0,
             lv_anim_path_overshoot);

    show_view(s_hug_hold.scr, COUPLE_VIEW_HUG_HOLD);
}

/*
 * 里程碑标题：couple_state.c 只提供中文（"1314 · 一生一世"），没法在界面层翻译，
 * 所以纯英文构建下宁可不显示，也不要弹出 missing glyph 方框。
 */
#ifdef COUPLE_USE_APP_FONT
static void milestone_title_set(int index)
{
    if (screen_ensure(COUPLE_VIEW_MILESTONE) == NULL) return;

    const char *t = couple_milestone_title(index);
    if (t == NULL || t[0] == '\0') {
        lv_obj_add_flag(s_ms.title, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_ms.title, t);
    lv_obj_remove_flag(s_ms.title, LV_OBJ_FLAG_HIDDEN);
}
#else
static void milestone_title_set(int index)
{
    if (screen_ensure(COUPLE_VIEW_MILESTONE) == NULL) return;

    (void)index;
    lv_obj_add_flag(s_ms.title, LV_OBJ_FLAG_HIDDEN);
}
#endif

/* ---------- 公开接口 ---------- */

/* ---------- 背光休眠 ---------- */

/*
 * 只关背光：页面对象、动画、LVGL 定时器全部原样留着，所以唤醒后画面还是离开时
 * 那一页，不需要重建（48 KB 的 LVGL 池也经不起重建）。
 * 这里不碰 LVGL 对象，因此不取 bsp_lvgl_lock()；ledc 写占空比本身很短。
 */
void couple_ui_sleep(void)
{
    if (s_sleeping) return;

    bsp_display_backlight(0);
    s_sleeping = true;
}

void couple_ui_wake(void)
{
    if (!s_sleeping) return;

    bsp_display_backlight(kBacklightOn);
    s_sleeping = false;
}

bool couple_ui_sleeping(void)
{
    return s_sleeping;
}

esp_err_t couple_ui_init(void)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return ESP_ERR_TIMEOUT;

    if (s_inited) {
        bsp_lvgl_unlock();
        return ESP_OK;
    }

    home_build();
    toast_build();

    s_tick = lv_timer_create(tick_cb, COUPLE_HOME_TICK_MS, NULL);
    s_toast_timer = lv_timer_create(toast_timer_cb, COUPLE_TOAST_MS, NULL);
    if (s_toast_timer != NULL) lv_timer_pause(s_toast_timer);

    s_inited = true;
    home_refresh();
    daily_refresh();
    miss_recv_refresh();
    show_view(s_home.scr, COUPLE_VIEW_HOME);

    bsp_lvgl_unlock();
    return ESP_OK;
}

void couple_ui_push_status(const couple_ui_status_t *status)
{
    if (status == NULL) return;
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;

    s_status = *status;
    if (s_inited) {
        s_status_pushed = true;
        home_refresh();
        daily_refresh();
        miss_recv_refresh();
    }

    bsp_lvgl_unlock();
}

couple_view_t couple_ui_view(void)
{
    couple_view_t view;

    if (!bsp_lvgl_lock(kLockTimeoutMs)) return s_view;
    view = s_view;
    bsp_lvgl_unlock();
    return view;
}

void couple_ui_show(couple_view_t view)
{
    lv_obj_t *scr;

    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    scr = screen_ensure(view);
    if (scr == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    /* 首页、日报页、收到的想念这三页的内容由 push_status 和每秒的 tick 维护，
     * 首次进入时页面刚建出来、里面还是空白文案；这里先补刷一次，免得空着等下一秒。 */
    if (view == COUPLE_VIEW_HOME) home_refresh();
    else if (view == COUPLE_VIEW_DAILY) daily_refresh();
    else if (view == COUPLE_VIEW_MISS_RECV) miss_recv_refresh();

    show_view(scr, view);
    bsp_lvgl_unlock();
}

void couple_ui_show_miss_received(const char *flavor)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (screen_ensure(COUPLE_VIEW_MISS_RECV) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    if (flavor != NULL && flavor[0] != '\0') lv_label_set_text(s_miss_recv.flavor, flavor);
    else lv_label_set_text(s_miss_recv.flavor, COUPLE_STR_MISS_RECV_FLAVOR_DEFAULT);

    miss_recv_refresh();
    show_view(s_miss_recv.scr, COUPLE_VIEW_MISS_RECV);

    /* 心形由小到大：40% → 100%，越过目标再回落。 */
    anim_run(s_miss_recv.heart, anim_scale_percent, 40, 100, 900, 0, 0, 0, 0, lv_anim_path_overshoot);

    bsp_lvgl_unlock();
}

void couple_ui_show_miss_sent(void)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    show_view(screen_ensure(COUPLE_VIEW_MISS_SENT), COUPLE_VIEW_MISS_SENT);
    bsp_lvgl_unlock();
}

void couple_ui_show_hug_hold(uint32_t held_ms, couple_hug_tier_t tier)
{
    int t;
    unsigned key;

    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (screen_ensure(COUPLE_VIEW_HUG_HOLD) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    /* 20Hz 的热路径：只在真正需要时才动 LVGL 对象。 */
    if (s_view != COUPLE_VIEW_HUG_HOLD) hug_hold_enter();

    if (held_ms > COUPLE_HUG_MAX_MS) held_ms = COUPLE_HUG_MAX_MS;

    t = (int)tier;
    if (t < 0) t = 0;
    if (t > 3) t = 3;
    if ((couple_hug_tier_t)t != s_tier) {
        s_tier = (couple_hug_tier_t)t;
        lv_label_set_text(s_hug_hold.title, hug_tier_text(s_tier));
        /* pop 幅度必须够一整格：像素心只能整格缩放，基数是 4px 时单位格每 25% 才变一次，
         * 原来写 18% 在 340% 档上四舍五入后还是同一格（(4*358+50)/100 == (4*340+50)/100 == 14），
         * 也就是说「不撒手」档按下时心形完全不动。 */
        anim_pop(s_hug_hold.heart, kTierScale[t], 25, 260);
    }

    hug_bar_set(hug_bar_x(held_ms));

    /* 「十分之一秒」为键：文案每秒变 10 次，同值不重复格式化。 */
    key = held_ms / 100u;
    if (key != s_hug_hold.secs_key) {
        s_hug_hold.secs_key = key;
        lv_label_set_text_fmt(s_hug_hold.secs, COUPLE_STR_HUG_HELD_SECONDS_FMT,
                              (unsigned)(held_ms / 1000u), (unsigned)((held_ms / 100u) % 10u));
    }

    bsp_lvgl_unlock();
}

void couple_ui_show_hug_sync(void)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    show_view(screen_ensure(COUPLE_VIEW_HUG_SYNC), COUPLE_VIEW_HUG_SYNC);
    bsp_lvgl_unlock();
}

void couple_ui_show_hug_received(uint16_t held_ms)
{
    couple_hug_tier_t tier;

    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (screen_ensure(COUPLE_VIEW_HUG_RECV) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    lv_label_set_text_fmt(s_hug_recv.duration, COUPLE_STR_HUG_RECV_DURATION_FMT,
                          (unsigned)(held_ms / 1000u), (unsigned)((held_ms / 100u) % 10u));

    /* 没到「小抱抱」档位的误触不标档位，省得出现「Holding...」这种对不上的话。 */
    tier = couple_hug_tier_for_ms(held_ms);
    if (tier == COUPLE_HUG_TIER_NONE) {
        lv_obj_add_flag(s_hug_recv.tier, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_hug_recv.tier, hug_tier_text(tier));
        lv_obj_remove_flag(s_hug_recv.tier, LV_OBJ_FLAG_HIDDEN);
    }

    show_view(s_hug_recv.scr, COUPLE_VIEW_HUG_RECV);

    anim_run(s_hug_recv.heart_a, anim_scale_percent, 60, 100, 700, 0, 0, 0, 0, lv_anim_path_overshoot);
    anim_run(s_hug_recv.heart_b, anim_scale_percent, 60, 100, 700, 250, 0, 0, 0, lv_anim_path_overshoot);

    bsp_lvgl_unlock();
}

void couple_ui_show_heart_sync(void)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    show_view(screen_ensure(COUPLE_VIEW_HEART_SYNC), COUPLE_VIEW_HEART_SYNC);
    bsp_lvgl_unlock();
}

void couple_ui_show_milestone(int index)
{
    bool romantic;

    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (index < 0 || index >= (int)COUPLE_MILESTONE_COUNT) {
        /* 越界不是错误，只是这次没有彩蛋：给个不扫兴的横幅就回去。 */
        toast_show(COUPLE_STR_MILESTONE_OOR_TOAST);
        bsp_lvgl_unlock();
        return;
    }

    /* 越界分支只发横幅、不碰本页 widget，所以 ensure 放在它后面：省得为一次越界白建一页。 */
    if (screen_ensure(COUPLE_VIEW_MILESTONE) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    lv_label_set_text_fmt(s_ms.value, "%u", (unsigned)couple_milestone_value(index));
    milestone_title_set(index);

    /* 520 / 1314 是「两颗心」，其余里程碑只有一颗心摆在正中。 */
    romantic = couple_milestone_is_romantic(index);
    if (romantic) {
        lv_obj_set_pos(s_ms.heart_a, 24, 130);
        lv_obj_set_pos(s_ms.heart_b, 161, 130);
        lv_obj_remove_flag(s_ms.heart_b, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_pos(s_ms.heart_a, 92, 130);
        lv_obj_add_flag(s_ms.heart_b, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text_fmt(s_ms.bonus, COUPLE_STR_MILESTONE_BONUS_FMT,
                          (unsigned)couple_milestone_energy_bonus(index));

    if (index == (int)COUPLE_MILESTONE_520) lv_label_set_text(s_ms.romantic, COUPLE_STR_MILESTONE_ROMANTIC_520);
    else if (index == (int)COUPLE_MILESTONE_1314)
        lv_label_set_text(s_ms.romantic, COUPLE_STR_MILESTONE_ROMANTIC_1314);
    else lv_label_set_text(s_ms.romantic, "");

    show_view(s_ms.scr, COUPLE_VIEW_MILESTONE);

    /* 数值是 label，改不了几何；用透明度强调替代原来的缩放弹跳。 */
    anim_run(s_ms.value, anim_opa_emphasis, LV_OPA_40, LV_OPA_COVER, 420, 0, 0, 0, 0,
             lv_anim_path_ease_out);
    anim_pop(s_ms.heart_a, 100, 25, 420);

    bsp_lvgl_unlock();
}

void couple_ui_show_offline_summary(uint16_t miss_count)
{
    char buf[64];

    if (miss_count == 0) return;

    /* 这条路径只出横幅、不经过 show_view()，但它是「你不在的时候 TA 想了你 N 次」，
     * 属于对方发来的互动：必须能点亮屏幕，否则睡着的设备会整段错过。 */
    couple_ui_wake();

    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    /* 汇总只以横幅的形式出现：补拉发生在开机回首页的那一刻，
     * 强行切页会把用户从首页拽走，横幅更礼貌。 */
    offline_text(buf, sizeof buf, miss_count);
    if (buf[0] != '\0') toast_show(buf);

    bsp_lvgl_unlock();
}

void couple_ui_toast(const char *text)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (s_inited) toast_show(text);
    bsp_lvgl_unlock();
}

bool couple_ui_home_ready(void)
{
    return s_status_pushed;
}

uint8_t couple_ui_menu_count(void)
{
    return (uint8_t)kMenuCount;
}

void couple_ui_show_menu(uint8_t selected)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (screen_ensure(COUPLE_VIEW_MENU) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    if (selected >= (uint8_t)kMenuCount) selected = (uint8_t)(kMenuCount - 1);

    for (int i = 0; i < kMenuCount; i++) {
        bool on = ((uint8_t)i == selected);
        lv_obj_set_style_bg_color(s_menu.item[i], on ? COL_PINK : COL_PANEL, 0);
        lv_obj_set_style_border_opa(s_menu.item[i], on ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_menu.text[i], on ? COL_BG : COL_MUTED, 0);
    }

    show_view(s_menu.scr, COUPLE_VIEW_MENU);
    bsp_lvgl_unlock();
}

void couple_ui_show_pair_offer(const char *code, uint32_t ttl_secs)
{
    size_t n;

    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (screen_ensure(COUPLE_VIEW_PAIR) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    pair_cursor_stop();
    pair_hide_all();

    lv_label_set_text(s_pair.title, COUPLE_STR_PAIR_OFFER_TITLE);
    lv_obj_align(s_pair.title, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_remove_flag(s_pair.title, LV_OBJ_FLAG_HIDDEN);

    n = (code != NULL) ? strlen(code) : 0u;
    for (int i = 0; i < kPairDigits; i++) {
        char c = '-';
        if ((size_t)i < n && code[i] >= '0' && code[i] <= '9') c = code[i];
        pair_digit_set(i, c, COL_CREAM);
    }

    if (ttl_secs > 0) {
        lv_label_set_text_fmt(s_pair.ttl, COUPLE_STR_PAIR_TTL_FMT, (unsigned)ttl_secs);
        lv_obj_remove_flag(s_pair.ttl, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(s_pair.hint, COUPLE_STR_PAIR_OFFER_HINT);
    lv_obj_remove_flag(s_pair.hint, LV_OBJ_FLAG_HIDDEN);

    show_view(s_pair.scr, COUPLE_VIEW_PAIR);
    bsp_lvgl_unlock();
}

void couple_ui_show_pair_entry(const uint8_t *digits, uint8_t filled, uint8_t cursor)
{
    lv_obj_t *want;
    lv_obj_t *prev;
    bool cursor_moved;

    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (screen_ensure(COUPLE_VIEW_PAIR) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    want = (cursor < (uint8_t)kPairDigits) ? s_pair.cell[cursor] : NULL;
    prev = s_pair.cursor_cell;
    cursor_moved = (prev != want);
    if (cursor_moved) pair_cursor_stop();

    pair_hide_all();

    lv_label_set_text(s_pair.title, COUPLE_STR_PAIR_ENTRY_TITLE);
    lv_obj_align(s_pair.title, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_remove_flag(s_pair.title, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_pair.hint, COUPLE_STR_PAIR_ENTRY_HINT);
    lv_obj_remove_flag(s_pair.hint, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < kPairDigits; i++) {
        bool confirmed = (digits != NULL) && ((uint8_t)i < filled);
        /* 每一位都要算出数值：当前编辑位还没按 OK 确认，但屏幕上必须能看见正在拨的
         * 数字，否则用户完全不知道自己拨到了几（光标位以前恒显示 '-'）。 */
        char c = (digits != NULL) ? (char)('0' + (int)(digits[i] % 10u)) : '-';

        if (want != NULL && s_pair.cell[i] == want) {
            /* 当前编辑位：描边呼吸，数字用主粉。 */
            pair_cell_default(i);
            lv_obj_set_style_border_color(s_pair.cell[i], COL_PINK, 0);
            lv_obj_set_style_border_width(s_pair.cell[i], 2, 0);
            pair_digit_set(i, c, COL_PINK);
            if (cursor_moved) {
                s_pair.cursor_cell = s_pair.cell[i];
                anim_run(s_pair.cell[i], anim_set_border_opa, 70, 255, 600, 0, 600,
                         LV_ANIM_REPEAT_INFINITE, 0, lv_anim_path_linear);
            }
        } else if (confirmed) {
            /* 已确认：实心深粉底，去掉描边。 */
            lv_obj_set_style_bg_color(s_pair.cell[i], COL_PINK_DEEP, 0);
            lv_obj_set_style_bg_opa(s_pair.cell[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_opa(s_pair.cell[i], LV_OPA_TRANSP, 0);
            pair_digit_set(i, c, COL_CREAM);
        } else {
            pair_digit_set(i, '-', COL_MUTED);
        }
    }

    show_view(s_pair.scr, COUPLE_VIEW_PAIR);
    bsp_lvgl_unlock();
}

void couple_ui_show_pair_status(const char *headline, const char *detail, bool busy, bool ok)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (screen_ensure(COUPLE_VIEW_PAIR) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    pair_cursor_stop();
    pair_hide_all();

    lv_label_set_text(s_pair.title, (headline != NULL) ? headline : "");
    lv_obj_align(s_pair.title, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_remove_flag(s_pair.title, LV_OBJ_FLAG_HIDDEN);

    if (detail != NULL && detail[0] != '\0') {
        lv_label_set_text(s_pair.detail, detail);
        lv_obj_align(s_pair.detail, LV_ALIGN_TOP_MID, 0, 152);
        lv_obj_remove_flag(s_pair.detail, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_color(s_pair.detail, busy ? COL_MUTED : (ok ? COL_MINT : COL_PINK), 0);

    if (busy) {
        lv_obj_remove_flag(s_pair.dot, LV_OBJ_FLAG_HIDDEN);
        /* 同一个 busy 状态会被反复推（长轮询期间），动画已经在跑就别重启，
         * 否则每次推送都把它拉回 80% 起点，看起来像不动的点。 */
        if (lv_anim_get(s_pair.dot, anim_scale_percent) == NULL) {
            anim_run(s_pair.dot, anim_scale_percent, 80, 120, 500, 0, 500, LV_ANIM_REPEAT_INFINITE, 0,
                     lv_anim_path_ease_in_out);
        }
    } else {
        /* 非 busy：转圈点收掉，别留一个半透明的残影。 */
        lv_anim_delete(s_pair.dot, anim_scale_percent);
        set_scale_percent(s_pair.dot, 100);
        lv_obj_add_flag(s_pair.dot, LV_OBJ_FLAG_HIDDEN);
    }

    show_view(s_pair.scr, COUPLE_VIEW_PAIR);
    bsp_lvgl_unlock();
}

void couple_ui_show_pair_confirmed(const char *partner_name)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (screen_ensure(COUPLE_VIEW_PAIR) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    pair_cursor_stop();
    pair_hide_all();

    lv_label_set_text(s_pair.title, COUPLE_STR_PAIR_CONFIRMED_TITLE);
    lv_obj_align(s_pair.title, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_remove_flag(s_pair.title, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_pair.name, (partner_name != NULL) ? partner_name : "");
    lv_obj_align(s_pair.name, LV_ALIGN_TOP_MID, 0, 182);
    lv_obj_remove_flag(s_pair.name, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_pair.hint, COUPLE_STR_PAIR_CONFIRMED_HINT);
    lv_obj_align(s_pair.hint, LV_ALIGN_TOP_MID, 0, 236);
    lv_obj_remove_flag(s_pair.hint, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(s_pair.heart, LV_OBJ_FLAG_HIDDEN);

    show_view(s_pair.scr, COUPLE_VIEW_PAIR);

    anim_run(s_pair.heart, anim_scale_percent, 40, 100, 900, 0, 0, 0, 0, lv_anim_path_overshoot);

    bsp_lvgl_unlock();
}

void couple_ui_show_unpair_prompt(const char *partner_name)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (screen_ensure(COUPLE_VIEW_PAIR) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    pair_cursor_stop();
    pair_hide_all();

    lv_label_set_text(s_pair.title, COUPLE_STR_UNPAIR_TITLE);
    lv_obj_align(s_pair.title, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_remove_flag(s_pair.title, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_pair.name, (partner_name != NULL) ? partner_name : "");
    lv_obj_align(s_pair.name, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_remove_flag(s_pair.name, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_pair.detail, COUPLE_STR_UNPAIR_DETAIL);
    lv_obj_align(s_pair.detail, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_set_style_text_color(s_pair.detail, COL_MUTED, 0);
    lv_obj_remove_flag(s_pair.detail, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_pair.hint, COUPLE_STR_UNPAIR_HINT);
    lv_obj_align(s_pair.hint, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_remove_flag(s_pair.hint, LV_OBJ_FLAG_HIDDEN);

    show_view(s_pair.scr, COUPLE_VIEW_PAIR);
    bsp_lvgl_unlock();
}

void couple_ui_show_prov_ap(const char *ssid, const char *pass, const char *ip, uint8_t clients)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (screen_ensure(COUPLE_VIEW_PROV) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    /* 从 status 页切回来：先把那一刻的转圈点收掉。 */
    lv_anim_delete(s_prov.dot, anim_scale_percent);
    set_scale_percent(s_prov.dot, 100);
    lv_obj_add_flag(s_prov.dot, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_prov.detail, LV_OBJ_FLAG_HIDDEN);
    prov_info_show(true);

    lv_label_set_text(s_prov.title, COUPLE_STR_PROV_AP_TITLE);
    lv_obj_align(s_prov.title, LV_ALIGN_TOP_MID, 0, 32);

    lv_label_set_text_fmt(s_prov.ssid, COUPLE_STR_PROV_SSID_FMT, (ssid != NULL) ? ssid : "");

    prov_set_pass(pass);

    lv_label_set_text_fmt(s_prov.ip, COUPLE_STR_PROV_IP_FMT, (ip != NULL) ? ip : "");

    if (clients > 0) lv_label_set_text_fmt(s_prov.clients, COUPLE_STR_PROV_CLIENTS_FMT, (unsigned)clients);
    else lv_label_set_text(s_prov.clients, COUPLE_STR_PROV_CLIENTS_WAITING);

    lv_label_set_text(s_prov.hint, COUPLE_STR_PROV_HINT);

    show_view(s_prov.scr, COUPLE_VIEW_PROV);
    bsp_lvgl_unlock();
}

void couple_ui_show_prov_status(const char *headline, const char *detail, bool busy, bool ok)
{
    if (!bsp_lvgl_lock(kLockTimeoutMs)) return;
    if (!s_inited) {
        bsp_lvgl_unlock();
        return;
    }

    if (screen_ensure(COUPLE_VIEW_PROV) == NULL) {
        bsp_lvgl_unlock();
        return;
    }

    prov_info_show(false);

    lv_label_set_text(s_prov.title, (headline != NULL) ? headline : "");
    lv_obj_align(s_prov.title, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_remove_flag(s_prov.title, LV_OBJ_FLAG_HIDDEN);

    if (detail != NULL && detail[0] != '\0') {
        lv_label_set_text(s_prov.detail, detail);
        lv_obj_align(s_prov.detail, LV_ALIGN_TOP_MID, 0, 152);
        lv_obj_remove_flag(s_prov.detail, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_color(s_prov.detail, busy ? COL_MUTED : (ok ? COL_MINT : COL_PINK), 0);

    if (busy) {
        lv_obj_remove_flag(s_prov.dot, LV_OBJ_FLAG_HIDDEN);
        if (lv_anim_get(s_prov.dot, anim_scale_percent) == NULL) {
            anim_run(s_prov.dot, anim_scale_percent, 80, 120, 500, 0, 500, LV_ANIM_REPEAT_INFINITE, 0,
                     lv_anim_path_ease_in_out);
        }
    } else {
        lv_anim_delete(s_prov.dot, anim_scale_percent);
        set_scale_percent(s_prov.dot, 100);
        lv_obj_add_flag(s_prov.dot, LV_OBJ_FLAG_HIDDEN);
    }

    show_view(s_prov.scr, COUPLE_VIEW_PROV);
    bsp_lvgl_unlock();
}