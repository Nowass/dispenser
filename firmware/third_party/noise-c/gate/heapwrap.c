/* Host-only allocation accounting for the probe (linker --wrap). Scratch measurement code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t);
void __real_free(void *);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t a, size_t b);
void __wrap_free(void *p);
void *__wrap_realloc(void *p, size_t n);

static size_t live, peak, total_allocs;
typedef struct {
    size_t n;
    size_t pad;
} hdr_t;

static void track(size_t n)
{
    live += n;
    total_allocs++;
    if (live > peak) {
        peak = live;
    }
}

void *__wrap_malloc(size_t n)
{
    hdr_t *h = __real_malloc(sizeof *h + n);
    if (!h) {
        return NULL;
    }
    h->n = n;
    track(n);
    return h + 1;
}

void *__wrap_calloc(size_t a, size_t b)
{
    void *p = __wrap_malloc(a * b);
    if (p) {
        memset(p, 0, a * b);
    }
    return p;
}

void __wrap_free(void *p)
{
    if (!p) {
        return;
    }
    hdr_t *h = (hdr_t *)p - 1;
    live -= h->n;
    __real_free(h);
}

void *__wrap_realloc(void *p, size_t n)
{
    void *q = __wrap_malloc(n);
    if (p && q) {
        hdr_t *h = (hdr_t *)p - 1;
        memcpy(q, p, h->n < n ? h->n : n);
        __wrap_free(p);
    }
    return q;
}

__attribute__((destructor)) static void report(void)
{
    fprintf(stderr, "HEAP peak=%zu B allocations=%zu leaked=%zu\n", peak, total_allocs, live);
}
