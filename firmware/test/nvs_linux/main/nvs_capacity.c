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
 *      several size pairs on a fresh and an aged partition (traced and dumped for 32 -> 64 B).
 *
 * FINDING (2026-11-16, ESP-IDF v6.1 fff9895c): every blob replacement has exactly one cut point
 * at which the key is lost (nvs_get_blob -> ESP_ERR_NVS_NOT_FOUND): power lost after the new
 * version's last data chunk is complete and before the new blob index is written. At the next
 * nvs_flash_init_partition(), Page::load() ("check that last item is not duplicate",
 * nvs_page.cpp) looks up the page's last item WITHOUT its chunk index and erases the OLD
 * version's data chunk as a "duplicate"; eraseMismatchedBlobIndexes() then deletes the old
 * index and eraseOrphanDataBlobs() the new chunk. The dispenser storage reports the missing key
 * as CORRUPT (fail closed). Checks 3 and 5 therefore fail by design until this is resolved.
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

#include "dsp_codec.h"
#include "dsp_nvs_kv.h"
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
            if (pw_left > 0) {
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

#define DATA_KEYS (DSP_STORAGE_HISTORY_SLOTS + 6) /* h00..h31, sched, config, time, notify, dosing, layout */

typedef struct {
    size_t len[DATA_KEYS];
    uint8_t v[DATA_KEYS][DSP_RECORD_MAX_BYTES];
} keyset_t;
static keyset_t old_keys, now_keys;

static void key_name(size_t i, char out[8])
{
    static const char *tail[] = {"sched", "config", "time", "notify", "dosing", "layout"};
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

int main(void)
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
    nvs_handle_t h;
    size_t ours = 0;
    CHECK(nvs_open_from_partition("nvs", DSP_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK);
    CHECK(nvs_get_used_entry_count(h, &ours) == ESP_OK);
    nvs_close(h);
    add_cotenants();
    nvs_stats_t s1 = stats();
    printf("1. max records: schedule set %zu B; dispenser namespace %zu entries; partition "
           "total %zu, used %zu, free %zu, available %zu (empty: available %zu)\n",
           sched_len(), ours, s1.total_entries, s1.used_entries, s1.free_entries,
           s1.available_entries, s0.available_entries);
    CHECK(ours <= 310);
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

    printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
