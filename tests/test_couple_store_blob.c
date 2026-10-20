/*
 * couple_store_blob 的 host 测试：NVS 数据模型的纯逻辑部分。
 *
 * 这里覆盖的是「最容易悄悄写坏、又最难在板子上查」的那些地方：
 * 结构不变量、seq/ack 单调性、名字清洗与 UTF-8 截断、relay 地址规范化。
 * 不依赖 ESP-IDF，也不依赖 NVS。
 *
 * 成功时打印 PASS 并返回 0；任何检查失败都会打印诊断行，最终返回 1。
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "couple_state.h"
#include "couple_store_blob.h"
#include "couple_types.h"

static unsigned g_failed;

static void fail_line(int line, const char *what)
{
    g_failed++;
    printf("FAIL line %d: %s\n", line, what);
}

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fail_line(__LINE__, #cond);                                   \
        }                                                                 \
    } while (0)

#define CHECK_U(got, want)                                               \
    do {                                                                 \
        unsigned g_ = (unsigned)(got);                                   \
        unsigned w_ = (unsigned)(want);                                  \
        if (g_ != w_) {                                                  \
            g_failed++;                                                  \
            printf("FAIL line %d: got %u want %u\n", __LINE__, g_, w_);  \
        }                                                                \
    } while (0)

#define CHECK_STR(got, want)                                                   \
    do {                                                                       \
        const char *g_ = (got);                                                \
        const char *w_ = (want);                                               \
        if (strcmp(g_, w_) != 0) {                                             \
            g_failed++;                                                        \
            printf("FAIL line %d: got \"%s\" want \"%s\"\n", __LINE__, g_, w_); \
        }                                                                      \
    } while (0)

/* ---------- 结构不变量 ---------- */

static void test_init_and_valid(void)
{
    couple_store_blob_t blob;

    memset(&blob, 0xAA, sizeof(blob));
    couple_store_blob_init(&blob);

    CHECK_U(blob.magic, COUPLE_STORE_MAGIC);
    CHECK_U(blob.schema, COUPLE_STORE_SCHEMA);
    CHECK_U(blob.flags, 0u);
    CHECK(couple_store_blob_valid(&blob));
    CHECK(!couple_store_blob_is_paired(&blob));
    /* 出厂状态下统计也要是合法的一份，别让上层再补一次初始化。 */
    CHECK(couple_state_valid(&blob.stats));
    CHECK_U(blob.sync_seq, 0u);
    CHECK_U(blob.sync_ack, 0u);
    CHECK_U(blob.relay_url[0], 0u);
    CHECK_U(blob.device_id[0], 0u);

    /* magic 不对：必须判死，否则会把别的命名空间的数据当成自己的。 */
    blob.magic = 0u;
    CHECK(!couple_store_blob_valid(&blob));
    blob.magic = COUPLE_STORE_MAGIC;

    /* schema 不对：将来布局变了要能识别出老数据。 */
    blob.schema = (uint16_t)(COUPLE_STORE_SCHEMA + 1u);
    CHECK(!couple_store_blob_valid(&blob));
    blob.schema = (uint16_t)COUPLE_STORE_SCHEMA;

    CHECK(couple_store_blob_valid(&blob));

    /* NULL 永远安全。 */
    CHECK(!couple_store_blob_valid(NULL));
    CHECK(!couple_store_blob_is_paired(NULL));
    couple_store_blob_init(NULL);
    couple_store_blob_set_ack(NULL, 5u);
    CHECK_U(couple_store_blob_next_seq(NULL), 0u);
}

static void test_paired_flag_requires_full_trio(void)
{
    couple_store_blob_t blob;

    couple_store_blob_init(&blob);

    blob.flags |= COUPLE_STORE_FLAG_PAIRED;
    /* 只有 flag，没有 id / key / partner：这是坏数据，不是已配对。 */
    CHECK(!couple_store_blob_is_paired(&blob));
    CHECK(!couple_store_blob_valid(&blob));

    for (size_t i = 0u; i < sizeof(blob.pair_id); ++i) {
        blob.pair_id[i] = 0x11u;
    }
    CHECK(!couple_store_blob_is_paired(&blob)); /* 还差 key 与 partner */
    CHECK(!couple_store_blob_valid(&blob));

    for (size_t i = 0u; i < sizeof(blob.pair_key); ++i) {
        blob.pair_key[i] = 0x22u;
    }
    CHECK(!couple_store_blob_is_paired(&blob));
    CHECK(!couple_store_blob_valid(&blob));

    for (size_t i = 0u; i < sizeof(blob.partner_id); ++i) {
        blob.partner_id[i] = 0x33u;
    }
    CHECK(couple_store_blob_is_paired(&blob));
    CHECK(couple_store_blob_valid(&blob));

    /* 密钥整份丢掉就不算——半份密钥绝不能拿去连 relay。 */
    memset(blob.pair_key, 0, sizeof(blob.pair_key));
    CHECK(!couple_store_blob_is_paired(&blob));
    CHECK(!couple_store_blob_valid(&blob));

    for (size_t i = 0u; i < sizeof(blob.pair_key); ++i) {
        blob.pair_key[i] = 0x22u;
    }
    CHECK(couple_store_blob_is_paired(&blob));

    blob.flags &= (uint16_t)~COUPLE_STORE_FLAG_PAIRED;
    CHECK(!couple_store_blob_is_paired(&blob));
    /* 光把 flag 摘掉仍然是合法结构（stats 还在，记忆还在）。 */
    CHECK(couple_store_blob_valid(&blob));
}

static void test_seq_and_ack(void)
{
    couple_store_blob_t blob;

    couple_store_blob_init(&blob);

    CHECK_U(couple_store_blob_next_seq(&blob), 1u);
    CHECK_U(couple_store_blob_next_seq(&blob), 2u);
    CHECK_U(blob.sync_seq, 2u);

    /* 0 是「还没发过」的保留值：回绕的时候要跳过它。 */
    blob.sync_seq = 0xFFFFFFFFu;
    CHECK_U(couple_store_blob_next_seq(&blob), 1u);
    CHECK_U(blob.sync_seq, 1u);

    /* ack 只增不减：乱序到达的老响应不能把剪枝点拉回去。 */
    blob.sync_ack = 10u;
    couple_store_blob_set_ack(&blob, 5u);
    CHECK_U(blob.sync_ack, 10u);
    couple_store_blob_set_ack(&blob, 10u);
    CHECK_U(blob.sync_ack, 10u);
    couple_store_blob_set_ack(&blob, 11u);
    CHECK_U(blob.sync_ack, 11u);
    couple_store_blob_set_ack(&blob, 0u);
    CHECK_U(blob.sync_ack, 11u);
}

static void test_id_is_empty(void)
{
    uint8_t id[COUPLE_DEVICE_ID_LEN];

    memset(id, 0, sizeof(id));
    CHECK(couple_store_blob_id_is_empty(id, sizeof(id)));
    CHECK(couple_store_blob_id_is_empty(id, 0u));
    CHECK(couple_store_blob_id_is_empty(NULL, sizeof(id)));

    id[COUPLE_DEVICE_ID_LEN - 1u] = 1u;
    CHECK(!couple_store_blob_id_is_empty(id, sizeof(id)));
    id[COUPLE_DEVICE_ID_LEN - 1u] = 0u;
    id[0] = 0xFFu;
    CHECK(!couple_store_blob_id_is_empty(id, sizeof(id)));
}

/* ---------- 安全拷贝 ---------- */

static void test_copy(void)
{
    char buf[8];

    memset(buf, 'X', sizeof(buf));
    CHECK_U(couple_store_copy(buf, sizeof(buf), "hi"), 2u);
    CHECK_STR(buf, "hi");

    /* 长得放不下就截断，但一定留 NUL。 */
    memset(buf, 'X', sizeof(buf));
    CHECK_U(couple_store_copy(buf, sizeof(buf), "0123456789"), 7u);
    CHECK_STR(buf, "0123456");

    /* 刚好放得下。 */
    memset(buf, 'X', sizeof(buf));
    CHECK_U(couple_store_copy(buf, sizeof(buf), "0123456"), 7u);
    CHECK_STR(buf, "0123456");

    memset(buf, 'X', sizeof(buf));
    CHECK_U(couple_store_copy(buf, sizeof(buf), "01234567"), 7u);
    CHECK_STR(buf, "0123456");

    /* NULL 源写空串，不是崩。 */
    memset(buf, 'X', sizeof(buf));
    CHECK_U(couple_store_copy(buf, sizeof(buf), NULL), 0u);
    CHECK_STR(buf, "");

    /* cap 为 0 时一个字节都不许碰。 */
    memset(buf, 'X', sizeof(buf));
    CHECK_U(couple_store_copy(buf, 0u, "hi"), 0u);
    CHECK_U(buf[0], (unsigned)'X');

    CHECK_U(couple_store_copy(NULL, sizeof(buf), "hi"), 0u);
}

/* ---------- 名字清洗 ---------- */

static void test_sanitize_name_basics(void)
{
    char buf[COUPLE_NAME_MAX + 1u];

    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), "Wren"), 4u);
    CHECK_STR(buf, "Wren");

    /* 中文照抄，一个字节都不能少。 */
    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), "小夜"), 6u);
    CHECK_STR(buf, "小夜");

    /* 控制字符全丢（换行、制表、退格、DEL、NUL 之后的内容当成结尾）。 */
    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), "\t小\n夜\r"), 6u);
    CHECK_STR(buf, "小夜");
    /* 注意 \x.. 会一直吃掉后面的十六进制字符，所以要拆开写。 */
    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), "a\x7F" "b"), 2u);
    CHECK_STR(buf, "ab");
    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), "\x01\x02"), 0u);
    CHECK_STR(buf, "");

    /* 首尾空白补掉，中间连续空白压成一个。 */
    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), "   Wren   "), 4u);
    CHECK_STR(buf, "Wren");
    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), "a    b"), 3u);
    CHECK_STR(buf, "a b");
    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), "     "), 0u);
    CHECK_STR(buf, "");

    /* 全是控制字符 + 空白 → 空串。 */
    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), " \t\n "), 0u);
    CHECK_STR(buf, "");

    /* NULL 源。 */
    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), NULL), 0u);
    CHECK_STR(buf, "");
}

static void test_sanitize_name_utf8_edges(void)
{
    char buf[32];

    /* 4 字节 emoji 不能被切一半。 */
    CHECK_U(couple_store_sanitize_name(buf, 5u, "\xF0\x9F\x98\x80"), 4u);
    CHECK_STR(buf, "\xF0\x9F\x98\x80");

    /* 只剩 4 字节时放不下整个 emoji，整个不写。 */
    memset(buf, 'X', sizeof(buf));
    CHECK_U(couple_store_sanitize_name(buf, 4u, "\xF0\x9F\x98\x80"), 0u);
    CHECK_STR(buf, "");

    /* 三字节汉字：cap 只够 3 个字时，宁少不多。 */
    memset(buf, 'X', sizeof(buf));
    CHECK_U(couple_store_sanitize_name(buf, 10u, "宝宝宝宝"), 9u);
    CHECK_STR(buf, "宝宝宝");

    memset(buf, 'X', sizeof(buf));
    CHECK_U(couple_store_sanitize_name(buf, 9u, "宝宝宝宝"), 6u);
    CHECK_STR(buf, "宝宝");

    /* cap 只够一个字节时，连一个汉字都放不下。 */
    memset(buf, 'X', sizeof(buf));
    CHECK_U(couple_store_sanitize_name(buf, 2u, "宝宝"), 0u);
    CHECK_STR(buf, "");

    /* 截断之后不许残留半个字符：写入长度本身必须是 UTF-8 边界。 */
    memset(buf, 'X', sizeof(buf));
    size_t n = couple_store_sanitize_name(buf, 8u, "宝宝宝宝");
    CHECK_U(n, 6u);
    CHECK_U(buf[n], 0u);

    /* 尾部残缺的 UTF-8 序列（比如从 NVS 里读回半截）直接丢掉。 */
    memset(buf, 'X', sizeof(buf));
    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), "宝宝\xE5\xAE"), 6u);
    CHECK_STR(buf, "宝宝");

    /* 非法续字节按单字节带过，不会把它当成汉字的一部分吃掉。 */
    CHECK_U(couple_store_sanitize_name(buf, sizeof(buf), "a\x80" "b"), 3u);
}

/* ---------- relay 地址规范化 ---------- */

static void test_sanitize_relay_url(void)
{
    char buf[COUPLE_RELAY_URL_MAX];

    memset(buf, 'X', sizeof(buf));
    CHECK(couple_store_sanitize_relay_url(buf, sizeof(buf), "http://192.168.1.100:8787"));
    CHECK_STR(buf, "http://192.168.1.100:8787");

    /* 尾部斜杠去掉，多少个都去。 */
    CHECK(couple_store_sanitize_relay_url(buf, sizeof(buf), "http://192.168.1.100:8787/"));
    CHECK_STR(buf, "http://192.168.1.100:8787");
    CHECK(couple_store_sanitize_relay_url(buf, sizeof(buf), "http://192.168.1.100:8787///"));
    CHECK_STR(buf, "http://192.168.1.100:8787");

    /* 带路径的也接受（后半段原样保留）。 */
    CHECK(couple_store_sanitize_relay_url(buf, sizeof(buf), "https://heart.link/api/"));
    CHECK_STR(buf, "https://heart.link/api");

    /* scheme 大小写不敏感，落存时统一小写。 */
    CHECK(couple_store_sanitize_relay_url(buf, sizeof(buf), "HTTP://Heart.Link:8787"));
    CHECK_STR(buf, "http://Heart.Link:8787");

    /* 只有 scheme、没有主机名：拒。这条最容易被漏掉。 */
    memset(buf, 'X', sizeof(buf));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "http://"));
    CHECK_STR(buf, "");
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "http:///"));
    CHECK_STR(buf, "");

    /* 别的 scheme：拒。 */
    memset(buf, 'X', sizeof(buf));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "ftp://host"));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "ws://host"));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "192.168.1.100:8787"));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "/v1/sync"));
    CHECK_STR(buf, "");

    /* 中间有空白或控制字符：多半是整块粘进来的，拒。 */
    memset(buf, 'X', sizeof(buf));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "http://host/a b"));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "http://host\n:8787"));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "http:// host:8787"));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "http://host:8787 "));
    CHECK_STR(buf, "");

    /* 基础地址不带查询串或片段，路径由程序自己拼。 */
    memset(buf, 'X', sizeof(buf));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "http://host:8787/?t=1"));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), "http://host:8787/#x"));
    CHECK_STR(buf, "");

    /* NULL / 空 / 容量为 0 / 放不下：全部拒，且 out 变空串。 */
    memset(buf, 'X', sizeof(buf));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), NULL));
    CHECK_STR(buf, "");
    memset(buf, 'X', sizeof(buf));
    CHECK(!couple_store_sanitize_relay_url(buf, sizeof(buf), ""));
    CHECK_STR(buf, "");
    CHECK(!couple_store_sanitize_relay_url(NULL, sizeof(buf), "http://host"));
    CHECK(!couple_store_sanitize_relay_url(buf, 0u, "http://host"));

    memset(buf, 'X', sizeof(buf));
    CHECK(!couple_store_sanitize_relay_url(buf, 10u, "http://host.example"));
    CHECK_STR(buf, "");

    /* 刚好放得下：8 字节 scheme+host 需要 9 字节缓冲。 */
    CHECK(couple_store_sanitize_relay_url(buf, 9u, "http://h"));
    CHECK_STR(buf, "http://h");
    CHECK(couple_store_sanitize_relay_url(buf, 10u, "https://h"));
    CHECK_STR(buf, "https://h");

    /* 放不下就整条拒掉：宁可让用户改短，也不要存下一个连不上的地址。 */
    char long_url[COUPLE_RELAY_URL_MAX];
    char small[32];
    char roomy[40];

    memset(long_url, 'a', sizeof(long_url));
    memcpy(long_url, "http://", 7u);
    long_url[sizeof(long_url) - 1u] = '\0'; /* 95 字节，正好填满整个字段 */
    CHECK_U(strlen(long_url), COUPLE_RELAY_URL_MAX - 1u);

    memset(small, 'X', sizeof(small));
    CHECK(!couple_store_sanitize_relay_url(small, sizeof(small), long_url));
    CHECK_STR(small, "");

    /* 装满整个字段是允许的：95 字节地址需要 96 字节缓冲。 */
    CHECK(couple_store_sanitize_relay_url(buf, sizeof(buf), long_url));
    CHECK_U(strlen(buf), COUPLE_RELAY_URL_MAX - 1u);

    /* 31 字节地址需要 32 字节缓冲（别忘了结尾的 NUL）。 */
    long_url[31u] = '\0';
    memset(small, 'X', sizeof(small));
    CHECK(couple_store_sanitize_relay_url(small, sizeof(small), long_url));
    CHECK_U(strlen(small), 31u);

    /* 再多一个字符就得 33 字节缓冲：宁可拒，也不静默截断。 */
    long_url[31u] = 'a';
    long_url[32u] = '\0';
    CHECK_U(strlen(long_url), 32u);
    memset(small, 'X', sizeof(small));
    CHECK(!couple_store_sanitize_relay_url(small, sizeof(small), long_url));
    CHECK_STR(small, "");

    memset(roomy, 'X', sizeof(roomy));
    CHECK(couple_store_sanitize_relay_url(roomy, sizeof(roomy), long_url));
    CHECK_U(strlen(roomy), 32u);
}

int main(void)
{
    test_init_and_valid();
    test_paired_flag_requires_full_trio();
    test_seq_and_ack();
    test_id_is_empty();
    test_copy();
    test_sanitize_name_basics();
    test_sanitize_name_utf8_edges();
    test_sanitize_relay_url();

    if (g_failed != 0u) {
        printf("couple_store_blob: %u check(s) failed\n", g_failed);
        return 1;
    }
    printf("couple_store_blob: PASS\n");
    return 0;
}