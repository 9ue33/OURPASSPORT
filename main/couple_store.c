#include "couple_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "couple_config.h"
#include "couple_radio.h"

static const char *TAG = "couple_store";

#define STORE_NS "couple"
#define STORE_KEY_BLOB "blob"

/*
 * 旧固件把本机名字的默认值写死并原样存进了 NVS，所以只改
 * COUPLE_DEFAULT_SELF_NAME 对已经存过档的机器不生效。
 * repair_blob() 见到这些历史默认值就顺手迁到当前默认值。
 *
 * "PASSPORT" 是出厂工程自带的默认名，"WREN" 是上一版我们自己写死的默认名。
 *
 * 代价：用户真的给自己起名叫 "PASSPORT" 或 "WREN" 时也会被改写，而且以后每次启动
 * 都会再改一次。本阶段接受这个取舍——换来的是旧机器不用重新配网就能显示新默认名字。
 * 反过来说：任何一次「把默认名改掉」的升级，都要把上一版的默认名加进这个列表。
 */
#define STORE_LEGACY_DEFAULT_NAME "PASSPORT"
#define STORE_LEGACY_DEFAULT_NAME_PREV "WREN"

static couple_store_blob_t s_blob;
static bool s_ready;

/* 所有对 s_blob 的读写都必须先拿到这把锁（init 之前的单线程调用除外）。 */
static SemaphoreHandle_t s_lock;

/* 已经落盘的 seq 水位，用来判断「涨够一档了没有」。 */
static uint32_t s_persisted_seq;

static uint32_t s_dirty; /* 一般改动的脏计数，到 COUPLE_NVS_FLUSH_DIRTY_LIMIT 就写 */
static bool s_seq_dirty; /* seq 前进到了需要落盘的一档 */
static bool s_have_tick;
static uint32_t s_last_flush_ms;

/* ---------- 锁 ---------- */

static void store_lock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void store_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

static void mark_dirty_locked(void)
{
    if (s_dirty < COUPLE_NVS_FLUSH_DIRTY_LIMIT) {
        s_dirty++;
    }
}

/* ---------- Flash ---------- */

/*
 * 写一次 Flash。调用方必须持锁。
 * 失败时不动脏标记，交给 couple_store_tick 稍后重试——
 * 这里绝不做擦除：NVS 真的坏了由 couple_nvs_prepare() 在下次启动时处理，
 * 而在运行期擦分区会连带清掉身份与配对，代价远大于晚几秒落盘。
 */
static esp_err_t flush_locked(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORE_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, STORE_KEY_BLOB, &s_blob, sizeof(s_blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "落盘失败: %s（保留脏标记，稍后重试）", esp_err_to_name(err));
        return err;
    }

    s_dirty = 0u;
    s_seq_dirty = false;
    s_persisted_seq = s_blob.sync_seq;
    return ESP_OK;
}

/* 写不进去就标脏，保证下次 tick 还会再试。调用方必须持锁。 */
static esp_err_t flush_or_mark_locked(void)
{
    esp_err_t err = flush_locked();
    if (err != ESP_OK) {
        mark_dirty_locked();
    }
    return err;
}

/* ---------- 修复 ---------- */

/*
 * 生成一个不可能全零的设备身份。
 * 随机为主，掺入本机 MAC 作为兜底（万一 esp_random 尚未就绪也不会退化成全零）。
 */
static void fill_device_id(uint8_t *out)
{
    uint8_t mac[6] = {0};

    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);

    for (size_t i = 0; i < COUPLE_DEVICE_ID_LEN; i += 4u) {
        uint32_t r = esp_random();
        out[i + 0u] = (uint8_t)(r >> 24);
        out[i + 1u] = (uint8_t)(r >> 16);
        out[i + 2u] = (uint8_t)(r >> 8);
        out[i + 3u] = (uint8_t)r;
    }
    for (size_t i = 0; i < sizeof(mac); ++i) {
        out[i] ^= mac[i];
    }
    if (couple_store_blob_id_is_empty(out, COUPLE_DEVICE_ID_LEN)) {
        out[0] = 0x01u;
    }
}

/*
 * 把从 Flash 读出来的东西整成一具备用数据。
 *
 * 关键红线：统计坏了只重置统计，配对关系一个字都不动。
 * 反过来，配对三元组残缺时只清 PAIRED 标志，也不会顺手抹掉互动记忆。
 */
/*
 * 旧固件还用过「随机码」当默认名字：小写字母 + 数字，5~6 位（真机上出现过 "9ue33"）。
 * 那不是用户起的名字，留在 NVS 里只会让对端一直看到一串看不懂的字符，
 * 所以也按历史默认值处理，一并迁到当前默认名。
 *
 * 判定刻意收紧：必须同时含数字和字母，像 "lover"（纯字母）这种用户可能真会起的名字不会被误伤。
 */
static bool name_is_legacy_code(const char *name)
{
    const size_t len = strlen(name);
    size_t digits = 0u;
    size_t letters = 0u;

    if (len < 5u || len > 6u) {
        return false;
    }

    for (size_t i = 0u; i < len; i++) {
        const char c = name[i];

        if (c >= '0' && c <= '9') {
            digits++;
        } else if (c >= 'a' && c <= 'z') {
            letters++;
        } else {
            return false;
        }
    }

    return digits > 0u && letters > 0u;
}

static void repair_blob(couple_store_blob_t *blob)
{
    /* 先把所有字符串在最后一个字节上强制封口：NVS 里的旧数据不保证有 NUL。 */
    blob->partner_name[COUPLE_NAME_MAX] = '\0';
    blob->self_name[COUPLE_NAME_MAX] = '\0';
    blob->relay_url[COUPLE_RELAY_URL_MAX - 1u] = '\0';
    blob->wifi_ssid[COUPLE_WIFI_SSID_MAX - 1u] = '\0';
    blob->wifi_pass[COUPLE_WIFI_PASS_MAX - 1u] = '\0';

    /* 标志位以内容为准，谁也别信标志自己。 */
    if (couple_store_blob_is_paired(blob)) {
        blob->flags |= COUPLE_STORE_FLAG_PAIRED;
    } else {
        blob->flags &= (uint16_t)~COUPLE_STORE_FLAG_PAIRED;
    }
    if (blob->wifi_ssid[0] != '\0') {
        blob->flags |= COUPLE_STORE_FLAG_HAS_WIFI;
    } else {
        blob->flags &= (uint16_t)~COUPLE_STORE_FLAG_HAS_WIFI;
    }

    if (couple_store_blob_id_is_empty(blob->device_id, COUPLE_DEVICE_ID_LEN)) {
        fill_device_id(blob->device_id);
    }

    if (blob->relay_url[0] == '\0') {
        couple_store_copy(blob->relay_url, sizeof(blob->relay_url), COUPLE_DEFAULT_RELAY_URL);
    }

    if (blob->self_name[0] == '\0') {
        couple_store_copy(blob->self_name, sizeof(blob->self_name), COUPLE_DEFAULT_SELF_NAME);
    } else if (strcmp(blob->self_name, STORE_LEGACY_DEFAULT_NAME) == 0 ||
               strcmp(blob->self_name, STORE_LEGACY_DEFAULT_NAME_PREV) == 0 ||
               name_is_legacy_code(blob->self_name)) {
        /* 旧固件把默认名字原样写进了 NVS，光改默认值不会生效，这里就地迁移。 */
        couple_store_copy(blob->self_name, sizeof(blob->self_name), COUPLE_DEFAULT_SELF_NAME);
        ESP_LOGI(TAG, "本机名字仍是旧默认值，已迁移为 \"%s\"", blob->self_name);
    }

    if (!couple_state_valid(&blob->stats)) {
        ESP_LOGW(TAG, "统计数据结构不符，重置统计（配对关系保留）");
        couple_state_init(&blob->stats);
    }
}

/* ---------- 生命周期 ---------- */

esp_err_t couple_store_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "互斥锁创建失败");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t result = couple_nvs_prepare();

    couple_store_blob_t loaded;
    couple_store_blob_init(&loaded);
    bool loaded_from_nvs = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORE_NS, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        size_t len = sizeof(loaded);
        err = nvs_get_blob(handle, STORE_KEY_BLOB, &loaded, &len);
        if (err == ESP_OK) {
            if (len != sizeof(loaded)) {
                ESP_LOGW(TAG, "存档长度 %u 与当前结构 %u 不符，按出厂状态重建",
                         (unsigned)len, (unsigned)sizeof(loaded));
                err = ESP_ERR_INVALID_SIZE;
            } else if (!couple_store_blob_valid(&loaded)) {
                ESP_LOGW(TAG, "存档校验未通过，按出厂状态重建");
                err = ESP_ERR_INVALID_STATE;
            }
        }
        if (err == ESP_OK) {
            loaded_from_nvs = true;
        } else {
            if (err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "读取存档失败: %s", esp_err_to_name(err));
            }
            couple_store_blob_init(&loaded);
            err = ESP_OK;
        }
        nvs_close(handle);
    } else {
        ESP_LOGE(TAG, "打开 NVS 命名空间失败: %s", esp_err_to_name(err));
        result = err;
    }

    repair_blob(&loaded);

    /* seq 抬到「已落盘值 + BOOT_MARGIN」：跨重启单调，又不至于每次同步都写 Flash。 */
    uint32_t persisted = loaded.sync_seq;
    uint32_t next = persisted + COUPLE_SEQ_BOOT_MARGIN;
    if (next < persisted) {
        next = 0xFFFFFFFFu; /* 回绕兜底：宁可贴着上限，也绝不往后退 */
    }
    loaded.sync_seq = next;

    store_lock();
    s_blob = loaded;
    s_persisted_seq = persisted;
    s_dirty = 0u;
    s_seq_dirty = false;
    s_ready = true;
    esp_err_t flush_err = flush_or_mark_locked();
    store_unlock();

    if (couple_nvs_was_wiped()) {
        ESP_LOGW(TAG, "NVS 曾被擦除重建，身份与配对数据已重置");
    }
    ESP_LOGI(TAG, "已加载（%s）：设备 %02x%02x%02x%02x，seq=%u，relay=%s，self=\"%s\"，partner=\"%s\"",
             loaded_from_nvs ? "来自 Flash" : "出厂默认",
             (unsigned)loaded.device_id[0], (unsigned)loaded.device_id[1],
             (unsigned)loaded.device_id[2], (unsigned)loaded.device_id[3],
             (unsigned)loaded.sync_seq, loaded.relay_url, loaded.self_name, loaded.partner_name);

    if (flush_err != ESP_OK && result == ESP_OK) {
        result = flush_err;
    }
    return result;
}

const couple_store_blob_t *couple_store(void)
{
    return &s_blob;
}

void couple_store_snapshot(couple_store_blob_t *out)
{
    if (out == NULL) {
        return;
    }
    store_lock();
    *out = s_blob;
    store_unlock();
}

bool couple_store_is_paired(void)
{
    store_lock();
    bool paired = couple_store_blob_is_paired(&s_blob);
    store_unlock();
    return paired;
}

bool couple_store_has_wifi(void)
{
    store_lock();
    bool ok = (s_blob.flags & COUPLE_STORE_FLAG_HAS_WIFI) != 0u;
    store_unlock();
    return ok;
}

/* ---------- 同步计数器 ---------- */

uint32_t couple_store_bump_seq(void)
{
    store_lock();
    uint32_t seq = couple_store_blob_next_seq(&s_blob);
    if (!s_seq_dirty &&
        (uint32_t)(s_blob.sync_seq - s_persisted_seq) >= COUPLE_SEQ_PERSIST_STEP) {
        s_seq_dirty = true;
    }
    store_unlock();
    return seq;
}

void couple_store_apply_ack(uint32_t ack)
{
    store_lock();
    uint32_t before = s_blob.sync_ack;
    couple_store_blob_set_ack(&s_blob, ack);
    if (s_blob.sync_ack != before) {
        mark_dirty_locked();
    }
    store_unlock();
}

void couple_store_mark_dirty(void)
{
    store_lock();
    mark_dirty_locked();
    store_unlock();
}

/* ---------- 统计 ---------- */

void couple_store_stats_update(couple_store_stats_fn fn, void *user)
{
    if (fn == NULL) {
        return;
    }
    store_lock();
    couple_stats_t before = s_blob.stats;
    fn(&s_blob.stats, user);
    if (memcmp(&before, &s_blob.stats, sizeof(before)) != 0) {
        mark_dirty_locked();
    }
    store_unlock();
}

void couple_store_stats_read(couple_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    store_lock();
    *out = s_blob.stats;
    store_unlock();
}

void couple_store_stats_write(const couple_stats_t *in)
{
    if (in == NULL) {
        return;
    }
    store_lock();
    s_blob.stats = *in;
    mark_dirty_locked();
    store_unlock();
}

bool couple_store_roll_day(uint32_t utc)
{
    /*
     * utc == 0 表示「还不知道现在几点」（尚未与服务端对时）。
     * 此时推进日界线会把「今天」清零，等于凭空吃掉对方今天的想念次数，所以直接不动。
     */
    if (utc == 0u) {
        return false;
    }

    store_lock();
    bool rolled = couple_state_roll_day(&s_blob.stats, utc, COUPLE_UTC_OFFSET_MINUTES);
    if (rolled) {
        mark_dirty_locked();
    }
    store_unlock();
    return rolled;
}

/* ---------- 落盘节奏 ---------- */

esp_err_t couple_store_flush(void)
{
    store_lock();
    esp_err_t err = flush_locked();
    store_unlock();
    return err;
}

void couple_store_tick(uint32_t now_ms)
{
    store_lock();

    if (!s_have_tick) {
        /* 第一拍只记时间戳：否则 s_last_flush_ms 还是 0，会立刻误判成「早就该写了」。 */
        s_have_tick = true;
        s_last_flush_ms = now_ms;
        store_unlock();
        return;
    }

    if (s_dirty == 0u && !s_seq_dirty) {
        s_last_flush_ms = now_ms;
        store_unlock();
        return;
    }

    bool due = s_seq_dirty || (s_dirty >= COUPLE_NVS_FLUSH_DIRTY_LIMIT) ||
               (uint32_t)(now_ms - s_last_flush_ms) >= COUPLE_NVS_FLUSH_INTERVAL_MS;
    if (!due) {
        store_unlock();
        return;
    }

    (void)flush_locked();
    s_last_flush_ms = now_ms; /* 失败也重新计时，避免日志与擦写被反复触发 */
    store_unlock();
}

/* ---------- 关系 ---------- */

esp_err_t couple_store_set_pair(const uint8_t *pair_id, const uint8_t *pair_key,
                                const uint8_t *partner_id, const char *partner_name)
{
    if (pair_id == NULL || pair_key == NULL || partner_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    store_lock();
    memcpy(s_blob.pair_id, pair_id, COUPLE_PAIR_ID_LEN);
    memcpy(s_blob.pair_key, pair_key, COUPLE_STORE_PAIR_KEY_LEN);
    memcpy(s_blob.partner_id, partner_id, COUPLE_DEVICE_ID_LEN);
    couple_store_sanitize_name(s_blob.partner_name, sizeof(s_blob.partner_name), partner_name);
    s_blob.flags |= COUPLE_STORE_FLAG_PAIRED;

    /* 新 pair_id 就是新的一段关系，对端与 relay 都还没有水位，从零起步。 */
    s_blob.sync_seq = 0u;
    s_blob.sync_ack = 0u;
    s_persisted_seq = 0u;

    esp_err_t err = flush_or_mark_locked();
    store_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "配对信息落盘失败: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t couple_store_clear_pair(void)
{
    store_lock();
    memset(s_blob.pair_id, 0, sizeof(s_blob.pair_id));
    memset(s_blob.pair_key, 0, sizeof(s_blob.pair_key));
    memset(s_blob.partner_id, 0, sizeof(s_blob.partner_id));
    s_blob.partner_name[0] = '\0';
    s_blob.flags &= (uint16_t)~COUPLE_STORE_FLAG_PAIRED;

    s_blob.sync_seq = 0u;
    s_blob.sync_ack = 0u;
    s_persisted_seq = 0u;

    /* stats 一律保留：解除关系不等于把一起积攒的 Love Energy 抹掉。 */
    esp_err_t err = flush_or_mark_locked();
    store_unlock();
    return err;
}

esp_err_t couple_store_set_wifi(const char *ssid, const char *pass)
{
    if (ssid != NULL && ssid[0] != '\0') {
        if (strlen(ssid) >= sizeof(s_blob.wifi_ssid)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (pass != NULL && strlen(pass) >= sizeof(s_blob.wifi_pass)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    store_lock();
    if (ssid == NULL || ssid[0] == '\0') {
        s_blob.wifi_ssid[0] = '\0';
        s_blob.wifi_pass[0] = '\0';
        s_blob.flags &= (uint16_t)~COUPLE_STORE_FLAG_HAS_WIFI;
    } else {
        couple_store_copy(s_blob.wifi_ssid, sizeof(s_blob.wifi_ssid), ssid);
        couple_store_copy(s_blob.wifi_pass, sizeof(s_blob.wifi_pass), (pass != NULL) ? pass : "");
        s_blob.flags |= COUPLE_STORE_FLAG_HAS_WIFI;
    }
    esp_err_t err = flush_or_mark_locked();
    store_unlock();
    return err;
}

esp_err_t couple_store_set_relay_url(const char *url)
{
    if (url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char normalized[COUPLE_RELAY_URL_MAX];
    if (!couple_store_sanitize_relay_url(normalized, sizeof(normalized), url)) {
        return ESP_ERR_INVALID_ARG;
    }

    store_lock();
    couple_store_copy(s_blob.relay_url, sizeof(s_blob.relay_url), normalized);
    esp_err_t err = flush_or_mark_locked();
    store_unlock();
    return err;
}

esp_err_t couple_store_set_names(const char *self_name, const char *partner_name)
{
    if (self_name == NULL && partner_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    store_lock();
    if (self_name != NULL) {
        char cleaned[COUPLE_NAME_MAX + 1u];
        couple_store_sanitize_name(cleaned, sizeof(cleaned), self_name);
        if (cleaned[0] != '\0') {
            couple_store_copy(s_blob.self_name, sizeof(s_blob.self_name), cleaned);
        }
    }
    if (partner_name != NULL) {
        couple_store_sanitize_name(s_blob.partner_name, sizeof(s_blob.partner_name), partner_name);
    }
    esp_err_t err = flush_or_mark_locked();
    store_unlock();
    return err;
}