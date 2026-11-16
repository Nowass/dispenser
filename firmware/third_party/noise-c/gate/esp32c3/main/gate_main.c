/* M1.10a gate: links the Noise self-test so the real handshake/transport code is measured.
   Prints heap/stack figures when run (hardware or emulator); the gate only builds it. */
#include <stdio.h>

#include <sodium.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

int nprobe_selftest(void);

void app_main(void)
{
    if (sodium_init() < 0) {
        printf("GATE sodium_init failed\n");
        return;
    }
    size_t before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    int fails = nprobe_selftest();
    size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    printf("GATE fails=%d heap_before=%u heap_min=%u stack_free_min=%u\n", fails, (unsigned)before,
           (unsigned)min_free, (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
}
