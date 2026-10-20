/*
 * Heartlink 情侣心动终端 —— SoftAP 配网实现（热点 + 手机配置页）。
 *
 * 一次 start 的完整动作：
 *   couple_nvs_prepare() → couple_network_prepare() → couple_wifi_begin()
 *   → couple_wifi_ap_start(NULL, COUPLE_PROV_AP_PASS) → httpd_start()
 *   → 注册 GET / 、POST /save 、GET /health → 起 1 Hz 客户端轮询
 *
 * 页面是自包含的：CSS 内联，无任何外部资源，手机在无外网的配网热点里
 * 也能完整渲染；整页约 2.8 KB，留足余量，一次 httpd_resp_send 发完。
 *
 * 安全约定：
 *   - 请求体上限 1024 字节，超了直接 413，不进解析器；
 *   - 先把整个 body 读干净，再做 URL 解码（不做流式解析，省得中途出错）；
 *   - 所有拼接都走有界缓冲（sbuf_*），任何一处放不下就回 500，
 *     绝不出现半截字符串或越界写；
 *   - 校验通过前不回调、不落盘；
 *   - Wi-Fi 密码只在 form_get() → s_pass 之间流转，永远不进日志。
 */

#include "couple_prov.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "couple_config.h"
#include "couple_radio.h"
#include "couple_types.h"
#include "couple_wifi.h"

static const char *TAG = "couple_prov";

/* 表单只有四个短字段，正常请求几十字节；这个值是硬上限，不是期望值。 */
#define COUPLE_PROV_BODY_MAX 1024
/* 页面构造缓冲。固定骨架（含 CSS）1616 + 表单与文案 1190 + 页尾 20
 * + maxlength 的数字 2 = 2828 字节；
 * 最坏情况是热点名 32、服务器地址 96、名字 16 字节，每个字符都转义成
 * 6 字节实体（&quot; 最长），再涨 864 字节，合计 3692 字节。
 * 取 4096 留约 400 字节余量，超了回 500。 */
#define COUPLE_PROV_PAGE_MAX 4096
/* 手机上下线靠轮询发现，1 Hz 足够，也不会给 Wi-Fi 驱动造成压力。 */
#define COUPLE_PROV_POLL_PERIOD_US 1000000

static const char *k_form_type = "application/x-www-form-urlencoded";
static const char *k_page_type = "text/html; charset=utf-8";
static const char *k_text_type = "text/plain; charset=utf-8";

/*
 * 页面外壳。成功页、错误页、配置页共用同一份 CSS，省 flash 也省得样式跑偏。
 * 深色可爱风：暗紫底 + 粉色爱心 + 圆角卡片。
 */
static const char *k_page_head =
    "<!DOCTYPE html>"
    "<html lang=\"zh-CN\"><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Heartlink 配网</title>"
    "<style>"
    ":root{color-scheme:dark}"
    "*{box-sizing:border-box}"
    "body{margin:0;padding:24px 16px;min-height:100vh;color:#f4eefb;"
    "font-family:-apple-system,\"PingFang SC\",\"Microsoft YaHei\",sans-serif;"
    "background:radial-gradient(circle at 50% 0%,#2a1f3d,#171220 62%)}"
    ".card{max-width:420px;margin:0 auto;padding:24px 20px;border-radius:20px;"
    "background:#221a33;border:1px solid #3a2c55;box-shadow:0 12px 32px rgba(0,0,0,.45)}"
    ".logo{font-size:40px;line-height:1;text-align:center;color:#ff7aa8}"
    "h1{font-size:20px;text-align:center;margin:12px 0 4px}"
    ".sub{font-size:13px;line-height:1.7;text-align:center;color:#b6a8cc;margin:0 0 16px}"
    ".tip{padding:10px 12px;margin-bottom:14px;border-radius:12px;"
    "font-size:13px;background:#2c2140;color:#cbbde3}"
    ".tip.on{background:#2b3a2c;color:#a8e6b0}"
    ".ap{font-size:12px;text-align:center;color:#9c8fb5;margin:0 0 18px}"
    ".ap b{color:#ff9ec1}"
    "label{display:block;font-size:13px;color:#c9bcdd;margin:14px 0 6px}"
    "input{width:100%;padding:11px 12px;font-size:15px;color:#fff;"
    "border-radius:12px;border:1px solid #443458;background:#1b1529}"
    "input:focus{outline:none;border-color:#ff7aa8;box-shadow:0 0 0 3px rgba(255,122,168,.18)}"
    "button{width:100%;margin-top:20px;padding:13px;font-size:16px;font-weight:600;"
    "color:#2a1020;border:0;border-radius:14px;cursor:pointer;"
    "background:linear-gradient(135deg,#ff9ec1,#ff7aa8)}"
    ".foot{font-size:12px;line-height:1.7;text-align:center;color:#7d7195;margin:18px 0 0}"
    "a{color:#ff9ec1}"
    "</style></head><body><div class=\"card\">";

static const char *k_page_tail = "</div></body></html>";

/* ---------- 模块状态 ---------- */

static couple_prov_hooks_t s_hooks;
static httpd_handle_t s_server;
static esp_timer_handle_t s_poll;
static bool s_active;
static bool s_last_client;
static char s_relay[COUPLE_RELAY_URL_MAX + 1]; /* 表单预填 / 最近一次提交 */
static char s_self_name[COUPLE_NAME_MAX + 1u]; /* 「你的名字」预填 */
static char s_name[COUPLE_NAME_MAX * 4u + 1u]; /* 最近一次提交的名字；按 4 倍留余量，
                                                * 让超长值在解码阶段放得下、好报错 */
static char s_ssid[COUPLE_WIFI_SSID_MAX];      /* 最近一次提交 */
static char s_pass[COUPLE_WIFI_PASS_MAX];      /* 最近一次提交，绝不进日志 */
static char s_body[COUPLE_PROV_BODY_MAX + 1];  /* 请求体 */
static char s_page[COUPLE_PROV_PAGE_MAX];      /* 页面构造缓冲 */
static char s_ap_ssid[COUPLE_WIFI_SSID_MAX];   /* 本模块起的热点名 */

/* ---------- 有界字符串拼装 ---------- */

/*
 * 极简拼装器：放不下就把 overflow 置起来，之后所有写入都变成空操作。
 * 调用方只在最后检查一次 overflow，中间不需要判空。
 */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} sbuf_t;

static void sbuf_init(sbuf_t *sb, char *buf, size_t cap)
{
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->overflow = false;

    if (cap > 0) {
        buf[0] = '\0';
    }
}

static void sbuf_put(sbuf_t *sb, const char *text, size_t len)
{
    if (sb->overflow || len == 0) {
        return;
    }
    /* cap == 0 时下面的减法会下溢，先单独挡掉；并留出结尾的 NUL */
    if (sb->cap == 0 || len >= sb->cap - sb->len) {
        sb->overflow = true;
        return;
    }

    memcpy(sb->buf + sb->len, text, len);
    sb->len += len;
    sb->buf[sb->len] = '\0';
}

static void sbuf_add(sbuf_t *sb, const char *text)
{
    sbuf_put(sb, text, strlen(text));
}

/* 拼用户输入时必须走这里：SSID 和服务器地址都可能带 < > " & 这类字符。 */
static void sbuf_add_escaped(sbuf_t *sb, const char *text)
{
    for (const char *p = text; *p != '\0' && !sb->overflow; p++) {
        switch (*p) {
        case '&':
            sbuf_add(sb, "&amp;");
            break;
        case '<':
            sbuf_add(sb, "&lt;");
            break;
        case '>':
            sbuf_add(sb, "&gt;");
            break;
        case '"':
            sbuf_add(sb, "&quot;");
            break;
        case '\'':
            sbuf_add(sb, "&#39;");
            break;
        default:
            /* UTF-8 多字节字符走这里，逐字节照抄即可 */
            sbuf_put(sb, p, 1);
            break;
        }
    }
}

/* ---------- 表单解析 ---------- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/*
 * 百分号解码：'+' 还原成空格，"%XX" 还原成字节，其余照抄。
 *
 * 出现残缺转义、非十六进制字符，或解码结果里出现 NUL 字节，一律判失败——
 * 否则 "\0" 之类的手工构造能把后面的长度校验整个绕过。
 * 失败时把 out 清空，保证它始终是个合法 C 串。
 */
static bool url_decode(const char *src, size_t src_len, char *out, size_t cap)
{
    size_t w = 0;

    if (cap == 0) {
        return false;
    }

    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];

        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= src_len) {
                out[0] = '\0';
                return false;
            }
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi < 0 || lo < 0) {
                out[0] = '\0';
                return false;
            }
            int decoded = (hi << 4) | lo;
            if (decoded == 0) {
                out[0] = '\0';
                return false;
            }
            c = (char)decoded;
            i += 2;
        }

        if (w + 1 >= cap) {
            out[0] = '\0';
            return false;
        }
        out[w++] = c;
    }

    out[w] = '\0';
    return true;
}

/*
 * 从表单体里取一个字段并解码。
 *
 * 手写解析而不用 httpd 自带的 query 接口，是因为配置页的字段里可能出现
 * 未转义的 '&'（用户手抄的地址），这里逐个字段按 key 精确定位，语义更直白。
 * 全程用下标遍历，不构造任何可能越界的指针。
 */
static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t key_len = strlen(key);
    size_t body_len = strlen(body);
    size_t pos = 0;

    while (pos <= body_len) {
        size_t next = pos;
        while (next < body_len && body[next] != '&') {
            next++;
        }

        size_t eq = pos;
        while (eq < next && body[eq] != '=') {
            eq++;
        }

        if (eq < next && eq - pos == key_len && memcmp(body + pos, key, key_len) == 0) {
            size_t value_len = next - (eq + 1); /* eq + 1 <= next <= body_len */
            return url_decode(body + eq + 1, value_len, out, cap);
        }

        if (next >= body_len) {
            break;
        }
        pos = next + 1;
    }

    return false;
}

/*
 * 只接受表单编码。请求头缺失时按宽松策略放行——手机浏览器一定会带，
 * 缺头的基本只有手工 curl 调试。
 */
static bool content_type_is_form(httpd_req_t *req)
{
    char value[64] = {0};
    size_t len = httpd_req_get_hdr_value_len(req, "Content-Type");

    if (len == 0) {
        return true;
    }
    if (len >= sizeof(value)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Content-Type", value, sizeof(value)) != ESP_OK) {
        return false;
    }

    return strncmp(value, k_form_type, strlen(k_form_type)) == 0;
}

/*
 * 读请求体的结果。
 *
 * 失败原因要分开上报，因为每种原因对应不同的状态码与文案；而文案的渲染
 * 统一留给 send_result_page()——httpd_resp_send_err() 发出去的
 * Content-Type 是裸的 "text/html"（见 httpd_txrx.c 的 httpd_resp_send_err），
 * 不带 charset，中文要靠浏览器猜编码，而且错误页上还没有返回配网页的入口。
 */
typedef enum {
    BODY_READ_OK = 0,
    BODY_READ_EMPTY,     /* 没有请求体 → 400 */
    BODY_READ_TOO_LARGE, /* 超过 COUPLE_PROV_BODY_MAX → 413 */
    BODY_READ_TIMEOUT,   /* 收包超时 → 408 */
    BODY_READ_FAILED,    /* 连接已经坏了，回什么也发不出去 */
} body_read_t;

/*
 * 读完整请求体到 s_body 并补 NUL。本函数不发任何响应。
 *
 * 超限时不再读剩下的体：handler 返回后 httpd 会走 httpd_req_delete()，
 * 把没读完的字节一次性丢弃，不会污染同一条连接上的下一条请求。
 */
static body_read_t read_body(httpd_req_t *req)
{
    if (req->content_len == 0) {
        return BODY_READ_EMPTY;
    }
    if (req->content_len > COUPLE_PROV_BODY_MAX) {
        ESP_LOGW(TAG, "配网请求体过大：%u 字节", (unsigned)req->content_len);
        return BODY_READ_TOO_LARGE;
    }

    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, s_body + got, req->content_len - got);

        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "读取配网请求体超时");
            return BODY_READ_TIMEOUT;
        }
        if (n <= 0) {
            ESP_LOGW(TAG, "读取配网请求体失败：%d", n);
            return BODY_READ_FAILED;
        }
        got += (size_t)n;
    }

    s_body[got] = '\0';
    return BODY_READ_OK;
}

/* ---------- 页面渲染 ---------- */

/* 配网首页：热点名、手机连接提示、四个输入框（服务器地址与名字预填）。 */
static bool render_config_page(sbuf_t *sb)
{
    const char *ap_ssid = (s_ap_ssid[0] != '\0') ? s_ap_ssid : COUPLE_PROV_AP_SSID_PREFIX;
    char name_max[8];

    sbuf_init(sb, s_page, sizeof(s_page));
    sbuf_add(sb, k_page_head);

    sbuf_add(sb, "<div class=\"logo\">&#9829;</div>");
    sbuf_add(sb, "<h1>Heartlink 配网</h1>");
    sbuf_add(sb, "<p class=\"sub\">让心跳先连上网络</p>");

    /* 只有手机真的连上了，提示才该变成「可以填了」 */
    if (couple_wifi_ap_client_count() > 0) {
        sbuf_add(sb, "<div class=\"tip on\">手机已连接，可以填写下面的信息了。</div>");
    } else {
        sbuf_add(sb, "<div class=\"tip\">正在等手机连上热点……</div>");
    }

    sbuf_add(sb, "<p class=\"ap\">当前热点：<b>");
    sbuf_add_escaped(sb, ap_ssid);
    sbuf_add(sb, "</b></p>");

    sbuf_add(sb, "<form action=\"/save\" method=\"post\">");

    sbuf_add(sb, "<label for=\"ssid\">家里的 Wi-Fi 名称</label>");
    sbuf_add(sb, "<input id=\"ssid\" name=\"ssid\" type=\"text\" maxlength=\"32\" "
                 "autocomplete=\"off\" placeholder=\"例如 Home-2.4G\" required>");

    sbuf_add(sb, "<label for=\"pass\">Wi-Fi 密码</label>");
    sbuf_add(sb, "<input id=\"pass\" name=\"pass\" type=\"password\" maxlength=\"64\" "
                 "autocomplete=\"off\" placeholder=\"没有密码就留空\">");

    sbuf_add(sb, "<label for=\"url\">Heartlink 服务器地址</label>");
    sbuf_add(sb, "<input id=\"url\" name=\"url\" type=\"text\" maxlength=\"95\" "
                 "autocomplete=\"off\" required value=\"");
    sbuf_add_escaped(sb, s_relay);
    sbuf_add(sb, "\">");

    /*
     * maxlength 给的是 COUPLE_NAME_MAX 的「字节」上限，不是字数：
     * 浏览器按字符计数（一个汉字算 1），所以中文名字填到 5 个字以内都能过服务端；
     * 真正的字节上限由 prov_post_save() 兜住，这里只负责别让人多打。
     */
    (void)snprintf(name_max, sizeof(name_max), "%u", (unsigned)COUPLE_NAME_MAX);

    sbuf_add(sb, "<label for=\"name\">你的名字</label>");
    sbuf_add(sb, "<input id=\"name\" name=\"name\" type=\"text\" maxlength=\"");
    sbuf_add(sb, name_max);
    sbuf_add(sb, "\" autocomplete=\"off\" placeholder=\"对方会看到这个名字\" value=\"");
    sbuf_add_escaped(sb, s_self_name);
    sbuf_add(sb, "\">");

    sbuf_add(sb, "<button type=\"submit\">保存并连接</button>");
    sbuf_add(sb, "</form>");

    sbuf_add(sb, "<p class=\"foot\">保存后设备会立刻去连你家的 Wi-Fi，"
                 "热点随后自动关闭。<br>服务器地址要以 http:// 或 https:// 开头。"
                 "<br>名字可以留空，留空就保持原来的名字。</p>");
    sbuf_add(sb, k_page_tail);

    return !sb->overflow;
}

/*
 * 结果页（保存成功 / 各种校验失败共用）。
 * status 传 NULL 表示 200；title 与 message 都是本文件里的固定中文，无需转义。
 */
static esp_err_t send_result_page(httpd_req_t *req, const char *status, const char *title,
                                  const char *message)
{
    sbuf_t sb;

    sbuf_init(&sb, s_page, sizeof(s_page));
    sbuf_add(&sb, k_page_head);
    sbuf_add(&sb, "<div class=\"logo\">&#9829;</div><h1>");
    sbuf_add(&sb, title);
    sbuf_add(&sb, "</h1><p class=\"sub\">");
    sbuf_add(&sb, message);
    sbuf_add(&sb, "</p><p class=\"foot\"><a href=\"/\">返回配网页</a></p>");
    sbuf_add(&sb, k_page_tail);

    if (sb.overflow) {
        /* 只有在静态骨架被改坏时才会走到这里 */
        ESP_LOGE(TAG, "结果页超出缓冲");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "页面生成失败");
    }

    if (status != NULL) {
        (void)httpd_resp_set_status(req, status);
    }
    (void)httpd_resp_set_type(req, k_page_type);
    return httpd_resp_send(req, s_page, HTTPD_RESP_USE_STRLEN);
}

/* 校验失败统一出口：回中文错误页，且绝不回调 on_saved。 */
static esp_err_t send_reject(httpd_req_t *req, const char *title, const char *message)
{
    ESP_LOGW(TAG, "配网提交被拒绝：%s", title);
    return send_result_page(req, HTTPD_400, title, message);
}

/* ---------- URI 处理 ---------- */

static esp_err_t prov_get_root(httpd_req_t *req)
{
    sbuf_t sb;

    if (!render_config_page(&sb)) {
        ESP_LOGE(TAG, "配置页超出缓冲");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "页面生成失败");
    }

    (void)httpd_resp_set_type(req, k_page_type);
    return httpd_resp_send(req, s_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t prov_post_save(httpd_req_t *req)
{
    if (!content_type_is_form(req)) {
        return send_reject(req, "提交格式不对", "请回到配网页，直接用页面上的按钮提交。");
    }

    switch (read_body(req)) {
    case BODY_READ_OK:
        break;
    case BODY_READ_EMPTY:
        return send_reject(req, "提交是空的", "请回到配网页，填好信息再从页面提交。");
    case BODY_READ_TOO_LARGE:
        /* HTTPD_413_CONTENT_TOO_LARGE 只是错误码，5.5.3 没给出对应的状态串宏 */
        return send_result_page(req, "413 Content Too Large", "提交的内容太大了",
                                "请回到配网页，把内容改短一点再提交。");
    case BODY_READ_TIMEOUT:
        return send_result_page(req, HTTPD_408, "提交超时", "网络有点慢，请回到配网页重试一次。");
    default:
        return ESP_FAIL; /* 连接已经坏了，回什么都发不出去 */
    }

    /* 四个字段都不能缺；密码和名字可以是空串，但必须带 "pass=" / "name=" 出现 */
    if (!form_get(s_body, "ssid", s_ssid, sizeof(s_ssid)) ||
        !form_get(s_body, "pass", s_pass, sizeof(s_pass)) ||
        !form_get(s_body, "url", s_relay, sizeof(s_relay)) ||
        !form_get(s_body, "name", s_name, sizeof(s_name))) {
        return send_reject(req, "表单没读明白", "字段缺失或编码有误，请回到配网页重填一次。");
    }

    size_t len = strlen(s_ssid);
    if (len == 0 || len > COUPLE_WIFI_SSID_MAX - 1) {
        return send_reject(req, "Wi-Fi 名称不合法", "名称必须是 1~32 个字节，请检查后重填。");
    }

    /* 允许空密码（开放网络）；大于上限就直接挡掉，不要交给驱动去报错 */
    if (strlen(s_pass) > COUPLE_WIFI_PASS_MAX - 1) {
        return send_reject(req, "Wi-Fi 密码太长", "密码最多 64 个字节，请检查后重填。");
    }

    len = strlen(s_relay);
    if (len == 0 || len >= COUPLE_RELAY_URL_MAX) {
        return send_reject(req, "服务器地址不合法", "地址要在 1~95 个字节之间，请检查后重填。");
    }
    if (strncmp(s_relay, "http://", 7) != 0 && strncmp(s_relay, "https://", 8) != 0) {
        return send_reject(req, "服务器地址不合法", "地址要以 http:// 或 https:// 开头。");
    }

    /*
     * 名字可以留空，留空表示「保持当前名字不变」，由应用决定不改 NVS。
     * 有值就按 store 的口径校验：上限是 UTF-8 字节数、不收控制字符，
     * 而且不能只有空白（清洗后会变成空串，等于悄悄什么都没改）。
     * s_name 按上限的 4 倍留了余量，17~64 字节的超长名字能明确报「太长」，
     * 再长的（解码后超过 64 字节）在解码阶段就失败，落到上面的通用文案。
     */
    len = strlen(s_name);
    if (len > COUPLE_NAME_MAX) {
        return send_reject(req, "你的名字太长", "名字最多 16 个字节（一个汉字占 3 个字节），请改短一点。");
    }
    bool only_spaces = (len > 0u);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s_name[i];
        if (c < 0x20u || c == 0x7Fu) {
            return send_reject(req, "你的名字不合法", "名字里不能有换行、制表符之类的控制字符。");
        }
        if (c != ' ') {
            only_spaces = false;
        }
    }
    if (only_spaces) {
        return send_reject(req, "你的名字不合法", "名字不能只有空格；想保持原来的名字就留空。");
    }

    /*
     * 到这里四个值都已经在模块静态缓冲里，回调期间指针有效、内容不会被人动。
     * 日志只打 SSID、服务器地址与名字——密码永远不出现在日志里。
     */
    ESP_LOGI(TAG, "收到配网信息：ssid=%s url=%s name=%s", s_ssid, s_relay, s_name);
    if (s_hooks.on_saved != NULL) {
        s_hooks.on_saved(s_ssid, s_pass, s_relay, s_name, s_hooks.user);
    }

    return send_result_page(req, NULL, "保存成功", "设备这就去连你家的 Wi-Fi，稍等片刻。");
}

/* 给自动化脚本和调试用的极简探活接口。 */
static esp_err_t prov_get_health(httpd_req_t *req)
{
    (void)httpd_resp_set_type(req, k_text_type);
    return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

/* 用静态表注册，省得 handler 与 URI 的对应关系散在各处。 */
static const httpd_uri_t k_uris[] = {
    {.uri = "/", .method = HTTP_GET, .handler = prov_get_root, .user_ctx = NULL},
    {.uri = "/save", .method = HTTP_POST, .handler = prov_post_save, .user_ctx = NULL},
    {.uri = "/health", .method = HTTP_GET, .handler = prov_get_health, .user_ctx = NULL},
};

#define COUPLE_PROV_URI_COUNT (sizeof(k_uris) / sizeof(k_uris[0]))

/* ---------- 客户端上下线轮询 ---------- */

/*
 * couple_wifi 只上报 STA 事件，没有 AP 客户端事件可用，
 * 所以用 1 Hz 轮询把「手机接入 / 断开」转成 on_status。
 * 回调跑在 esp_timer 任务里，只允许做瞬时动作。
 */
static void poll_timer_cb(void *arg)
{
    (void)arg;

    if (!s_active) {
        return;
    }

    bool connected = (couple_wifi_ap_client_count() > 0);
    if (connected == s_last_client) {
        return;
    }
    s_last_client = connected;

    ESP_LOGI(TAG, "配网热点：%s", connected ? "手机已接入" : "手机已断开");
    if (s_hooks.on_status != NULL) {
        s_hooks.on_status(connected, s_hooks.user);
    }
}

/* 轮询起不来只影响连接提示，不影响配网本身，所以失败只记一条 warn。 */
static void start_poll_timer(void)
{
    esp_timer_create_args_t args = {
        .callback = poll_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "couple_prov",
        .skip_unhandled_events = true,
    };

    esp_err_t err = esp_timer_create(&args, &s_poll);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_poll, COUPLE_PROV_POLL_PERIOD_US);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "客户端状态轮询未启动：%s", esp_err_to_name(err));
        if (s_poll != NULL) {
            (void)esp_timer_delete(s_poll);
            s_poll = NULL;
        }
    }
}

/* ---------- 对外接口 ---------- */

esp_err_t couple_prov_start(const couple_prov_hooks_t *hooks, const char *current_relay_url,
                            const char *self_name)
{
    if (s_active) {
        return ESP_OK;
    }

    /* hooks 按值存下来：调用方通常传的是局部变量 */
    if (hooks != NULL) {
        s_hooks = *hooks;
    } else {
        memset(&s_hooks, 0, sizeof(s_hooks));
    }

    /* 表单预填。snprintf 自带截断，调用方给超长串也不会越界 */
    const char *relay = (current_relay_url != NULL && current_relay_url[0] != '\0')
                            ? current_relay_url
                            : COUPLE_DEFAULT_RELAY_URL;
    (void)snprintf(s_relay, sizeof(s_relay), "%s", relay);

    /* 名字没有「兜底默认值」这一说：传 NULL 就预填空串，用户不填则保持原值 */
    (void)snprintf(s_self_name, sizeof(s_self_name), "%s",
                   (self_name != NULL) ? self_name : "");

    esp_err_t err = couple_nvs_prepare();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 准备失败：%s", esp_err_to_name(err));
        return err;
    }

    err = couple_network_prepare();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "网络栈准备失败：%s", esp_err_to_name(err));
        return err;
    }

    err = couple_wifi_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 驱动初始化失败：%s", esp_err_to_name(err));
        return err;
    }

    /* ssid 传 NULL：由 couple_wifi 按 MAC 末两字节拼出 "Heartlink-XXXX" */
    err = couple_wifi_ap_start(NULL, COUPLE_PROV_AP_PASS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "开启配网热点失败：%s", esp_err_to_name(err));
        return err;
    }

    const char *ap_ssid = couple_wifi_ap_ssid();
    (void)snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s",
                   (ap_ssid != NULL && ap_ssid[0] != '\0') ? ap_ssid : COUPLE_PROV_AP_SSID_PREFIX);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = COUPLE_PROV_HTTP_PORT;
    cfg.max_uri_handlers = 6;
    cfg.max_open_sockets = 4; /* 配网场景只有一台手机，4 个足够且省内存 */
    cfg.lru_purge_enable = true; /* 手机浏览器会开多条连接，旧的必须能被挤掉 */
    cfg.stack_size = 6144;       /* 要跑字符串处理与 KV 落盘，默认 4096 偏紧 */
    cfg.task_priority = 5;
    cfg.core_id = tskNO_AFFINITY;

    err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置页服务启动失败：%s", esp_err_to_name(err));
        /* 页面起不来就不要留着热点，否则手机连上了却什么也看不到 */
        couple_wifi_ap_stop();
        s_ap_ssid[0] = '\0';
        return err;
    }

    for (size_t i = 0; i < COUPLE_PROV_URI_COUNT; i++) {
        err = httpd_register_uri_handler(s_server, &k_uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败：%s", k_uris[i].uri, esp_err_to_name(err));
            couple_prov_stop(); /* 把已经起好的部分整体收回去 */
            return err;
        }
    }

    s_last_client = (couple_wifi_ap_client_count() > 0);
    s_active = true;

    start_poll_timer();

    ESP_LOGI(TAG, "配网已开启：热点 %s，配置页 http://192.168.4.1/", s_ap_ssid);

    /* 先按当前状态报一次，UI 不用等第一个轮询周期 */
    if (s_hooks.on_status != NULL) {
        s_hooks.on_status(s_last_client, s_hooks.user);
    }

    return ESP_OK;
}

void couple_prov_stop(void)
{
    /*
     * 顺序刻意与「停 httpd → 注销 handler」的直觉相反：
     * httpd_stop() 内部会连着 httpd_data 一起 free（见 httpd_main.c 的
     * httpd_stop → httpd_delete → httpd_unregister_all_uri_handlers），
     * 停完再注销就是 use-after-free。所以必须先注销再停服务。
     */
    if (s_server != NULL) {
        for (size_t i = 0; i < COUPLE_PROV_URI_COUNT; i++) {
            (void)httpd_unregister_uri_handler(s_server, k_uris[i].uri, k_uris[i].method);
        }
        esp_err_t err = httpd_stop(s_server);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "配置页服务停止异常：%s", esp_err_to_name(err));
        }
        s_server = NULL;
    }

    if (s_poll != NULL) {
        (void)esp_timer_stop(s_poll);
        (void)esp_timer_delete(s_poll);
        s_poll = NULL;
    }

    /* 本模块是 AP 模式的唯一使用者，这里无条件关一次；
     * couple_wifi_ap_stop() 本身幂等，没开也不会报错。 */
    couple_wifi_ap_stop();

    /* 清掉回调：stop 返回之后保证不会再有任何 hook 被触发 */
    memset(&s_hooks, 0, sizeof(s_hooks));
    memset(s_ssid, 0, sizeof(s_ssid));
    memset(s_pass, 0, sizeof(s_pass)); /* 密码不在内存里留驻留副本 */
    s_ap_ssid[0] = '\0';
    s_last_client = false;
    s_active = false;

    ESP_LOGI(TAG, "配网已关闭");
}

bool couple_prov_active(void)
{
    return s_active;
}

const char *couple_prov_ap_ssid(void)
{
    return s_ap_ssid;
}