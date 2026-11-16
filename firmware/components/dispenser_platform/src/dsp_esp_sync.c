#include "dsp_esp_app.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Command-queue lock for dsp_app_post() (future NimBLE host task) and the application
 * task's pop. A portMUX critical section: on the single-core ESP32-C3 it masks interrupts
 * and suspends scheduling for the few instructions that copy one command (bounded, no
 * blocking call, no logging, no allocation inside). Task context only: never call
 * dsp_app_post() from an ISR. The BLE task never touches domain state: dsp_app_post()
 * accesses only the queue. Waking the application task after a post (a semaphore given
 * OUTSIDE the critical section) belongs to the BLE adapter (later milestone).
 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void lock(void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL(&s_mux);
}

static void unlock(void *ctx)
{
    (void)ctx;
    taskEXIT_CRITICAL(&s_mux);
}

void dsp_esp_app_sync(dsp_app_sync_t *out)
{
    out->ctx = NULL;
    out->lock = lock;
    out->unlock = unlock;
}
