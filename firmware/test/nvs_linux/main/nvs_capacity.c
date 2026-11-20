/*
 * M1.10b.1 NVS capacity and power-loss check: the domain storage (dsp_storage, dsp_schedule_repo,
 * codec) over the firmware's NVS adapter (dsp_nvs_kv.c, unchanged) and the REAL ESP-IDF v6.1
 * nvs_flash library, on the linux target's emulated SPI flash with the committed partition
 * table (nvs = 80 KiB). Power cuts: every flash write/erase that NVS issues goes through
 * link-time wrappers (--wrap); at the cut point the write is truncated at 4-byte granularity
 * (an interrupted erase leaves its sector untouched) and EVERY later write or erase fails
 * (powered off), unlike esp_partition_fail_after(), which lets later calls of the same
 * operation succeed. The NVS partition is then deinitialized and re-initialized (a reset).
 *
 * Checks:
 *   1. maximum-size records (schedule set 3,160 B: 16 schedules with 64-character IDs and
 *      32-byte names, 16 tombstones) plus realistic co-tenants (PHY calibration, Wi-Fi);
 *   2. 20,000 repeated maximum-size schedule-set writes (with configuration writes), reopened
 *      every 2,000: no entry leak, wear per write;
 *   3. format 1 -> 2 upgrade write cut at EVERY write/erase cycle: storage always opens, the
 *      set is exactly the old (format 1) or the new (format 2) record, every other key intact;
 *   4. low free space: the write is refused cleanly (old set intact) when it does not fit; the
 *      steady-state headroom for repeated maximum-size writes is measured; at that headroom a
 *      cut at every cycle of a write (page reclamation) leaves the old or the new set;
 *   5. the same cut-everywhere check on plain nvs_set_blob() without any dispenser code, for
 *      several size pairs on a fresh and an aged partition (traced and dumped for 32 -> 64 B);
 *  11. (M1.10b.3) the layout v4 -> v5 migration of a populated device and "auth" record writes
 *      (provisioned -> claimed, claimed -> claimed with a second phone), each cut at EVERY
 *      write/erase cycle: the storage always opens, "auth" is exactly the old or the new record
 *      (never missing, never damaged), every other key is intact.
 *
 * Modes (environment MODE): unset = checks 1-5; "matrix" = 6 (two-cut matrix: a cut at every
 * cycle of a write, then at every cycle of the recovery that follows, two more boots and a
 * follow-up write), 7 (the same with the #19214 "first word erased" tear model) and 8 (#19213
 * hash collisions of this firmware's item identities); "ab" = 10 (redundant-record alternative,
 * kept for comparison); "dump"/"load" = 9 (flash images exchanged between the patched and the
 * unpatched component, IMG=<path>).
 *
 * FINDING (2026-11-16, ESP-IDF v6.1 fff9895c): every blob replacement has exactly one cut point
 * at which the key is lost (nvs_get_blob -> ESP_ERR_NVS_NOT_FOUND): power lost after the new
 * version's last data chunk is complete and before the new blob index is written. At the next
 * nvs_flash_init_partition(), Page::load() ("check that last item is not duplicate",
 * nvs_page.cpp) looks up the page's last item WITHOUT its chunk index and erases the OLD
 * version's data chunk as a "duplicate"; eraseMismatchedBlobIndexes() then deletes the old
 * index and eraseOrphanDataBlobs() the new chunk. The dispenser storage reports the missing key
 * as CORRUPT (fail closed). With the UNPATCHED component checks 3 and 5 (and the matrix) fail;
 * with the project's patched component (firmware/third_party/nvs_flash) everything passes.
 * run_checks.sh requires both outcomes (negative control).
 * Exit status 0 only if every check passed.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_partition.h"
#include "esp_private/partition_linux.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_rom_crc.h"

#include "dsp_codec.h"
#include "dsp_nvs_kv.h"
#include "dsp_evlog.h"
#include "dsp_schedule_repo.h"
#include "dsp_storage.h"

/* ---- Power-off model ------------------------------------------------------------------ */

esp_err_t __real_esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t size);
esp_err_t __real_esp_partition_write_raw(const esp_partition_t *p, size_t off, const void *src,
                                         size_t size);
esp_err_t __real_esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t size);

static bool pw_armed;   /* a cut is scheduled */
static bool pw_off;     /* the cut happened: flash is no longer modified */
static size_t pw_left;  /* cycles before the cut (4 bytes written or 1 sector erased each) */
static size_t pw_cycles; /* cycles counted since pw_count_reset() */
static bool pw_trace;    /* print every write/erase (diagnostics) */
static int pw_tear_mode; /* 0: the cut write keeps a prefix; 1: every word but the first (#19214) */

static void pw_arm(size_t cycles)
{
    pw_armed = true;
    pw_off = false;
    pw_left = cycles;
}
static void pw_restore_power(void)
{
    pw_armed = false;
    pw_off = false;
}

typedef esp_err_t (*write_fn)(const esp_partition_t *, size_t, const void *, size_t);
static esp_err_t pw_write(write_fn real, const esp_partition_t *p, size_t off, const void *src,
                          size_t size)
{
    if (pw_off) {
        return ESP_FAIL;
    }
    size_t cyc = (size + 3) / 4;
    if (pw_trace) {
        printf("      write @0x%05zx (page %zu, +0x%03zx) %3zu B  cycles %zu..%zu\n", off, off / 4096,
               off % 4096, size, pw_cycles + 1, pw_cycles + cyc);
    }
    pw_cycles += cyc;
    if (pw_armed) {
        if (pw_left > cyc) {
            pw_left -= cyc;
        } else {
            if (pw_tear_mode == 1 && size >= 8) {
                size_t m = pw_left < cyc - 1 ? pw_left : cyc - 1;
                if (m > 0) { /* first word left erased, later words programmed */
                    (void)real(p, off + 4, (const uint8_t *)src + 4, m * 4);
                }
            } else if (pw_left > 0) {
                (void)real(p, off, src, pw_left * 4); /* torn write */
            }
            pw_off = true;
            return ESP_FAIL;
        }
    }
    return real(p, off, src, size);
}
esp_err_t __wrap_esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t size)
{
    return pw_write(__real_esp_partition_write, p, off, src, size);
}
esp_err_t __wrap_esp_partition_write_raw(const esp_partition_t *p, size_t off, const void *src,
                                         size_t size)
{
    return pw_write(__real_esp_partition_write_raw, p, off, src, size);
}
esp_err_t __wrap_esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t size)
{
    if (pw_off) {
        return ESP_FAIL;
    }
    size_t sectors = (size + 4095) / 4096;
    if (pw_trace) {
        printf("      erase @0x%05zx %zu sector(s)  cycles %zu..%zu\n", off, sectors, pw_cycles + 1,
               pw_cycles + sectors);
    }
    pw_cycles += sectors;
    if (pw_armed) {
        if (pw_left > sectors) {
            pw_left -= sectors;
        } else {
            if (pw_left > 0) {
                (void)__real_esp_partition_erase_range(p, off, pw_left * 4096);
            }
            pw_off = true; /* the interrupted sector keeps its content */
            return ESP_FAIL;
        }
    }
    return __real_esp_partition_erase_range(p, off, size);
}

static int checks, failures;
#define CHECK(c)                                                                                  \
    do {                                                                                          \
        checks++;                                                                                 \
        if (!(c)) {                                                                               \
            failures++;                                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                   \
        }                                                                                         \
    } while (0)

static const dsp_tz_t *const TZ = &DSP_TZ_EUROPE_PRAGUE;
static const esp_partition_t *part;
static dsp_nvs_kv_t nvs;
static dsp_kv_t kv;
static dsp_storage_t st;
static dsp_schedule_repo_t repo;
static dsp_history_store_t hist;
static uint8_t snap[0x14000];

#define DATA_KEYS (DSP_STORAGE_HISTORY_SLOTS + 7) /* h00..h31, sched, config, time, notify, dosing, layout, auth (v5) */

typedef struct {
    size_t len[DATA_KEYS];
    uint8_t v[DATA_KEYS][DSP_RECORD_MAX_BYTES];
} keyset_t;
static keyset_t old_keys, now_keys;

static void key_name(size_t i, char out[8])
{
    static const char *tail[] = {"sched", "config", "time", "notify", "dosing", "layout", "auth"};
    if (i < DSP_STORAGE_HISTORY_SLOTS) {
        dsp_storage_history_key(i, out, 8);
    } else {
        strcpy(out, tail[i - DSP_STORAGE_HISTORY_SLOTS]);
    }
}

static void read_keys(keyset_t *k)
{
    for (size_t i = 0; i < DATA_KEYS; i++) {
        char name[8];
        key_name(i, name);
        k->len[i] = 0;
        dsp_kv_status_t r = kv.get(kv.ctx, name, k->v[i], DSP_RECORD_MAX_BYTES, &k->len[i]);
        if (r != DSP_KV_OK) {
            printf("  key %s: read status %d (esp_err 0x%x)\n", name, (int)r, (unsigned)nvs.last_err);
        }
        CHECK(r == DSP_KV_OK);
    }
}

static void nvs_up(void)
{
    CHECK(dsp_nvs_kv_open(&nvs, "nvs", DSP_NVS_NAMESPACE, false, &kv) == DSP_NVS_OK);
}

/* Reset: the NVS library state is dropped and rebuilt from flash. */
static void power_cycle(void)
{
    pw_restore_power();
    dsp_nvs_kv_close(&nvs);
    nvs_flash_deinit_partition("nvs");
    nvs_up();
}

static dsp_storage_status_t open_storage(void)
{
    memset(&st, 0, sizeof st);
    dsp_storage_status_t s = dsp_storage_open(&st, &kv, TZ, true);
    dsp_storage_history_store(&st, &hist);
    if (s == DSP_STORAGE_OK) {
        s = dsp_schedule_repo_load(&repo, &st);
    }
    return s;
}

static void snapshot(void)
{
    CHECK(esp_partition_read(part, 0, snap, part->size) == ESP_OK);
}

static void restore(void)
{
    pw_restore_power();
    dsp_nvs_kv_close(&nvs);
    nvs_flash_deinit_partition("nvs");
    CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
    CHECK(esp_partition_write(part, 0, snap, part->size) == ESP_OK);
    nvs_up();
}

static nvs_stats_t stats(void)
{
    nvs_stats_t s;
    memset(&s, 0, sizeof s);
    CHECK(nvs_get_stats("nvs", &s) == ESP_OK);
    return s;
}

static dsp_schedule_t sched_max(char c, int i)
{
    dsp_schedule_t s;
    memset(&s, 0, sizeof s);
    memset(s.id, c, DSP_SCHEDULE_ID_MAX_LEN);
    s.revision = 1;
    s.enabled = true;
    s.amount_ul = 1000u + (uint32_t)i;
    s.start_date.year = 2026;
    s.start_date.month = 11;
    s.start_date.day = 2;
    s.hour = (uint8_t)(i % 24);
    s.kind = DSP_REC_DAILY;
    s.interval = 1;
    memset(s.name, 'n', DSP_SCHEDULE_NAME_MAX_BYTES);
    s.not_before = 1795158001;
    return s;
}

/* 16 schedules with 64-character IDs and 32-byte names, 16 tombstones: 3,160 B. */
/* Phase C: the event history at its maximum (64 events with 64-character schedule IDs). */
static dsp_evlog_t evlog;
static void fill_event_log_max(void)
{
    CHECK(dsp_evlog_open(&evlog, &kv));
    for (uint32_t i = 0; i < 2 * DSP_EVLOG_SLOTS; i++) { /* wraps once: every slot rewritten */
        dsp_event_t e;
        memset(&e, 0, sizeof e);
        e.type = DSP_EV_DOSE_SCHEDULED;
        e.flags = DSP_EVF_CLOCK_TRUSTED | DSP_EVF_PUMP_BENCH_GPIO;
        e.at = 1795158000 + (dsp_utc_t)i;
        e.volume_ul = 5000;
        e.before_ul = 100000;
        e.after_ul = 95000;
        e.capacity_ul = 100000;
        e.key = 0x1000u + i;
        memset(e.schedule_id, 'a' + (int)(i % 26), DSP_SCHEDULE_ID_MAX_LEN);
        CHECK(dsp_evlog_append(&evlog, &e, NULL) == DSP_EVLOG_OK);
    }
    CHECK(dsp_evlog_newest(&evlog) == 2 * DSP_EVLOG_SLOTS);
}

static void fill_max(void)
{
    for (int i = 0; i < 16; i++) {
        dsp_schedule_t s = sched_max((char)('a' + i), i);
        CHECK(dsp_schedule_repo_create(&repo, &s, &hist) == DSP_STORAGE_OK);
    }
    for (int i = 0; i < 16; i++) {
        dsp_schedule_t s = sched_max((char)('a' + i), i);
        CHECK(dsp_schedule_repo_delete(&repo, s.id, 1) == DSP_STORAGE_OK);
    }
    for (int i = 0; i < 16; i++) {
        dsp_schedule_t s = sched_max((char)('A' + i), i);
        CHECK(dsp_schedule_repo_create(&repo, &s, &hist) == DSP_STORAGE_OK);
    }
}

static size_t sched_len(void)
{
    uint8_t b[DSP_RECORD_MAX_BYTES];
    size_t len = 0;
    CHECK(kv.get(kv.ctx, "sched", b, sizeof b, &len) == DSP_KV_OK);
    return len;
}

/* One maximum-size rewrite of the set (schedule i: revision + 1). */
static dsp_storage_status_t update_one(int i, const char *name)
{
    const dsp_schedule_set_t *set = dsp_schedule_repo_set(&repo);
    if (set == NULL) {
        return DSP_STORAGE_ERR_RELOAD_REQUIRED;
    }
    dsp_schedule_t s = set->schedules[i % set->count];
    s.revision++;
    s.amount_ul ^= 1u;
    if (name != NULL) {
        memset(s.name, 0, sizeof s.name);
        strcpy(s.name, name);
    }
    return dsp_schedule_repo_update(&repo, &s);
}

/* PHY calibration data and Wi-Fi station configuration share the "nvs" partition on the
 * device (ESP-IDF defaults). Approximation of their footprint: a 1,904-byte PHY blob, and
 * 30 small values plus four blobs for Wi-Fi. */
static void add_cotenants(void)
{
    nvs_handle_t h;
    static uint8_t blob[2048];
    memset(blob, 0x5A, sizeof blob);
    CHECK(nvs_open_from_partition("nvs", "phy", NVS_READWRITE, &h) == ESP_OK);
    CHECK(nvs_set_blob(h, "cal_data", blob, 1904) == ESP_OK);
    CHECK(nvs_set_u32(h, "cal_version", 1) == ESP_OK);
    CHECK(nvs_set_blob(h, "cal_mac", blob, 6) == ESP_OK);
    CHECK(nvs_commit(h) == ESP_OK);
    nvs_close(h);
    CHECK(nvs_open_from_partition("nvs", "nvs.net80211", NVS_READWRITE, &h) == ESP_OK);
    for (int i = 0; i < 30; i++) {
        char k[16];
        snprintf(k, sizeof k, "wifi.k%02d", i);
        CHECK(nvs_set_u32(h, k, (uint32_t)i) == ESP_OK);
    }
    CHECK(nvs_set_blob(h, "sta.ssid", blob, 36) == ESP_OK);
    CHECK(nvs_set_blob(h, "sta.pswd", blob, 65) == ESP_OK);
    CHECK(nvs_set_blob(h, "sta.apinfo", blob, 700) == ESP_OK);
    CHECK(nvs_set_blob(h, "sta.pmk", blob, 128) == ESP_OK);
    CHECK(nvs_commit(h) == ESP_OK);
    nvs_close(h);
}


/* Cuts power at every cycle 1..n+1 of op() starting from the snapshot; after each cut the
 * storage must open and "sched" must be exactly old_set or new_set, everything else as in
 * old_keys. Returns the number of cuts that left the new set. */
typedef dsp_storage_status_t (*op_fn)(void);
static int cut_everywhere(op_fn op, const uint8_t *new_set, size_t new_len, const char *label)
{
    /* Measure the operation's cycles without a cut. */
    restore();
    CHECK(open_storage() == DSP_STORAGE_OK);
    pw_cycles = 0;
    CHECK(op() == DSP_STORAGE_OK);
    size_t n = pw_cycles;
    int as_new = 0, as_old = 0, opened = 0;
    const size_t sched_i = DSP_STORAGE_HISTORY_SLOTS;
    for (size_t k = 1; k <= n + 1; k++) {
        restore();
        CHECK(open_storage() == DSP_STORAGE_OK);
        pw_arm(k);
        (void)op();
        power_cycle();
        dsp_storage_status_t s = open_storage();
        if (s != DSP_STORAGE_OK) {
            printf("  %s: cut at cycle %zu of %zu: storage open failed (%d)\n", label, k, n, (int)s);
        }
        CHECK(s == DSP_STORAGE_OK);
        opened += s == DSP_STORAGE_OK;
        read_keys(&now_keys);
        for (size_t i = 0; i < DATA_KEYS; i++) {
            if (i == sched_i) {
                bool is_old = now_keys.len[i] == old_keys.len[i] &&
                              memcmp(now_keys.v[i], old_keys.v[i], old_keys.len[i]) == 0;
                bool is_new = now_keys.len[i] == new_len && memcmp(now_keys.v[i], new_set, new_len) == 0;
                CHECK(is_old || is_new);
                as_old += is_old;
                as_new += is_new;
            } else {
                CHECK(now_keys.len[i] == old_keys.len[i] &&
                      memcmp(now_keys.v[i], old_keys.v[i], old_keys.len[i]) == 0);
            }
        }
    }
    printf("  %s: %zu cycles, %zu cuts: storage opened %d, old set %d, new set %d\n", label, n,
           n + 1, opened, as_old, as_new);
    return as_new;
}

static dsp_storage_status_t op_upgrade(void)
{
    return update_one(0, "Upgraded");
}

static dsp_storage_status_t op_low_space(void)
{
    return update_one(3, "Low space");
}

static nvs_handle_t filler_open(void)
{
    nvs_handle_t f = 0;
    CHECK(nvs_open_from_partition("nvs", "filler", NVS_READWRITE, &f) == ESP_OK);
    return f;
}

/* Page 0 entry table (NVS format: 32 B header, 32 B state bitmap, 126 x 32 B entries). */
static void dump_page0(const char *when)
{
    uint8_t pg[4096];
    CHECK(esp_partition_read(part, 0, pg, sizeof pg) == ESP_OK);
    printf("      page 0 %s: state 0x%08" PRIx32 "\n", when,
           (uint32_t)pg[0] | (uint32_t)pg[1] << 8 | (uint32_t)pg[2] << 16 | (uint32_t)pg[3] << 24);
    static const char *st[] = {"ERASED", "ILLEGAL", "WRITTEN", "EMPTY"};
    for (int i = 0; i < 10; i++) {
        int bits = (pg[32 + i / 4] >> ((i % 4) * 2)) & 3;
        const uint8_t *e = pg + 64 + 32 * i;
        char key[16];
        for (int c = 0; c < 15; c++) {
            uint8_t ch = e[8 + c];
            key[c] = ch == 0 ? '\0' : (ch >= 0x20 && ch < 0x7F ? (char)ch : '.');
        }
        key[15] = '\0';
        printf("        entry %d %-7s ns %3u type 0x%02x span %u chunk 0x%02x key %s\n", i, st[bits],
               e[0], e[1], e[2], e[3], bits == 3 ? "-" : key);
    }
}

/* ---- 5. Raw NVS reproduction (no dispenser code): blob A replaced by blob B, cut at every
 * cycle; afterwards the key must read as exactly A or B. ---------------------------------- */
static int raw_blob_cuts(size_t len_a, size_t len_b, bool aged, const char *label)
{
    static uint8_t a[4096], b[4096], back[4096];
    memset(a, 0x11, sizeof a);
    memset(b, 0x22, sizeof b);
    pw_restore_power();
    dsp_nvs_kv_close(&nvs);
    nvs_flash_deinit_partition("nvs");
    CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
    CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
    nvs_handle_t h;
    CHECK(nvs_open_from_partition("nvs", "raw", NVS_READWRITE, &h) == ESP_OK);
    if (aged) { /* many prior rewrites: pages full of erased entries, reclamation needed */
        for (int i = 0; i < 300; i++) {
            b[0] = (uint8_t)i;
            CHECK(nvs_set_blob(h, "k", b, len_b) == ESP_OK);
        }
        b[0] = 0x22;
    }
    CHECK(nvs_set_blob(h, "k", a, len_a) == ESP_OK);
    nvs_close(h);
    nvs_flash_deinit_partition("nvs");
    snapshot();
    /* cycles of the replacement */
    CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
    CHECK(nvs_open_from_partition("nvs", "raw", NVS_READWRITE, &h) == ESP_OK);
    pw_cycles = 0;
    pw_trace = len_a == 32 && !aged;
    CHECK(nvs_set_blob(h, "k", b, len_b) == ESP_OK);
    pw_trace = false;
    size_t n = pw_cycles;
    nvs_close(h);
    nvs_flash_deinit_partition("nvs");
    int bad = 0, first_bad = -1;
    for (size_t k = 1; k <= n + 1; k++) {
        pw_restore_power();
        CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
        CHECK(esp_partition_write(part, 0, snap, part->size) == ESP_OK);
        CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
        CHECK(nvs_open_from_partition("nvs", "raw", NVS_READWRITE, &h) == ESP_OK);
        pw_arm(k);
        (void)nvs_set_blob(h, "k", b, len_b);
        nvs_close(h);
        nvs_flash_deinit_partition("nvs");
        pw_restore_power();
        CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
        CHECK(nvs_open_from_partition("nvs", "raw", NVS_READONLY, &h) == ESP_OK);
        size_t len = sizeof back;
        esp_err_t e = nvs_get_blob(h, "k", back, &len);
        bool is_a = e == ESP_OK && len == len_a && memcmp(back, a, len_a) == 0;
        bool is_b = e == ESP_OK && len == len_b && memcmp(back, b, len_b) == 0;
        if (!is_a && !is_b) {
            bad++;
            if (first_bad < 0) {
                first_bad = (int)k;
                printf("    cut at cycle %zu of %zu: get -> 0x%x, len %zu\n", k, n, (unsigned)e, len);
                if (len_a == 32 && !aged) { /* replay with dumps */
                    nvs_close(h);
                    nvs_flash_deinit_partition("nvs");
                    pw_restore_power();
                    CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
                    CHECK(esp_partition_write(part, 0, snap, part->size) == ESP_OK);
                    dump_page0("before the write");
                    CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
                    CHECK(nvs_open_from_partition("nvs", "raw", NVS_READWRITE, &h) == ESP_OK);
                    pw_arm(k);
                    (void)nvs_set_blob(h, "k", b, len_b);
                    nvs_close(h);
                    nvs_flash_deinit_partition("nvs");
                    pw_restore_power();
                    dump_page0("after the cut");
                    printf("      flash writes during re-initialization:\n");
                    pw_trace = true;
                    CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
                    pw_trace = false;
                    dump_page0("after re-initialization");
                    CHECK(nvs_open_from_partition("nvs", "raw", NVS_READONLY, &h) == ESP_OK);
                }
            }
        }
        nvs_close(h);
        nvs_flash_deinit_partition("nvs");
    }
    printf("  5. raw NVS %s: %zu -> %zu B%s: %zu cuts, %d lost/torn\n", label, len_a, len_b,
           aged ? " (aged partition)" : "", n + 1, bad);
    return bad;
}

static int sections_1_to_5(void)
{
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        printf("partition %-9s type %d subtype 0x%02x offset 0x%06" PRIx32 " size 0x%06" PRIx32 "\n",
               p->label, (int)p->type, (unsigned)p->subtype, p->address, p->size);
    }
    esp_partition_iterator_release(it);
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    CHECK(part != NULL && part->size == 0x14000 && part->address == 0x11000);
    if (part == NULL) {
        return 1;
    }
    printf("nvs partition: offset 0x%" PRIx32 ", size %" PRIu32 " B (%" PRIu32 " pages)\n",
           part->address, part->size, part->size / 4096);
    CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
    nvs_up();
    nvs_stats_t s0 = stats();

    /* ---- 1. Maximum-size records ------------------------------------------------------- */
    CHECK(open_storage() == DSP_STORAGE_OK);
    fill_max();
    CHECK(sched_len() == DSP_SCHEDULE_SET_MAX_BYTES);
    size_t ours_layout = 0;
    nvs_handle_t h;
    CHECK(nvs_open_from_partition("nvs", DSP_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK);
    CHECK(nvs_get_used_entry_count(h, &ours_layout) == ESP_OK);
    nvs_close(h);
    fill_event_log_max(); /* every later check runs with the full history (worst case) */
    size_t ours = 0;
    CHECK(nvs_open_from_partition("nvs", DSP_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK);
    CHECK(nvs_get_used_entry_count(h, &ours) == ESP_OK);
    nvs_close(h);
    add_cotenants();
    nvs_stats_t s1 = stats();
    printf("1. max records: schedule set %zu B; dispenser namespace %zu entries (layout %zu + "
           "full event history incl. reservation %zu); partition total %zu, used %zu, free %zu, available %zu "
           "(empty: available %zu)\n",
           sched_len(), ours, ours_layout, ours - ours_layout, s1.total_entries, s1.used_entries,
           s1.free_entries, s1.available_entries, s0.available_entries);
    CHECK(ours_layout <= 325); /* documented budget: 309 (M1.10b.2) + 15 for "auth" (layout v5) */
    CHECK(ours - ours_layout <= DSP_EVLOG_SLOTS * 6 + 3); /* Phase C: <= 6 per event + "evhw" */
    CHECK(s1.available_entries > 4 * 101); /* room for several full-set rewrites */
    power_cycle();
    CHECK(open_storage() == DSP_STORAGE_OK);

    /* ---- 2. Repeated updates ----------------------------------------------------------- */
    esp_partition_clear_stats();
    size_t used_ref = stats().used_entries;
    size_t used_max = used_ref;
    const int N = 20000;
    for (int i = 0; i < N; i++) {
        CHECK(update_one(i, NULL) == DSP_STORAGE_OK);
        if (i % 10 == 0) {
            dsp_config_t c;
            CHECK(dsp_storage_load_config(&st, &c) == DSP_STORAGE_OK);
            c.revision++;
            c.low_stock_percent = (uint8_t)(10 + i % 50);
            CHECK(dsp_storage_save_config(&st, &c) == DSP_STORAGE_OK);
        }
        if ((i + 1) % 2000 == 0) {
            power_cycle();
            CHECK(open_storage() == DSP_STORAGE_OK);
            size_t used = stats().used_entries;
            used_max = used > used_max ? used : used_max;
            CHECK(used <= used_ref + 8); /* no leak: same live data (blob chunking varies) */
        }
    }
    size_t first_sector = part->address / 4096;
    size_t max_erase = 0, min_erase = SIZE_MAX, total_erase = 0;
    for (size_t sec = first_sector; sec < first_sector + part->size / 4096; sec++) {
        size_t e = esp_partition_get_sector_erase_count(sec);
        max_erase = e > max_erase ? e : max_erase;
        min_erase = e < min_erase ? e : min_erase;
        total_erase += e;
    }
    printf("2. %d maximum-size set writes + %d config writes: %zu page erases (%.3f per set "
           "write), per page min %zu max %zu; used entries %zu at start, at most %zu at the reopen "
           "checks\n",
           N, N / 10, total_erase, (double)total_erase / N, min_erase, max_erase, used_ref, used_max);
    CHECK(max_erase <= 2 * (total_erase / (part->size / 4096)) + 2); /* wear is spread */

    /* ---- 2b. Phase C: repeated wraparound of the full event history ---------------------- */
    {
        esp_partition_clear_stats();
        size_t used0 = stats().used_entries, used_hi = used0;
        const int NE = 20000;
        CHECK(dsp_evlog_open(&evlog, &kv));
        uint32_t last = dsp_evlog_newest(&evlog);
        for (int i = 0; i < NE; i++) {
            dsp_event_t e;
            memset(&e, 0, sizeof e);
            e.type = DSP_EV_DOSE_SCHEDULED;
            e.flags = DSP_EVF_CLOCK_TRUSTED;
            e.at = 1795158000 + (dsp_utc_t)i;
            e.volume_ul = 5000;
            e.capacity_ul = 100000;
            memset(e.schedule_id, 'k', DSP_SCHEDULE_ID_MAX_LEN);
            uint32_t seq = 0;
            CHECK(dsp_evlog_append(&evlog, &e, &seq) == DSP_EVLOG_OK);
            CHECK(seq > last); /* never reused, across the power cycles below */
            last = seq;
            if ((i + 1) % 2000 == 0) {
                power_cycle();
                CHECK(open_storage() == DSP_STORAGE_OK); /* the safety layout is untouched */
                CHECK(dsp_evlog_open(&evlog, &kv));
                CHECK(evlog.faults == 0);
                CHECK(dsp_evlog_newest(&evlog) == last);
                size_t used = stats().used_entries;
                used_hi = used > used_hi ? used : used_hi;
                CHECK(used <= used0 + 8); /* bounded: overwrites only */
            }
        }
        size_t first = part->address / 4096, mx = 0, mn = SIZE_MAX, tot = 0;
        for (size_t sec = first; sec < first + part->size / 4096; sec++) {
            size_t e = esp_partition_get_sector_erase_count(sec);
            mx = e > mx ? e : mx;
            mn = e < mn ? e : mn;
            tot += e;
        }
        printf("2b. %d maximum-size event appends (each: reservation + event, %d wraparounds): %zu "
               "page erases (%.3f per append), per page min %zu max %zu; used entries %zu at "
               "start, at most %zu\n",
               NE, NE / DSP_EVLOG_SLOTS, tot, (double)tot / NE, mn, mx, used0, used_hi);
        CHECK(mx <= 2 * (tot / (part->size / 4096)) + 2); /* wear is spread */
    }

    /* ---- 3. Format 1 -> 2 upgrade, cut at every cycle ------------------------------------ */
    {
        /* The current set re-encoded in format 1 (91-byte entries, no names, no floors). */
        uint8_t f2[DSP_RECORD_MAX_BYTES], f1[DSP_RECORD_MAX_BYTES];
        size_t len2 = 0;
        CHECK(kv.get(kv.ctx, "sched", f2, sizeof f2, &len2) == DSP_KV_OK && len2 == 3160);
        size_t count = f2[6], tomb = f2[7];
        memcpy(f1, f2, 8);
        f1[1] = 1;
        size_t o = 8;
        for (size_t i = 0; i < count; i++, o += 91) {
            memcpy(f1 + o, f2 + 8 + i * 132, 91);
        }
        memcpy(f1 + o, f2 + 8 + count * 132, tomb * 65);
        size_t len1 = o + tomb * 65;
        CHECK(len1 == 2504);
        CHECK(kv.set(kv.ctx, "sched", f1, len1) == DSP_KV_OK && kv.commit(kv.ctx) == DSP_KV_OK);
        power_cycle();
        CHECK(open_storage() == DSP_STORAGE_OK);
        read_keys(&old_keys);
        snapshot();
        /* Expected upgraded record (no cut). */
        CHECK(op_upgrade() == DSP_STORAGE_OK);
        uint8_t up[DSP_RECORD_MAX_BYTES];
        size_t up_len = 0;
        CHECK(kv.get(kv.ctx, "sched", up, sizeof up, &up_len) == DSP_KV_OK);
        CHECK(up_len == 3160 && up[1] == 2);
        int as_new = cut_everywhere(op_upgrade, up, up_len, "3. format 1 -> 2 upgrade");
        CHECK(as_new >= 1);
        restore();
        CHECK(open_storage() == DSP_STORAGE_OK);
        CHECK(op_upgrade() == DSP_STORAGE_OK);
    }

    /* ---- 4. Low free space --------------------------------------------------------------- */
    {
        nvs_handle_t f = filler_open();
        static uint8_t blob[1024];
        memset(blob, 0xA5, sizeof blob);
        int nb = 0, nu = 0;
        char k[16];
        for (;; nb++) {
            snprintf(k, sizeof k, "b%04d", nb);
            if (nvs_set_blob(f, k, blob, sizeof blob) != ESP_OK) {
                break;
            }
        }
        for (;; nu++) {
            snprintf(k, sizeof k, "u%04d", nu);
            if (nvs_set_u32(f, k, (uint32_t)nu) != ESP_OK) {
                break;
            }
        }
        CHECK(nvs_commit(f) == ESP_OK);
        nvs_stats_t full = stats();
        read_keys(&old_keys);
        dsp_storage_status_t r = update_one(1, NULL);
        printf("4. partition filled (%d x 1 KiB + %d x u32): free %zu, available %zu entries; "
               "set write -> status %d\n", nb, nu, full.free_entries, full.available_entries, (int)r);
        CHECK(r == DSP_STORAGE_ERR_NO_SPACE || r == DSP_STORAGE_ERR_WRITE);
        power_cycle(); /* invalidates every handle */
        f = filler_open();
        CHECK(open_storage() == DSP_STORAGE_OK);
        read_keys(&now_keys);
        for (size_t i = 0; i < DATA_KEYS; i++) {
            CHECK(now_keys.len[i] == old_keys.len[i] &&
                  memcmp(now_keys.v[i], old_keys.v[i], old_keys.len[i]) == 0);
        }
        /* Free filler (u32 values, then 1 KiB blobs) until 500 consecutive maximum-size writes
         * succeed: the steady-state headroom the schedule set needs. */
        int freed_u = 0, freed_b = 0, ok = 0;
        nvs_stats_t at = stats();
        for (int step = 0; step < nu + nb; step++) {
            if (freed_u < nu) {
                snprintf(k, sizeof k, "u%04d", nu - 1 - freed_u);
                freed_u++;
            } else {
                snprintf(k, sizeof k, "b%04d", nb - 1 - freed_b);
                freed_b++;
            }
            CHECK(nvs_erase_key(f, k) == ESP_OK && nvs_commit(f) == ESP_OK);
            at = stats();
            ok = 0;
            while (ok < 500 && update_one(ok, NULL) == DSP_STORAGE_OK) {
                ok++;
            }
            if (ok == 500) {
                break;
            }
            CHECK(open_storage() == DSP_STORAGE_OK); /* a refused write: reload the set */
        }
        printf("   steady state: 500 consecutive set writes succeed after freeing %d values and "
               "%d KiB blobs; then free %zu, available %zu entries\n",
               freed_u, freed_b, at.free_entries, at.available_entries);
        CHECK(ok == 500);
        size_t needed = at.available_entries;
        printf("   steady-state headroom needed: %zu available entries (realistic device "
               "configuration has %zu: margin %zu)\n",
               needed, s1.available_entries, s1.available_entries - needed);
        CHECK(s1.available_entries > 2 * needed);
        /* A refused write at too little space leaves the durable set unchanged (checked above);
         * now cut at every cycle of a write at this minimal headroom (page reclamation). */
        nvs_close(f);
        power_cycle();
        CHECK(open_storage() == DSP_STORAGE_OK);
        snapshot();
        read_keys(&old_keys);
        pw_cycles = 0;
        CHECK(op_low_space() == DSP_STORAGE_OK);
        printf("   one write at minimal headroom: %zu cycles\n", pw_cycles);
        uint8_t nw[DSP_RECORD_MAX_BYTES];
        size_t nw_len = 0;
        CHECK(kv.get(kv.ctx, "sched", nw, sizeof nw, &nw_len) == DSP_KV_OK);
        int as_new = cut_everywhere(op_low_space, nw, nw_len, "4. write at minimal headroom");
        CHECK(as_new >= 1);
    }

    /* ---- 5. Raw NVS reproduction ------------------------------------------------------ */
    {
        const size_t sizes[][2] = {{2504, 3160}, {3160, 3160}, {3160, 2504}, {100, 3160},
                                   {2504, 2504}, {1000, 2000}, {32, 64}};
        int raw_bad = 0;
        for (int aged = 0; aged < 2; aged++) {
            for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
                raw_bad += raw_blob_cuts(sizes[i][0], sizes[i][1], aged != 0, "blob replace");
            }
        }
        printf("  5. raw NVS total lost/torn: %d\n", raw_bad);
    }

    return 0;
}


/* ---- 11. M1.10b.3: layout v4 -> v5 and "auth" writes, cut at every cycle ------------------ */

static bool is_key(size_t i, const char *name)
{
    char k[8];
    key_name(i, k);
    return strcmp(k, name) == 0;
}

/* Reads every key except "auth" (and "layout" when skip_layout). */
static void read_keys_except_auth(keyset_t *k, bool skip_layout)
{
    for (size_t i = 0; i < DATA_KEYS; i++) {
        k->len[i] = 0;
        if (is_key(i, "auth") || (skip_layout && is_key(i, "layout"))) {
            continue;
        }
        char name[8];
        key_name(i, name);
        CHECK(kv.get(kv.ctx, name, k->v[i], DSP_RECORD_MAX_BYTES, &k->len[i]) == DSP_KV_OK);
    }
}

static bool same_except_auth(const keyset_t *a, const keyset_t *b, bool skip_layout)
{
    for (size_t i = 0; i < DATA_KEYS; i++) {
        if (is_key(i, "auth") || (skip_layout && is_key(i, "layout"))) {
            continue;
        }
        if (a->len[i] != b->len[i] || memcmp(a->v[i], b->v[i], a->len[i]) != 0) {
            return false;
        }
    }
    return true;
}

static dsp_auth_t auth_target;
static dsp_storage_status_t op_open(void)
{
    return open_storage();
}
static dsp_storage_status_t op_save_auth(void)
{
    return dsp_storage_save_auth(&st, &auth_target);
}

static dsp_auth_t make_claimed(int phones)
{
    dsp_auth_t a;
    dsp_auth_provisioned(&a);
    uint8_t ip[32], iu[32], p[32];
    memset(ip, 0xA1, 32);
    memset(iu, 0xB1, 32);
    memset(p, 0x11, 32);
    CHECK(dsp_auth_claim(&a, ip, iu, p, "A", 1, 0) == DSP_AUTH_OK);
    if (phones > 1) {
        memset(p, 0x22, 32);
        CHECK(dsp_auth_add(&a, p, "B", 1, 0, NULL) == DSP_AUTH_OK);
    }
    return a;
}

static void auth_cuts(const dsp_auth_t *old_rec, const dsp_auth_t *new_rec, const char *label)
{
    restore();
    CHECK(open_storage() == DSP_STORAGE_OK);
    read_keys_except_auth(&old_keys, false);
    auth_target = *new_rec;
    pw_cycles = 0;
    CHECK(op_save_auth() == DSP_STORAGE_OK);
    size_t n = pw_cycles;
    int as_old = 0, as_new = 0;
    for (size_t k = 1; k <= n + 1; k++) {
        restore();
        CHECK(open_storage() == DSP_STORAGE_OK);
        pw_arm(k);
        (void)op_save_auth();
        power_cycle();
        dsp_storage_status_t s = open_storage();
        CHECK(s == DSP_STORAGE_OK);
        CHECK(st.auth_status == DSP_STORAGE_OK); /* never missing or damaged */
        dsp_auth_t got;
        bool loaded = dsp_storage_load_auth(&st, &got) == DSP_STORAGE_OK;
        CHECK(loaded);
        bool o = loaded && dsp_auth_equal(&got, old_rec);
        bool w = loaded && dsp_auth_equal(&got, new_rec);
        if (!(o || w)) {
            printf("  %s: cut at cycle %zu of %zu: neither old nor new record\n", label, k, n);
        }
        CHECK(o || w);
        as_old += o;
        as_new += w;
        read_keys_except_auth(&now_keys, false);
        CHECK(same_except_auth(&old_keys, &now_keys, false));
    }
    printf("  %s: %zu cycles, %zu cuts: old record %d, new record %d\n", label, n, n + 1, as_old, as_new);
    CHECK(as_old >= 1 && as_new >= 1);
}

static void section_11(void)
{
    pw_restore_power();
    dsp_nvs_kv_close(&nvs);
    nvs_flash_deinit_partition("nvs");
    CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
    nvs_up();
    CHECK(open_storage() == DSP_STORAGE_OK); /* provisions layout v5 */
    fill_max(); /* a populated device: maximum-size schedule set */
    CHECK(update_one(1, "Migrated") == DSP_STORAGE_OK);
    /* Make it the device an M1.10b.2 firmware left: no "auth", a v4 marker. */
    nvs_handle_t h;
    CHECK(nvs_open_from_partition("nvs", DSP_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK);
    CHECK(nvs_erase_key(h, "auth") == ESP_OK);
    CHECK(nvs_commit(h) == ESP_OK);
    nvs_close(h);
    const dsp_layout_t v4 = {4, DSP_STORAGE_HISTORY_SLOTS, DSP_SCHEDULE_CAPACITY, DSP_TOMBSTONE_CAPACITY};
    uint8_t m[DSP_LAYOUT_RECORD_BYTES];
    CHECK(dsp_codec_encode_layout(&v4, m, sizeof m) == sizeof m);
    CHECK(kv.set(kv.ctx, "layout", m, sizeof m) == DSP_KV_OK && kv.commit(kv.ctx) == DSP_KV_OK);
    power_cycle();
    snapshot();
    read_keys_except_auth(&old_keys, true);
    /* 11a. migration cut at every cycle */
    restore();
    pw_cycles = 0;
    CHECK(op_open() == DSP_STORAGE_OK);
    CHECK(st.migrated_now);
    size_t n = pw_cycles;
    int resumed = 0;
    for (size_t k = 1; k <= n + 1; k++) {
        restore();
        pw_arm(k);
        (void)op_open();
        power_cycle();
        dsp_storage_status_t s = open_storage();
        if (s != DSP_STORAGE_OK) {
            printf("  11a: cut at cycle %zu of %zu: storage open failed (%d)\n", k, n, (int)s);
        }
        CHECK(s == DSP_STORAGE_OK);
        CHECK(st.auth_status == DSP_STORAGE_OK);
        dsp_auth_t got;
        CHECK(dsp_storage_load_auth(&st, &got) == DSP_STORAGE_OK && dsp_auth_is_provisioning(&got));
        read_keys_except_auth(&now_keys, true);
        CHECK(same_except_auth(&old_keys, &now_keys, true));
        uint8_t lb[16];
        size_t ll = 0;
        dsp_layout_t lay;
        CHECK(kv.get(kv.ctx, "layout", lb, sizeof lb, &ll) == DSP_KV_OK &&
              dsp_codec_decode_layout(lb, ll, &lay) == DSP_CODEC_OK && lay.layout_version == 5);
        resumed += s == DSP_STORAGE_OK;
    }
    printf("11. v4 -> v5 migration of a populated device: %zu cycles, %zu cuts, opened as v5 %d\n",
        n, n + 1, resumed);
    /* 11b. "auth" writes: provisioned -> claimed, claimed -> claimed with a second phone */
    restore();
    CHECK(open_storage() == DSP_STORAGE_OK); /* migrated */
    power_cycle();
    snapshot();
    dsp_auth_t prov;
    dsp_auth_provisioned(&prov);
    dsp_auth_t one = make_claimed(1), two = make_claimed(2);
    auth_cuts(&prov, &one, "11b. auth write (claim)");
    restore();
    CHECK(open_storage() == DSP_STORAGE_OK);
    auth_target = one;
    CHECK(op_save_auth() == DSP_STORAGE_OK);
    power_cycle();
    snapshot();
    auth_cuts(&one, &two, "11b. auth write (add phone)");
}

/* ================= Modes: matrix, ab, dump, load (see run_checks.sh) ================= */

static uint8_t snap2[0x14000];

static void flash_put(const uint8_t *img)
{
    pw_restore_power();
    CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
    CHECK(esp_partition_write(part, 0, img, part->size) == ESP_OK);
}

static esp_err_t raw_get(const char *ns, const char *key, uint8_t *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open_from_partition("nvs", ns, NVS_READONLY, &h);
    if (e != ESP_OK) {
        return e;
    }
    e = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return e;
}

static esp_err_t raw_set(const char *ns, const char *key, const uint8_t *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open_from_partition("nvs", ns, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    e = nvs_set_blob(h, key, buf, len);
    nvs_close(h);
    return e;
}

/* 0 = A, 1 = B, -1 = lost or torn */
static int classify(const uint8_t *a, size_t la, const uint8_t *b, size_t lb)
{
    static uint8_t back[4096];
    size_t len = sizeof back;
    if (raw_get("raw", "k", back, &len) != ESP_OK) {
        return -1;
    }
    if (len == la && memcmp(back, a, la) == 0) {
        return 0;
    }
    if (len == lb && memcmp(back, b, lb) == 0) {
        return 1;
    }
    return -1;
}

typedef struct {
    long cuts1, cuts2, lost1, lost2, unstable, followup_lost, boots_failed;
} two_cut_t;

/* Writes blob A (optionally after aging), snapshots, then for every cut k of the A -> B
 * write and every cut j of the recovery that follows: reboot, check A|B, reboot again, check
 * the same value, then a follow-up write to another key and to k must survive a reboot. */
static two_cut_t two_cut(size_t len_a, size_t len_b, bool aged, int tear)
{
    static uint8_t a[4096], b[4096], c[64], back[4096];
    two_cut_t r;
    memset(&r, 0, sizeof r);
    memset(a, 0x11, sizeof a);
    memset(b, 0x22, sizeof b);
    memset(c, 0x33, sizeof c);
    pw_restore_power();
    dsp_nvs_kv_close(&nvs);
    nvs_flash_deinit_partition("nvs");
    CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
    CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
    if (aged) {
        for (int i = 0; i < 300; i++) {
            b[0] = (uint8_t)i;
            CHECK(raw_set("raw", "k", b, len_b) == ESP_OK);
        }
        b[0] = 0x22;
    }
    CHECK(raw_set("raw", "k", a, len_a) == ESP_OK);
    CHECK(raw_set("raw", "other", c, 16) == ESP_OK);
    nvs_flash_deinit_partition("nvs");
    snapshot();
    CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
    pw_cycles = 0;
    CHECK(raw_set("raw", "k", b, len_b) == ESP_OK);
    size_t n = pw_cycles;
    nvs_flash_deinit_partition("nvs");
    pw_tear_mode = tear;
    for (size_t k = 1; k <= n; k++) {
        flash_put(snap);
        CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
        pw_arm(k);
        (void)raw_set("raw", "k", b, len_b);
        nvs_flash_deinit_partition("nvs");
        pw_restore_power();
        CHECK(esp_partition_read(part, 0, snap2, part->size) == ESP_OK);
        r.cuts1++;
        /* recovery without a second cut: measure its cycles and the outcome */
        pw_cycles = 0;
        if (nvs_flash_init_partition("nvs") != ESP_OK) {
            r.boots_failed++;
            continue;
        }
        size_t rec = pw_cycles;
        int v1 = classify(a, len_a, b, len_b);
        r.lost1 += v1 < 0;
        /* follow-up writes after this single-cut recovery (#19214 exposure) */
        c[0] = (uint8_t)k;
        CHECK(raw_set("raw", "other", c, 16) == ESP_OK);
        uint8_t d[48];
        memset(d, 0x44, sizeof d);
        d[0] = (uint8_t)k;
        CHECK(raw_set("raw", "k", d, sizeof d) == ESP_OK);
        nvs_flash_deinit_partition("nvs");
        CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
        size_t len = sizeof back;
        bool ok_other = raw_get("raw", "other", back, &len) == ESP_OK && len == 16 && memcmp(back, c, 16) == 0;
        len = sizeof back;
        bool ok_k = raw_get("raw", "k", back, &len) == ESP_OK && len == sizeof d && memcmp(back, d, sizeof d) == 0;
        r.followup_lost += !(ok_other && ok_k);
        nvs_flash_deinit_partition("nvs");
        /* second cut at every cycle of the recovery */
        for (size_t j = 1; j <= rec; j++) {
            flash_put(snap2);
            pw_arm(j);
            (void)nvs_flash_init_partition("nvs");
            nvs_flash_deinit_partition("nvs");
            pw_restore_power();
            r.cuts2++;
            if (nvs_flash_init_partition("nvs") != ESP_OK) {
                r.boots_failed++;
                continue;
            }
            int v2 = classify(a, len_a, b, len_b);
            r.lost2 += v2 < 0;
            nvs_flash_deinit_partition("nvs");
            CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
            int v3 = classify(a, len_a, b, len_b);
            r.unstable += v3 != v2;
            nvs_flash_deinit_partition("nvs");
        }
    }
    pw_tear_mode = 0;
    nvs_up();
    return r;
}

static void section_6_7(void)
{
    const size_t sizes[][2] = {{2504, 3160}, {3160, 3160}, {3160, 2504}, {100, 3160},
                               {2504, 2504}, {1000, 2000}, {32, 64}};
    for (int tear = 0; tear < 2; tear++) {
        two_cut_t t;
        memset(&t, 0, sizeof t);
        for (int aged = 0; aged < 2; aged++) {
            for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
                two_cut_t r = two_cut(sizes[i][0], sizes[i][1], aged != 0, tear);
                printf("  %s %4zu -> %4zu%s: cuts %ld (+%ld in recovery): lost %ld / %ld, unstable %ld, "
                       "follow-up lost %ld, boot failures %ld\n",
                       tear ? "7. tear=first-word-erased" : "6. tear=prefix",
                       sizes[i][0], sizes[i][1], aged ? " aged" : "     ", r.cuts1, r.cuts2, r.lost1,
                       r.lost2, r.unstable, r.followup_lost, r.boots_failed);
                t.cuts1 += r.cuts1; t.cuts2 += r.cuts2; t.lost1 += r.lost1; t.lost2 += r.lost2;
                t.unstable += r.unstable; t.followup_lost += r.followup_lost; t.boots_failed += r.boots_failed;
            }
        }
        printf("  %s TOTAL: %ld write cuts, %ld recovery cuts; lost after 1 cut %ld, after 2 cuts %ld, "
               "unstable %ld, follow-up writes lost %ld, boot failures %ld\n",
               tear ? "7." : "6.", t.cuts1, t.cuts2, t.lost1, t.lost2, t.unstable, t.followup_lost,
               t.boots_failed);
        CHECK(t.lost1 == 0 && t.lost2 == 0 && t.unstable == 0 && t.followup_lost == 0 && t.boots_failed == 0);
    }
}

/* 8. #19213: 24-bit item hashes of every item this firmware's namespace can have. */
static void section_8(void)
{
    static const char *keys[DATA_KEYS];
    static char names[DATA_KEYS][8];
    for (size_t i = 0; i < DATA_KEYS; i++) {
        key_name(i, names[i]);
        keys[i] = names[i];
    }
    const uint8_t chunks[] = {0, 1, 2, 3, 0x80, 0x81, 0x82, 0x83, 0xff};
    long collisions = 0, items = 0;
    for (uint8_t ns = 1; ns <= 16; ns++) {
        static uint32_t h[DATA_KEYS * 9 + 1];
        static char tag[DATA_KEYS * 9 + 1][24];
        size_t n = 0;
        for (size_t i = 0; i < DATA_KEYS; i++) {
            for (size_t c = 0; c < sizeof chunks; c++) {
                uint8_t key[16];
                memset(key, 0, sizeof key);
                strncpy((char *)key, keys[i], sizeof key);
                uint32_t r = esp_rom_crc32_le(0xffffffff, &ns, 1);
                r = esp_rom_crc32_le(r, key, sizeof key);
                r = esp_rom_crc32_le(r, &chunks[c], 1);
                h[n] = r & 0xffffff;
                snprintf(tag[n], sizeof tag[n], "%s/0x%02x", keys[i], chunks[c]);
                n++;
            }
        }
        items += (long)n;
        for (size_t x = 0; x < n; x++) {
            for (size_t y = 0; y < x; y++) {
                if (h[x] == h[y]) {
                    collisions++;
                    printf("  8. collision ns %u: %s and %s (0x%06" PRIx32 ")\n", ns, tag[x], tag[y], h[x]);
                }
            }
        }
    }
    printf("  8. #19213 hash check: %ld item identities (38 keys x 9 chunk indexes x namespace "
           "index 1..16), %ld colliding pairs within a namespace\n", items, collisions);
}

/* 9. Image dump / load (compatibility across implementations). */
static void dump_image(const char *path)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f != NULL) {
        snapshot();
        CHECK(fwrite(snap, 1, part->size, f) == part->size);
        fclose(f);
    }
}

static bool load_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    bool ok = fread(snap, 1, part->size, f) == part->size;
    fclose(f);
    return ok;
}

/* Deterministic workload: storage provisioning, maximum set, 300 updates, configuration,
 * format 1 -> 2 upgrade, raw blobs and integers. */
static void workload(void)
{
    CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
    nvs_up();
    CHECK(open_storage() == DSP_STORAGE_OK);
    fill_max();
    for (int i = 0; i < 300; i++) {
        CHECK(update_one(i, NULL) == DSP_STORAGE_OK);
    }
    add_cotenants();
    static uint8_t blob[3000];
    for (int i = 0; i < 40; i++) {
        memset(blob, i, sizeof blob);
        CHECK(raw_set("raw", "k", blob, (size_t)(100 + 70 * i)) == ESP_OK);
    }
    power_cycle();
    CHECK(open_storage() == DSP_STORAGE_OK);
}

static void keys_file(const char *path, bool write)
{
    keyset_t *k = &now_keys;
    read_keys(k);
    static uint8_t blob[4096];
    size_t blen = sizeof blob;
    CHECK(raw_get("raw", "k", blob, &blen) == ESP_OK);
    if (write) {
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL && fwrite(k, sizeof *k, 1, f) == 1 && fwrite(&blen, sizeof blen, 1, f) == 1 &&
              fwrite(blob, 1, blen, f) == blen);
        if (f) fclose(f);
        return;
    }
    static keyset_t ref;
    static uint8_t rblob[4096];
    size_t rlen = 0;
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL && fread(&ref, sizeof ref, 1, f) == 1 && fread(&rlen, sizeof rlen, 1, f) == 1 &&
          fread(rblob, 1, rlen, f) == rlen);
    if (f) fclose(f);
    int diff = 0;
    for (size_t i = 0; i < DATA_KEYS; i++) {
        diff += ref.len[i] != k->len[i] || memcmp(ref.v[i], k->v[i], k->len[i]) != 0;
    }
    diff += rlen != blen || memcmp(rblob, blob, blen) != 0;
    printf("  9. loaded image: %zu keys compared, %d differ\n", (size_t)DATA_KEYS + 1, diff);
    CHECK(diff == 0);
}

/* 10. Redundant records (A/B) over the given NVS: record = u32 seq | payload, written to the
 * slot that does not hold the newest valid copy; read = newest valid copy. */
static esp_err_t ab_read(uint32_t *seq, uint8_t *payload, size_t *len)
{
    static uint8_t x[4100], y[4100];
    size_t lx = sizeof x, ly = sizeof y;
    bool vx = raw_get("ab", "r.a", x, &lx) == ESP_OK && lx >= 4;
    bool vy = raw_get("ab", "r.b", y, &ly) == ESP_OK && ly >= 4;
    if (!vx && !vy) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    uint32_t sx = 0, sy = 0;
    if (vx) memcpy(&sx, x, 4);
    if (vy) memcpy(&sy, y, 4);
    bool use_x = vx && (!vy || sx > sy);
    *seq = use_x ? sx : sy;
    *len = (use_x ? lx : ly) - 4;
    memcpy(payload, (use_x ? x : y) + 4, *len);
    return ESP_OK;
}
static esp_err_t ab_write(const uint8_t *payload, size_t len)
{
    static uint8_t x[4100], y[4100];
    size_t lx = sizeof x, ly = sizeof y;
    bool vx = raw_get("ab", "r.a", x, &lx) == ESP_OK && lx >= 4;
    bool vy = raw_get("ab", "r.b", y, &ly) == ESP_OK && ly >= 4;
    uint32_t sx = 0, sy = 0;
    if (vx) memcpy(&sx, x, 4);
    if (vy) memcpy(&sy, y, 4);
    uint32_t newest = sx > sy ? sx : sy;
    const char *slot = (vx && (!vy || sx > sy)) ? "r.b" : "r.a"; /* overwrite the older slot */
    static uint8_t rec[4100];
    uint32_t seq = newest + 1;
    memcpy(rec, &seq, 4);
    memcpy(rec + 4, payload, len);
    return raw_set("ab", slot, rec, len + 4);
}
static void section_10(void)
{
    const size_t sizes[][2] = {{2504, 3160}, {3160, 3160}, {3160, 2504}, {100, 3160},
                               {2504, 2504}, {1000, 2000}, {32, 64}};
    static uint8_t a[4096], b[4096], back[4096];
    for (int tear = 0; tear < 2; tear++) {
        long cuts = 0, lost_prev = 0;
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            size_t la = sizes[i][0], lb = sizes[i][1];
            memset(a, 0x11, sizeof a);
            memset(b, 0x22, sizeof b);
            pw_restore_power();
            dsp_nvs_kv_close(&nvs);
            nvs_flash_deinit_partition("nvs");
            CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
            CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
            for (int w = 0; w < 5; w++) { /* both slots populated */
                a[0] = (uint8_t)w;
                CHECK(ab_write(a, la) == ESP_OK);
            }
            nvs_flash_deinit_partition("nvs");
            snapshot();
            CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
            pw_cycles = 0;
            CHECK(ab_write(b, lb) == ESP_OK);
            size_t n = pw_cycles;
            nvs_flash_deinit_partition("nvs");
            pw_tear_mode = tear;
            for (size_t k = 1; k <= n; k++) {
                flash_put(snap);
                CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
                pw_arm(k);
                (void)ab_write(b, lb);
                nvs_flash_deinit_partition("nvs");
                pw_restore_power();
                CHECK(nvs_flash_init_partition("nvs") == ESP_OK);
                uint32_t seq = 0;
                size_t len = 0;
                bool ok = ab_read(&seq, back, &len) == ESP_OK &&
                          ((len == la && memcmp(back, a, la) == 0) || (len == lb && memcmp(back, b, lb) == 0));
                lost_prev += !ok;
                cuts++;
                nvs_flash_deinit_partition("nvs");
            }
            pw_tear_mode = 0;
        }
        printf("  10. A/B records, tear=%s: %ld cuts, previous-or-new value lost %ld\n",
               tear ? "first-word-erased" : "prefix", cuts, lost_prev);
    }
    nvs_up();
}

int main(void)
{
    const char *mode = getenv("MODE");
    const char *img = getenv("IMG");
    if (mode != NULL && strcmp(mode, "dump") == 0) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        workload();
        dump_image(img);
        char kp[512];
        snprintf(kp, sizeof kp, "%s.keys", img);
        keys_file(kp, true);
        /* a recoverable mid-write cut state (unrecovered) for cross-loading */
        snapshot();
        pw_arm(400);
        (void)update_one(5, "Cut");
        dsp_nvs_kv_close(&nvs);
        nvs_flash_deinit_partition("nvs");
        pw_restore_power();
        snprintf(kp, sizeof kp, "%s.cut", img);
        dump_image(kp);
    } else if (mode != NULL && strcmp(mode, "load") == 0) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        CHECK(load_image(img));
        flash_put(snap);
        nvs_up();
        CHECK(open_storage() == DSP_STORAGE_OK);
        char kp[512];
        snprintf(kp, sizeof kp, "%s.keys", img);
        keys_file(kp, false);
        /* the other implementation's unrecovered cut state */
        snprintf(kp, sizeof kp, "%s.cut", img);
        dsp_nvs_kv_close(&nvs);
        nvs_flash_deinit_partition("nvs");
        CHECK(load_image(kp));
        flash_put(snap);
        nvs_up();
        dsp_storage_status_t so = open_storage();
        printf("  9. other implementation's unrecovered cut state: storage open %d\n", (int)so);
        CHECK(so == DSP_STORAGE_OK);
        /* and it keeps working */
        CHECK(update_one(2, "After") == DSP_STORAGE_OK);
        power_cycle();
        CHECK(open_storage() == DSP_STORAGE_OK);
    } else if (mode != NULL && strcmp(mode, "ab") == 0) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
        nvs_up();
        section_10();
    } else if (mode != NULL && strcmp(mode, "matrix") == 0) {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        CHECK(esp_partition_erase_range(part, 0, part->size) == ESP_OK);
        nvs_up();
        section_8();
        section_6_7();
    } else {
        (void)sections_1_to_5();
        section_11();
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
