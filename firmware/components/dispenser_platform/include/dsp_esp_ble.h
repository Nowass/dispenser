#ifndef DSP_ESP_BLE_H
#define DSP_ESP_BLE_H

/*
 * NimBLE adapter for the BLE byte transport (M1.10d.1; docs/BLE_TRANSPORT_DESIGN.md sections 3,
 * 4, 6, 9, 10). ESP-IDF only, compiled with CONFIG_DSP_BLE_TRANSPORT. Cross-built only: NOT run
 * on hardware yet (M1.10d.2 and M1.10c.2b are open).
 *
 *  - GATT: one primary service 36b60001-... with Info (read, 32 bytes), Request (write with
 *    response, one fragment per write) and Response (notify). No pairing, bonding or ATT
 *    security (security is the application layer); no other service besides GAP/GATT.
 *  - Advertising: connectable undirected legacy, public address (T11; never an identity),
 *    flags + 128-bit service UUID; scan response: shortened name "DSP2" + service data with the
 *    4-byte payload. 100 ms for 30 s after the window start and every disconnect, then 500 ms.
 *  - Init = NimBLE port init (controller enabled) + GATT registration + host sync; deinit =
 *    host stopped, port deinitialized (controller disabled). The controller stays enabled while
 *    the window is suspended for a pump run (T12); RF activity stops (advertising off, link
 *    closed). The RNG adapter's entropy prerequisite (dsp_esp_rng_ready) is the controller
 *    state: no handshake can start before init() and none after deinit().
 *  - The transport's retry callout runs on the NimBLE host task's event queue.
 */

#include <stdbool.h>

#include "dsp_app.h"
#include "dsp_ble_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Binds the singleton adapter to the Application Service and fills *port (dsp_app_config_t.ble).
 * No radio activity until the port's start(). false: the FreeRTOS objects could not be made. */
bool dsp_esp_ble_port(dsp_app_t *app, dsp_ble_port_t *port);

/* Per-window transport diagnostics (contention evidence, T1). */
void dsp_esp_ble_stats(dsp_ble_tr_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DSP_ESP_BLE_H */
