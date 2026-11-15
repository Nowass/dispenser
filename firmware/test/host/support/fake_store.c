#include "fake_store.h"

#include <string.h>

static dsp_store_status_t fake_read(void *ctx, size_t slot, dsp_history_record_t *out)
{
    fake_store_t *f = ctx;
    if (slot >= f->slot_count || out == NULL) {
        return DSP_STORE_ERR_READ;
    }
    if (f->fail_reads) {
        return DSP_STORE_ERR_READ;
    }
    if (f->fail_read_after > 0) {
        if (--f->fail_read_after == 0) {
            f->fail_reads = true; /* this and all further reads fail */
            return DSP_STORE_ERR_READ;
        }
    }
    f->reads++;
    if (!f->durable[slot].used) {
        return DSP_STORE_EMPTY;
    }
    *out = f->durable[slot].rec;
    if (f->corrupt_armed) {
        f->corrupt_armed = false;
        out->claimed_at += 1;
    }
    return DSP_STORE_OK;
}

static dsp_store_status_t fake_write(void *ctx, size_t slot, const dsp_history_record_t *rec)
{
    fake_store_t *f = ctx;
    if (slot >= f->slot_count || rec == NULL || f->fail_writes) {
        return DSP_STORE_ERR_WRITE;
    }
    f->writes++;
    f->staged[slot].used = true;
    f->staged[slot].rec = *rec;
    f->dirty[slot] = true;
    return DSP_STORE_OK;
}

static void drop_staged(fake_store_t *f)
{
    memset(f->dirty, 0, sizeof f->dirty);
    memset(f->staged, 0, sizeof f->staged);
}

static void make_durable(fake_store_t *f)
{
    for (size_t i = 0; i < f->slot_count; i++) {
        if (f->dirty[i]) {
            f->durable[i] = f->staged[i];
        }
    }
    drop_staged(f);
}

static dsp_store_status_t fake_commit(void *ctx)
{
    fake_store_t *f = ctx;
    f->commits++;
    if (f->corrupt_next_readback) {
        f->corrupt_next_readback = false;
        f->corrupt_armed = true;
    }
    if (f->power_loss_at_commit || f->fail_commits) {
        drop_staged(f);
        return DSP_STORE_ERR_COMMIT;
    }
    if (f->commit_fails_but_persists) {
        make_durable(f);
        return DSP_STORE_ERR_COMMIT;
    }
    make_durable(f);
    return DSP_STORE_OK;
}

void fake_store_init(fake_store_t *f, size_t slot_count)
{
    memset(f, 0, sizeof *f);
    f->slot_count = slot_count;
    f->iface.ctx = f;
    f->iface.slot_count = slot_count;
    f->iface.read = fake_read;
    f->iface.write = fake_write;
    f->iface.commit = fake_commit;
}

void fake_store_power_cycle(fake_store_t *f)
{
    drop_staged(f);
}

void fake_store_clear_faults(fake_store_t *f)
{
    f->fail_reads = false;
    f->fail_read_after = 0;
    f->fail_writes = false;
    f->fail_commits = false;
    f->commit_fails_but_persists = false;
    f->power_loss_at_commit = false;
    f->corrupt_next_readback = false;
    f->corrupt_armed = false;
    f->reads = 0;
    f->writes = 0;
    f->commits = 0;
}
