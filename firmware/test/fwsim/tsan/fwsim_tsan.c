/* ThreadSanitizer driver for the firmware simulator's connection lifecycle (TEST-ONLY).
 *
 * Compiles ../fwsim.c directly (the same source as libdsp_fwsim) with a minimal fake JNIEnv, so the
 * simulator's own threads (the firmware application task) and the phone-side JNI entry points run
 * natively under -fsanitize=thread, which is not possible inside a JVM. Never part of a firmware
 * image, the APK or the Android build. See README.md in this directory.
 *
 * Every round uses ONE reused BLE connection handle (FS_HANDLE_DEFAULT) and races:
 *   - a left-over poller of the previous link (keeps polling after its link ended),
 *   - a late writer and a late disconnector of the previous link,
 *   - an injector pushing notifications on the current link,
 *   - the current link's own poller,
 *   - real writes into the firmware transport (malformed fragments -> the transport's fault/close
 *     path on the application task), racing
 *   - the phone-side disconnect.
 * Functional checks: no cross-link delivery, nothing lost, duplicated or reordered, every stale
 * poller ends with GONE and never sees another link's event, late calls of an ended link are
 * refused, the firmware world reports no check failures or dropped radio events and its security
 * layer ends idle and wiped. Exit 0 only if every check passed and every round completed; TSan
 * reports additionally make the process exit non-zero (TSAN_OPTIONS=halt_on_error=1 in run.sh). */
#include "fwsim.c"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    jsize len;
    uint8_t d[];
} arr_t;

static jsize f_len(JNIEnv *e, jarray a) { (void)e; return ((arr_t *)a)->len; }
static jbyteArray f_newb(JNIEnv *e, jsize n)
{
    (void)e;
    arr_t *a = calloc(1, sizeof(arr_t) + (size_t)n * 4 + 4);
    a->len = n;
    return (jbyteArray)a;
}
static jintArray f_newi(JNIEnv *e, jsize n) { return (jintArray)f_newb(e, n); }
static void f_getb(JNIEnv *e, jbyteArray a, jsize s, jsize n, jbyte *buf) { (void)e; memcpy(buf, ((arr_t *)a)->d + s, (size_t)n); }
static void f_setb(JNIEnv *e, jbyteArray a, jsize s, jsize n, const jbyte *buf) { (void)e; memcpy(((arr_t *)a)->d + s, buf, (size_t)n); }
static void f_seti(JNIEnv *e, jintArray a, jsize s, jsize n, const jint *buf)
{
    (void)e;
    memcpy(((arr_t *)a)->d + (size_t)s * 4, buf, (size_t)n * 4);
}

static jboolean f_exc(JNIEnv *e) { (void)e; return JNI_FALSE; }
static struct JNINativeInterface_ FN;
static JNIEnv ENV = &FN;
static JNIEnv *E = &ENV;

static jbyteArray bytes(const uint8_t *b, jsize n)
{
    jbyteArray a = f_newb(E, n);
    memcpy(((arr_t *)a)->d, b, (size_t)n);
    return a;
}

static int fails;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                            \
        }                                                                       \
    } while (0)

#define N_EVENTS 40

static uint32_t ev_link(const arr_t *a) { return (uint32_t)a->d[3] | (uint32_t)a->d[4] << 8 | (uint32_t)a->d[5] << 16 | (uint32_t)a->d[6] << 24; }

typedef struct {
    jlong link;
    atomic_int stop;
    int gone, foreign, own;
    pthread_t th;
} stale_t;

static void *stale_poller(void *p)
{
    stale_t *s = p;
    while (!atomic_load(&s->stop) || s->gone == 0) {
        arr_t *e = (arr_t *)FS(poll)(E, NULL, s->link, 3);
        if (e == NULL) {
            continue;
        }
        if (e->d[0] == FS_EVT_GONE) {
            s->gone++;
        } else if (ev_link(e) != (uint32_t)s->link) {
            s->foreign++;
        } else {
            s->own++;
        }
        free(e);
    }
    return NULL;
}

typedef struct {
    jlong old;
    int writes_refused, writes;
} late_t;

static void *late_calls(void *p)
{
    late_t *l = p;
    uint8_t g[4] = {0xC0, 1, 2, 3};
    for (int i = 0; i < 20; i++) {
        jbyteArray a = bytes(g, 4);
        jint rc = FS(write)(E, NULL, l->old, a);
        free(a);
        l->writes++;
        if (rc == -2) {
            l->writes_refused++;
        }
        FS(disconnect)(E, NULL, l->old);
    }
    return NULL;
}

typedef struct {
    int round;
    int ok;
} inj_t;

static void *injector(void *p)
{
    inj_t *in = p;
    for (int i = 0; i < N_EVENTS; i++) {
        uint8_t b[6] = {0xC0, 0, 0, 0x7F, (uint8_t)(in->round & 0xFF), (uint8_t)i};
        jbyteArray a = bytes(b, 6);
        in->ok += FS(injectNotify)(E, NULL, a) ? 1 : 0;
        free(a);
    }
    return NULL;
}

static void *garbage_writer(void *p)
{
    jlong link = *(jlong *)p;
    uint8_t g[3] = {0x00, 0xFF, 0xEE}; /* a malformed fragment: the transport's fault path */
    for (int i = 0; i < 8; i++) {
        jbyteArray a = bytes(g, 3);
        (void)FS(write)(E, NULL, link, a);
        free(a);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 200;
    FN.GetArrayLength = f_len;
    FN.NewByteArray = f_newb;
    FN.NewIntArray = f_newi;
    FN.GetByteArrayRegion = f_getb;
    FN.SetByteArrayRegion = f_setb;
    FN.SetIntArrayRegion = f_seti;
    FN.ExceptionCheck = f_exc;

    uint8_t id[6] = {0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5}, setup[16];
    for (int i = 0; i < 16; i++) {
        setup[i] = (uint8_t)(0x30 + i);
    }
    jbyteArray ja = bytes(id, 6), jb = bytes(setup, 16);
    CHECK(FS(reset)(E, NULL, ja, jb));
    CHECK(FS(buttonWake)(E, NULL, JNI_FALSE));

    stale_t *prev = NULL;
    int done = 0;
    for (int r = 0; r < rounds; r++) {
        if (!FS(waitAdvertising)(E, NULL, 5000)) {
            fprintf(stderr, "no advertising in round %d (window ended?)\n", r);
            break;
        }
        late_t late = {prev ? prev->link : 0, 0, 0};
        pthread_t lt;
        if (prev) {
            pthread_create(&lt, NULL, late_calls, &late);
        }
        jlong token = FS(connect)(E, NULL, 247);
        CHECK(token != 0);
        jlong link = token >> 16;
        CHECK((token & 0xFFFF) == FS_HANDLE_DEFAULT);
        if (prev) {
            CHECK(link > prev->link);
        }
        inj_t in = {r, 0};
        pthread_t it;
        pthread_create(&it, NULL, injector, &in);
        int got = 0, order_ok = 1;
        while (got < N_EVENTS) {
            arr_t *e = (arr_t *)FS(poll)(E, NULL, link, 2000);
            CHECK(e != NULL);
            if (e == NULL) {
                break;
            }
            CHECK(e->d[0] == FS_EVT_NOTIFY);
            CHECK(ev_link(e) == (uint32_t)link);
            if (e->d[7 + 4] != (uint8_t)(r & 0xFF) || e->d[7 + 5] != (uint8_t)got) {
                order_ok = 0;
            }
            got++;
            free(e);
        }
        pthread_join(it, NULL);
        CHECK(in.ok == N_EVENTS);
        CHECK(order_ok);
        if (prev) {
            pthread_join(lt, NULL);
            CHECK(late.writes_refused == late.writes);
            atomic_store(&prev->stop, 1);
            pthread_join(prev->th, NULL);
            CHECK(prev->gone > 0);
            CHECK(prev->foreign == 0);
            free(prev);
        }
        /* this link's own poller keeps running into the next round (a left-over poller) */
        stale_t *s = calloc(1, sizeof *s);
        s->link = link;
        pthread_create(&s->th, NULL, stale_poller, s);
        /* real writes into the firmware transport racing the phone's disconnect */
        pthread_t gw;
        pthread_create(&gw, NULL, garbage_writer, &link);
        FS(disconnect)(E, NULL, link);
        pthread_join(gw, NULL);
        prev = s;
        done++;
    }
    if (prev) {
        atomic_store(&prev->stop, 1);
        pthread_join(prev->th, NULL);
        CHECK(prev->gone > 0 && prev->foreign == 0);
        free(prev);
    }
    FS(advance)(E, NULL, 24LL * 3600 * 1000);
    CHECK(FS(joinWake)(E, NULL, 20000));
    arr_t *d = (arr_t *)FS(diagnostics)(E, NULL);
    jint dg[7];
    memcpy(dg, d->d, sizeof dg);
    free(d);
    CHECK(dg[0] == 0); /* DSP_CHECK failures in the firmware world */
    CHECK(dg[1] == 0); /* dropped radio events */
    CHECK(dg[2] == 1); /* security layer idle and wiped */
    printf("rounds %d/%d, stale discarded %d, failures %d\n", done, rounds, dg[6], fails);
    return fails == 0 && done == rounds ? 0 : 1;
}
