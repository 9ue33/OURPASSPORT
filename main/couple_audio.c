/*
 * Heartlink 情侣心动终端 —— 音效播放 worker 实现。
 *
 * 线程模型：
 *   调用方任务            音频 worker 任务
 *   ----------            --------------
 *   play()  -> 队列 -----> 取消息、流式渲染、写 I2S
 *   loop()  -> s_loop + 队列
 *   stop_loop() / cancel() -> 直接改 s_loop / s_gen（必须绕开队列，
 *                             否则 worker 正卡在播放循环里，消息排到后面才被看到，
 *                             「立刻停」就变成了「播完才停」）
 *
 * s_loop / s_gen / s_playing / s_playing_loop 都是 volatile 的 32 位（或更窄）对齐标量，
 * 在这颗芯片上单次读写是原子的，所以不需要额外加锁：这里只要「最终可见」，
 * 不需要多个字段之间的一致性。s_gen 是「作废代数」，调用方自增即代表
 * 「现在正在播的这一段作废」。
 */

#include "couple_audio.h"

#include "bsp_audio.h"
#include "couple_config.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "couple_audio";

enum {
    AUDIO_CHUNK_SAMPLES = COUPLE_AUDIO_CHUNK_SAMPLES,
    AUDIO_QUEUE_DEPTH = COUPLE_AUDIO_QUEUE_DEPTH,
    AUDIO_TASK_STACK = 3072,
    AUDIO_TASK_PRIO = 4
};

typedef enum {
    AUDIO_MSG_PLAY_ONE = 0, /* arg = couple_tone_t，播一次 */
    AUDIO_MSG_SET_LOOP,     /* arg = couple_tone_t，循环它 */
    AUDIO_MSG_CANCEL,       /* 丢弃队列里所有更早的消息（代数已在调用方自增） */
    AUDIO_MSG_VOLUME        /* arg = 0..100，在 worker 里改，避免与播放争 codec */
} audio_msg_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t arg;
} audio_msg_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;

/* 流式渲染的目标缓冲。1024 样本 / 64 ms，是「停得多快」和「占多少 DRAM」的折中。 */
static int16_t s_pcm[AUDIO_CHUNK_SAMPLES];

static volatile bool s_ready;
static volatile bool s_playing;      /* 正在写 I2S（含循环之间的静音） */
static volatile bool s_playing_loop; /* 当前正在写的这一段属于循环音效 */
static volatile uint8_t s_loop = (uint8_t)COUPLE_TONE_NONE;
static volatile uint32_t s_gen;

static bool audio_post(audio_msg_kind_t kind, uint8_t arg)
{
    if (s_queue == NULL) {
        return false;
    }

    audio_msg_t msg = { .kind = (uint8_t)kind, .arg = arg };
    return xQueueSend(s_queue, &msg, 0) == pdTRUE;
}

/* 重新建立采样格式。codec 可能被休眠/唤醒路径关过，写失败时给一次恢复机会。 */
static bool audio_write_chunk(const int16_t *pcm, size_t samples, bool *recovered)
{
    size_t bytes = samples * sizeof(int16_t);
    esp_err_t err = bsp_audio_write(pcm, bytes);

    if (err == ESP_OK) {
        return true;
    }

    if (!*recovered) {
        *recovered = true;
        if (bsp_audio_set_format(COUPLE_TONE_SAMPLE_RATE, 16u, 1u) == ESP_OK) {
            err = bsp_audio_write(pcm, bytes);
            if (err == ESP_OK) {
                ESP_LOGW(TAG, "I2S 写入失败后重建格式成功，继续播放");
                return true;
            }
        }
    }

    ESP_LOGW(TAG, "I2S 写入失败: %s", esp_err_to_name(err));
    return false;
}

/*
 * 循环音效两拍之间的静音。写成真正的零样本而不是「不写」：
 * 不写的话 DMA 会重复上一块内容，心跳会连成一片嗡嗡声。
 */
static void audio_write_silence(uint32_t ms, uint32_t gen)
{
    size_t left = ((size_t)COUPLE_TONE_SAMPLE_RATE * (size_t)ms) / 1000u;
    bool recovered = false;

    memset(s_pcm, 0, sizeof(s_pcm));

    while (left > 0u && s_gen == gen) {
        size_t n = left < (size_t)AUDIO_CHUNK_SAMPLES ? left : (size_t)AUDIO_CHUNK_SAMPLES;
        if (!audio_write_chunk(s_pcm, n, &recovered)) {
            return;
        }
        left -= n;
    }
}

/* 把一个音效完整（或按下标）写完。is_loop 决定这一段是否可被 stop_loop 截断。 */
static void audio_play_stream(couple_tone_t tone, bool is_loop)
{
    couple_tone_stream_t stream;
    bool recovered = false;

    if (!s_ready) {
        return;
    }
    if (couple_tone_stream_begin(&stream, tone) == 0u) {
        return;
    }

    s_playing_loop = is_loop;
    s_playing = true;
    uint32_t gen = s_gen;

    while (s_gen == gen) {
        size_t n = couple_tone_stream_render(&stream, s_pcm, (size_t)AUDIO_CHUNK_SAMPLES);
        if (n == 0u) {
            break;
        }
        if (!audio_write_chunk(s_pcm, n, &recovered)) {
            break;
        }
    }

    s_playing = false;
    s_playing_loop = false;

    if (is_loop && s_gen == gen && s_loop != (uint8_t)COUPLE_TONE_NONE) {
        audio_write_silence(COUPLE_AUDIO_LOOP_GAP_MS, gen);
    }
}

static void audio_handle_msg(const audio_msg_t *msg)
{
    switch ((audio_msg_kind_t)msg->kind) {
    case AUDIO_MSG_PLAY_ONE:
        audio_play_stream((couple_tone_t)msg->arg, false);
        break;
    case AUDIO_MSG_SET_LOOP:
        /* s_loop 已由调用方设置；这里只负责把 worker 从阻塞接收里叫醒。 */
        break;
    case AUDIO_MSG_CANCEL:
        /*
         * 代数已由调用方自增，正在写的那一块马上会退出。
         * 队列里排在 CANCEL 之前的消息都是「取消前排的」，一起丢掉。
         */
        xQueueReset(s_queue);
        ESP_LOGI(TAG, "音效已取消");
        break;
    case AUDIO_MSG_VOLUME:
        bsp_audio_set_volume(msg->arg);
        break;
    default:
        break;
    }
}

static void audio_worker(void *arg)
{
    (void)arg;
    audio_msg_t msg;

    for (;;) {
        if (s_loop != (uint8_t)COUPLE_TONE_NONE) {
            /* 循环期间仍然插播单次音效，播完自动回到循环。 */
            if (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
                audio_handle_msg(&msg);
                continue;
            }
            audio_play_stream((couple_tone_t)s_loop, true);
            continue;
        }

        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) == pdTRUE) {
            audio_handle_msg(&msg);
        }
    }
}

esp_err_t couple_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (s_queue == NULL) {
        s_queue = xQueueCreate((UBaseType_t)AUDIO_QUEUE_DEPTH, sizeof(audio_msg_t));
        if (s_queue == NULL) {
            ESP_LOGE(TAG, "音效队列创建失败");
            return ESP_ERR_NO_MEM;
        }
    }

    /*
     * bsp_audio_init() 可重复调用，失败时会自己回收本次已创建的资源，
     * 所以这里失败直接返回，修好外部故障后重试即可。
     */
    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    err = bsp_audio_set_format(COUPLE_TONE_SAMPLE_RATE, 16u, 1u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置采样格式失败: %s", esp_err_to_name(err));
        return err;
    }

    /* ES8311 的静态初值是 0（静音），必须显式设一次。 */
    bsp_audio_set_volume(COUPLE_AUDIO_VOLUME);

    s_loop = (uint8_t)COUPLE_TONE_NONE;
    s_gen = 0u;
    s_playing = false;
    s_playing_loop = false;
    s_ready = true;

    if (s_task == NULL) {
        if (xTaskCreate(audio_worker, "couple_audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIO, &s_task) != pdPASS) {
            s_task = NULL;
            s_ready = false;
            ESP_LOGE(TAG, "音效任务创建失败");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "音效通道就绪: %u Hz / 16 bit / 单声道, 音量 %u%%",
             (unsigned)COUPLE_TONE_SAMPLE_RATE, (unsigned)COUPLE_AUDIO_VOLUME);
    return ESP_OK;
}

bool couple_audio_ready(void)
{
    return s_ready;
}

bool couple_audio_play(couple_tone_t tone)
{
    if (!s_ready || tone <= COUPLE_TONE_NONE || tone >= COUPLE_TONE_COUNT) {
        return false;
    }
    return audio_post(AUDIO_MSG_PLAY_ONE, (uint8_t)tone);
}

bool couple_audio_loop(couple_tone_t tone)
{
    if (!s_ready || tone <= COUPLE_TONE_NONE || tone >= COUPLE_TONE_COUNT) {
        return false;
    }

    /*
     * 先直接改标志：worker 可能正卡在播放里，等它处理队列消息就太晚了。
     * 再补一条队列消息，把「阻塞等待消息」的 worker 叫起来开始循环。
     */
    s_loop = (uint8_t)tone;
    (void)audio_post(AUDIO_MSG_SET_LOOP, (uint8_t)tone);
    return true;
}

bool couple_audio_loop_active(void)
{
    return s_loop != (uint8_t)COUPLE_TONE_NONE;
}

void couple_audio_stop_loop(void)
{
    if (s_loop == (uint8_t)COUPLE_TONE_NONE) {
        return;
    }

    s_loop = (uint8_t)COUPLE_TONE_NONE;

    /*
     * 只截断「正在播的循环音」，已入队的单次音效不受影响。
     * 若此刻恰好在两段之间（s_playing_loop 为假），下一次迭代会看到 s_loop 已清空，
     * 自然不会再起一段，所以这里不做额外处理。
     */
    if (s_playing_loop) {
        s_gen++;
    }
}

void couple_audio_cancel(void)
{
    s_loop = (uint8_t)COUPLE_TONE_NONE;
    s_gen++; /* 立刻让正在写的那一块退出 */
    (void)audio_post(AUDIO_MSG_CANCEL, 0u);
}

bool couple_audio_set_volume(uint8_t percent)
{
    if (percent > 100u) {
        percent = 100u;
    }
    if (!s_ready && s_queue == NULL) {
        return false;
    }
    return audio_post(AUDIO_MSG_VOLUME, percent);
}

bool couple_audio_playing(void)
{
    return s_playing;
}

bool couple_audio_quiesce(uint32_t timeout_ms)
{
    couple_audio_cancel();

    TickType_t step = pdMS_TO_TICKS(10);
    if (step == 0u) {
        step = 1u;
    }

    uint32_t waited = 0u;
    while (s_playing && waited < timeout_ms) {
        vTaskDelay(step);
        waited += 10u;
    }

    return !s_playing;
}