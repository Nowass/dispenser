# Dispenser V2 — NimBLE BLE transport (M1.10d)

Status: **revision 3. Design approved with adjustments (review 2026-11-18); M1.10d.1 technical
review approved in principle (T3b, generation-bound port, 3-frame ring, bounded suspend/stop wait,
normative GATT); T13 and T14 clarified in sections 13.1–13.2; uncommitted, awaiting final
confirmation. M1.10d.2 (hardware) OPEN** together with M1.10c.2b.
Baseline: `docs/BLE_PROTOCOL.md` v1.0 (sections 2–3 amended here), `docs/BLE_PROTOCOL_DESIGN.md`
revision 3, `docs/SECURITY_INTEGRATION_DESIGN.md` revision 4 (M1.10c.1/2a committed as `613f146` /
`c102343`). Decisions are marked **[Tn]** and collected in section 13.

Split: **M1.10d.1** software implementation with host and cross-build verification (this
revision); **M1.10d.2** hardware acceptance (`firmware/HARDWARE_ACCEPTANCE.md` 8b, H1–H10), open
until hardware is available.

Scope: a **byte transport** between NimBLE and the Application Service (advertising, GATT server,
connection lifecycle, MTU, fragmentation and reassembly, notifications, backpressure). Not in
scope: cryptographic changes, the button driver (M10), Android (M1.10e), hardware, eFuses,
partitions, credentials, production services.

---

## 1. Starting point

| Item | State before M1.10d |
|---|---|
| Application Service | single owner; BLE stage only on a button wake, between the dose/CONNECT stages and SYNC; the runtime set `cfg.ble = NULL` |
| Port contract | `dsp_ble_port_t`: start, connected, suspend, resume, stop, is_off, wait, `send_frame(frame)`, `disconnect()`, publish_info — byte-only since M1.10c.1 |
| Queue | `dsp_app_post_frame`, `dsp_app_post_conn_event`, `dsp_app_post_button` under `dsp_app_sync_t`; **4 slots shared by frames, connection events and buttons** |
| Security | `dsp_blesec` (pre-auth gate, Noise, dispatcher, cache) in the application task |
| Codec | `dsp_ble_frag_*`, `dsp_ble_reasm_*` (one partial frame, 520 B, 2 s gap), shared vectors |
| RNG | `dsp_esp_rng`: entropy "active" only while the BT controller is enabled |
| Button | `CONFIG_DSP_BUTTON_WAKE_GPIO = -1` until M10: no button wake in any image |

**Superseded statements.** BLE_PROTOCOL_DESIGN 4.7 "three failed handshakes stop advertising" →
C10 (T9). BLE_PROTOCOL_DESIGN 11 "NimBLE not estimated" → measured (section 12). BLE_PROTOCOL
section 3 "report MALFORMED" for every fragment error → only before a handshake started (T3,
protocol text amended). The plain `EV_BUSY` event is no longer sent (T3b, protocol table marked).

## 2. Architecture

```text
 NimBLE host task                                        Application task (single owner)
 ───────────────────────────                             ───────────────────────────────
 dsp_esp_ble.c (ESP-IDF only)                            dsp_app (stage_ble loop)
   GAP/GATT callbacks, retry callout ──► dsp_ble_tr_on_*    │ port: wait, send_frame(gen, …),
            │                                               │ disconnect(gen), suspend/resume/stop,
            ▼                                               ▼ publish_info
 dsp_ble_transport.c (PURE C, host-tested; no ESP-IDF headers)
   connection record {handle, gen, mtu, subscribed, closing}   TX ring (3 frames) + pump
   RX reassembly (host task only)                              advertising control (app task only)
            │ dsp_app_post_frame / _conn_event / _rx_fault      │ radio ops (adv, notify, terminate,
            ▼ (queue lock)                                      ▼ retry_arm, init/deinit, …)
     dsp_app: bounded frame queue (4)  +  connection snapshot {gen, up, mtu, faults} (T2)
```

Rules:

1. **Bytes only.** RX: fragments are reassembled with the shared codec and the complete frame is
   posted unchanged. TX: frames given to `send_frame` are fragmented unchanged. The transport
   never parses a body, **never sends a protocol message of its own** (no MALFORMED, no EV_BUSY),
   never includes `dsp_app_sec.h` (the existing header guard covers every component), never
   creates an authorization, never touches dosing, storage or session state. A link-time check
   (`test_ble_transport_symbols`) fails the host build if the transport object references any
   external symbol other than the three producers, the fragmentation codec and `mem*`.
2. **Generations.** Each accepted GAP connection gets `gen = ++counter` (32 bit, never 0). Every
   frame, connection event and fragment-error report carries it; `send_frame` and `disconnect`
   take it and are refused unless it is the transport's current, open connection.
3. **Bounded memory, no allocation.** One RX partial frame (532 B), a TX ring of 3 frames (3 ×
   536 B), one fragment scratch (244 B), Info (32 B) and advert (4 B) — `sizeof(dsp_ble_transport_t)`
   is in section 12.
4. **Threads.** NimBLE callbacks never block, never wait for the application task and never hold
   a lock across a radio or `dsp_app_post_*` call; the transport lock and the queue lock never
   nest (section 6).
5. **Invariant (stale delivery).** Advertising is (re)started only from the application task.

### 2.1 Modules and interfaces

| Module | Location | Builds |
|---|---|---|
| `dsp_ble_transport.{h,c}` | `dispenser_platform` (pure) | firmware with `CONFIG_DSP_BLE_TRANSPORT`; host tests |
| `dsp_esp_ble.{h,c}` | `dispenser_platform` (ESP-IDF) | firmware with `CONFIG_DSP_BLE_TRANSPORT` |
| `main.c` | composition root | `cfg.ble` only when the security layer registered (never a BLE service without it); identity for the Info record; T7 flag |

Radio interface (`dsp_ble_radio_t`, implemented by `dsp_esp_ble.c` and by host fakes): `init`,
`deinit`, `is_off`, `adv_start(advert, fast)`, `adv_stop`, `terminate(handle)`,
`notify(handle, frag) → OK | BUSY | ERR` (non-blocking), `retry_arm(ms)` (one-shot, fires
`dsp_ble_tr_on_retry` **in the host task**: a NimBLE callout on the default event queue),
`signal`, `wait(max_ms)`, `now_ms`, `lock`, `unlock`.

Port contract change (consequence of the T2 requirement "stale responses are never delivered to
the new connection"): `send_frame(ctx, gen, frame, len)` and `disconnect(ctx, gen)`; the
Application Service passes its current connection generation.

### 2.2 Review adjustments and where they are met

| Requirement (review 2026-11-18) | Mechanism | Evidence (host) |
|---|---|---|
| T2: a new generation invalidates all previous authorization and transport state | app: `link_sync` closes the open connection whenever the snapshot names another generation or is down (`conn_end` → security `on_connection(false)` zeroizes, authorization ends, new session ID per connection); transport: `conn_clear` (record, TX ring, fault count) on every connect/disconnect, reassembly reset per generation | `test_session` `test_link_reconcile`; `test_ble_transport` lifecycle, disconnect-during-TX/RX |
| T2: disconnect followed immediately by connect cannot preserve the session | the snapshot is reconciled before every queue item; a generation is opened at most once; CONNECTED must be strictly newer | `test_link_reconcile` modes a, d (1000-cycle storm) |
| T2: stale queued frames and responses never reach the new connection | frames carry their generation (stale in the app); `send_frame`/`disconnect` are generation-bound in the transport; TX ring flushed at every change; invariant 5 | `test_link_reconcile` (`stale_sends` 0), `test_disconnect_during_tx_and_stale`, two-thread test (0 cross-link fragments) |
| T2: queue saturation cannot hide a disconnect | connection events are not queue items: snapshot beside the queue, never refused for space | `test_link_reconcile` saturation case |
| T2: sequences with the app task not running between callbacks | hooks post DISCONNECTED/CONNECTED/DISCONNECTED (and 1000 cycles) inside one step | `test_link_reconcile` modes a–d |
| T3: no plaintext error after authentication; tag failure never answered in plaintext | the transport only reports (`dsp_app_post_rx_fault`); `dsp_blesec` `on_rx_fault`: plain MALFORMED only in OPEN/NEGOTIATED, else close without reply; a missing hook closes (fail closed); decryption failures unchanged (close, no reply) | `test_blesec`/`test_ble_e2e` `test_rx_fault_policy`; `test_transport_manipulations` |
| T5: asynchronous retries, no blocking in callbacks, no lock while waiting | BUSY → `retry_arm(20 ms)` and return; completion events re-run the pump; `send_frame` never waits | `test_tx_backpressure` (returns at once, retry is a callout) |
| T5: bounded count + absolute deadline; on failure close and release buffers | 25 consecutive refusals or 1000 ms per frame → ring flushed, terminate | `test_tx_backpressure` |
| T5: buffer exhaustion, disconnect during TX, stale completions, timeout cleanup | — | `test_tx_backpressure`, `test_disconnect_during_tx_and_stale` |
| T7: default off, refused with production, no bypass of authentication / entropy, no deterministic RNG | `CONFIG_DSP_BLE_BRINGUP_POWER_ON_WINDOW` → `cfg.service_on_power_on` (presence only); CMake refuses it with `ALLOW_PRODUCTION` | `test_session` `test_power_on_window_option` |
| T8: negotiated MTU; MTU 23 and every boundary to 247 | MTU captured at a frame's first fragment; clamp to 247 | `test_fragmentation_all_mtus` (23…247 + 517), e2e at 23/64/247 |
| T12: BLE fully suspended during pump runs; teardown and RNG state | section 9 | `test_lifecycle` (suspended: no send, no write, no advertising, links refused) |

## 3. GATT service (topic 1)

Normative text: `BLE_PROTOCOL.md` section 2. Info (read, 32 B, served from the transport's copy
under its lock), Request (write with response, one fragment, longer writes → ATT 0x0D — covers
prepared/long writes, T10), Response (notify + CCCD, no indications, T10). GAP device name
`DSP2`, no other services, no ATT/SMP security (`CONFIG_BT_NIMBLE_SECURITY_ENABLE=n`).
Frames for an unsubscribed client are refused (`send_frame` false, counted); the client times out.
The ATT write response is returned after the fragment was handled (posted, refused or reported).

## 4. Advertising (topic 2)

- Only during a service window; started by `start()` + the first `publish_info` (so the payload is
  never empty), restarted by `resume()`, after disconnects and failures **in `wait()`** (invariant
  5), and when the published payload changes; stopped by `suspend()`/`stop()` and by a connection.
- Connectable undirected legacy, public address (T11). Advertising data: Flags + complete list of
  128-bit UUIDs (21 B). Scan response: shortened name `DSP2` + Service Data UUID128 + 4-byte
  payload (28 B). No Manufacturer Specific Data (D14).
- T4: 100 ms for 30 s after window start and after each disconnect, then 500 ms (switched in
  `wait()`, which never sleeps past the fast-window end while advertising).

## 5. MTU, fragmentation, reassembly (topic 3)

| Item | Implementation |
|---|---|
| MTU | preferred 247 (`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU`); the negotiated value from `BLE_GAP_EVENT_MTU`, clamped to 247; 23 until then |
| Frame MTU | captured when a frame's first fragment is sent (T8); a change applies to the next frame |
| RX | one `dsp_ble_reasm_t`, host task only, reset when the generation changes; writes longer than `mtu − 3` refused |
| Fragment error | discard; count; `dsp_app_post_rx_fault(gen)`; the application decides (T3, effective limit); the transport's backstop closes after **8** on one connection only if the application has not acted (section 13.2) |
| Complete frame | `dsp_app_post_frame(gen)`; refused → dropped silently, counted (`rx_refused`) (T3b) |

## 6. Buffer ownership and threading (topic 4)

| State | Writer | Reader | Guard |
|---|---|---|---|
| connection record, window/advertising flags, TX ring, stats, Info/advert | both tasks | both | transport lock (FreeRTOS mutex; short sections, no calls out) |
| RX reassembly + its generation | host task only | host task only | — |
| fragment scratch | the pump owner | the pump owner | `pumping` flag claimed under the lock |
| connection snapshot, frame queue | transport (post) / app (pop) | — | queue lock (`dsp_app_sync_t`) |

**Pump.** Notifications are sent by whichever task currently owns the pump (claimed under the
lock; a second caller sets `pump_again` and returns). The owner releases the lock around each
`notify`; afterwards a changed `tx_epoch` (the ring was flushed by a disconnect) or a vanished link
stops it. `send_frame` copies the frame into a free ring slot and runs the pump once — it never
waits. BUSY arms the retry callout (host task) and returns; `BLE_GAP_EVENT_NOTIFY_TX` also re-runs
the pump. Failure (25 consecutive BUSY, 1000 ms from `send_frame`, or ERR) flushes the ring and
terminates the link.

**Why a stale fragment cannot reach a newer link (invariant 5).** The generation check before a
`notify` and the `notify` itself are not atomic with respect to NimBLE. A newer link on the same
connection handle requires advertising, which only the application task restarts. Therefore: (a)
while the application task is inside a `notify`, no newer link can appear; (b) the host task
processes connection events and its own pump runs (completions, callout) sequentially. The
two-thread test exercises this with one reused handle (0 violations in ~500 links per run); the
unit test deliberately breaks the invariant inside `notify` and shows that at most the one fragment
already handed to the stack leaves and the ring stays consistent.

**Locks.** Transport lock → never held across radio calls or `dsp_app_post_*`; queue lock → held
only inside `dsp_app` (copy in/out), never across a port call. No nesting; the host fakes use
`PTHREAD_MUTEX_ERRORCHECK`, which aborts on nesting.

## 7. Flow control and connection state (topics 5, 7)

- Client → device: write with response, one outstanding request (protocol 6).
- Device → client: TX ring of 3 frames (response + up to two events); `send_frame` false when full.
- **T2 — connection snapshot.** `dsp_app` keeps `{gen, up, mtu, faults}` beside the frame queue,
  written by `dsp_app_post_conn_event` / `dsp_app_post_rx_fault` under the queue lock:
  CONNECTED(g) only if g is greater than every generation reported in the wake (refused otherwise);
  DISCONNECTED(g) of the newest generation marks it down, an older one is ignored; never refused
  for lack of space. The application copies the snapshot (in the same critical section as each
  pop) and reconciles before handling the item: close if the snapshot's generation differs or is
  down; open the snapshot's generation if up and newer than the last one opened (each generation at
  most once); handle at most 3 new fragment errors. `q_open` clears `up` at every window start and
  end (generations keep increasing within the wake).
- **T3b — no busy notice.** A refused frame is dropped without a reply (the client's timeout and
  the response cache recover, C4); the transport cannot know whether the connection is
  authenticated, and a plaintext notice after authentication is excluded by T3.

## 8. Connection lifecycle (topic 5)

| Event | Transport | Application (via snapshot) |
|---|---|---|
| GAP connect, window active, no link | record `{h, ++gen, mtu 23}`, flush ring | opens `gen` (session ID from the RNG, gate, security `on_connection(true)`) |
| GAP connect while suspended/stopping or a second link | terminate at once; never reported (gen 0) | — |
| GAP connect failure / advertising ended | `adv_pending` → restarted in `wait()` | — |
| MTU / subscribe | record update (current handle only) | — |
| write | reassemble; frame → post; error → report (+ transport limit 8) | frame or fault handled after reconcile |
| GAP disconnect (current handle) | record cleared, ring released, `adv_pending` | closes `gen` (zeroize) |
| callbacks for another handle | ignored | — |
| `disconnect(gen)` | `closing` at once (no new sends/writes); terminate after the ring drained + 100 ms grace (best effort: a final reply such as the END_SESSION response may leave first; section 13.1), or at the frame deadline | `conn_end` immediately |
| `suspend()` | window suspended (links refused), advertising off, close as above, wait ≤ 1.5 s for GAP; else force terminate and report DISCONNECTED itself | `conn_end` (after `on_suspend`'s best-effort EV_SUSPENDING_FOR_DOSE, which drains first) |
| `stop()` | advertising off, close, wait ≤ 1.5 s, record cleared, `deinit` (controller disabled) | queue already closed |

## 9. Suspension, stop, sleep (topics 6, 10) — T12 verification

- **Pump run:** `ble_suspend` (application) → security `on_suspend` (encrypted
  EV_SUSPENDING_FOR_DOSE, best effort; queued before the close and drained first) → port
  `suspend()` (transport: window SUSPENDED, advertising stopped, link closed and confirmed or
  force-terminated within 1.5 s) → `conn_end` → `on_connection(false)` (Noise and cipher states
  freed and zeroized, cache cleared, authorization ended). While suspended: no advertising (not
  even from `wait()`), connections refused, `send_frame` refused, writes ignored — no BLE
  communication. The **controller stays enabled** (no RF activity), so `dsp_esp_rng_ready()` stays
  true; no randomness is drawn while suspended (session IDs are drawn only when a connection
  opens, which needs `resume()` and a new link; a reconnect needs a new IK handshake, C7).
- **Stop:** controller disabled at the end of the window (`deinit`), before Wi-Fi or deep sleep;
  `is_off()` = transport deinitialized and controller IDLE; after it `dsp_esp_rng_ready()` is false
  and no handshake can start (fail closed, R3).
- **T7 bring-up:** `CONFIG_DSP_BLE_BRINGUP_POWER_ON_WINDOW` sets `cfg.service_on_power_on`; the
  Application Service then treats a power-on wake as a presence wake for the service window and
  for the security layer's session precondition (one helper, `presence_wake`). Authentication,
  authorization, the entropy prerequisite and the RNG are untouched; timer wakes never open a
  window.

## 10. Initialization order and the entropy prerequisite (topic 9)

1. Runtime start: `dsp_esp_rng_register()` before any libsodium/noise-c use; security layer
   registered; only then `dsp_esp_ble_port()` (no radio activity).
2. BLE stage: `start()` → `init()`: NimBLE port init (controller enabled) → GATT registered →
   host task → host sync (≤ 3 s, else full deinit and `START_FAILED`) → public address checked.
3. `publish_info` → advertising. First connection → `dsp_app_sec_connection_begin` →
   `dsp_esp_rng_ready()` true (controller enabled) → `sodium_init()` → hardware RNG.
4. Never: a handshake before `init()` or after `deinit()` (no controller → no entropy → HELLO
   session ID 0 → HS refused).

## 11. Unauthorized clients and monopolization (topic 8) — T1 approved

Single connection. Mitigations: fragment-error limits (application 3 before a handshake and 1
after; transport backstop 8, section 13.2), the 30 s
pre-authentication limit, close on a failed handshake, immediate re-advertising after every
disconnect, a second simultaneous link terminated at once, per-window counters
(`dsp_esp_ble_stats`: connections, refused links, disconnects, fragment errors, refused frames, TX
failures) for M1.10d.2 H9.

## 12. Resources (topic 12)

Measured on ESP32-C3 cross-builds (ESP-IDF v6.1, this tree vs a clean `c102343` worktree built
with the same script; slot 1,835,008 B; **budget gate 85 % = 1,559,757 B**). Static DRAM from
`idf.py size`. Target heap, stacks, current and timing remain open (H7, H8).

| Image | Image size | Slot | Static DRAM | vs `c102343` |
|---|---|---|---|---|
| default | 148,480 B | 8.1 % | 50,059 B | code and data identical (all loadable sections; only the app descriptor's build time and the image hash differ) |
| diagnostics (time + HTTPS) | 1,042,432 B | 56.8 % | 108,552 B | identical in the same sense |
| runtime (rt) | 251,008 B | 13.7 % | 87,479 B | +528 B / +16 B (T2 snapshot, T3, T7 in `dsp_app`) |
| runtime + connectivity (rtc) | 1,117,488 B | 60.9 % | 143,156 B | +528 B / +16 B |
| runtime + security (sec) | 437,424 B | 23.8 % | 96,085 B | +656 B / +16 B (also the `on_rx_fault` hook) |
| rtc + security (rtcsec) | 1,203,248 B | 65.6 % | 150,236 B | +656 B / +16 B |
| **sec + BLE transport (`sdkconfig.ble.defaults`)** | **680,816 B** | **37.1 %** | **136,021 B** | new (projection was ≈ 689 KB) |
| **rtc + sec + BLE** | **1,400,784 B** | **76.3 %** | **180,393 B** | new (projection ≈ 1.39 MB / 181 KB); 158,973 B (8.7 points) below the gate — watch item |
| sec + BLE + bring-up window | 680,944 B | 37.1 % | 136,021 B | +128 B over sec + BLE |

Attribution (image / static DRAM):

| Contribution | Image | DRAM | How |
|---|---|---|---|
| Noise + libsodium + PSA HKDF + dispatcher (M1.10c.2a) | +186,416 B | +8,606 B | sec − rt |
| **NimBLE controller + host + PHY/NVS/coexistence, plus transport and adapter** | **+243,392 B** | **+39,936 B** | (sec + BLE) − sec |
| of which transport + adapter code | ≈ 6.1 KB | 3,184 B | ELF symbols: transport functions + `dsp_esp_ble` text; `s` (transport state 2.6 KB incl. 3-frame TX ring, reassembly, semaphores, callout, fragment buffer) 3,124 B, `s_ble` 44 B, `s_identity` 16 B |
| NimBLE with Wi-Fi already linked (shared PHY/NVS) | +197,536 B | +30,157 B | (rtc + sec + BLE) − rtcsec |

Static stack (GCC `-fstack-usage`, target flags): every transport function ≤ 48 B per frame
(`pump`, `p_send_frame`, `dsp_ble_tr_on_write` 48 B); NimBLE callbacks `gap_event` → transport
(≤ 48 B) and `chr_access` 48 B + `dsp_ble_tr_on_write` 48 B + reassembly + `dsp_app_post_frame` (no
frame on the stack; the fragment buffer is static); adapter `r_adv_start` 384 B (application task:
advertising fields). Application side: `link_sync` 32 B, `rx_fault` 32 B, `stage_ble` 112 B — the
M1.10c.2a worst case (dispatch, 5,344 B) is unchanged. NimBLE host task stack: ESP-IDF default
(`CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE`), high-water mark is H7.

## 13. Decisions

| # | Decision | Status |
|---|---|---|
| T1 | Single connection; fast re-advertising; contention counters | approved |
| T2 | Authoritative connection snapshot beside the queue, strictly increasing generations, reconcile before every item; generation-bound `send_frame`/`disconnect` | approved in principle; implemented as required (section 2.2) |
| T3 | Fragment errors reported to the application; plain MALFORMED (type 0, txn 0) only before a handshake started, else close without reply | approved; protocol section 3 amended |
| T3b | No plain EV_BUSY: a refused frame is dropped silently | approved (review of M1.10d.1) |
| T4 | 100 ms for 30 s after start/disconnect, then 500 ms | approved |
| T5 | Asynchronous retries: 20 ms callout, ≤ 25 consecutive refusals, 1000 ms per frame; close + release on failure | approved (values tunable in M1.10d.2) |
| T6 | `CONFIG_DSP_BLE_TRANSPORT` build-only, requires the security layer, refused with production | approved |
| T7 | Power-on service window for bring-up (`service_on_power_on`) | approved |
| T8 | NimBLE: peripheral + broadcaster, no GATT client, no SM, 1 connection, MTU 247 preferred, logs off, legacy advertising | approved |
| T9 | C10 supersedes "three failed handshakes stop advertising" | approved |
| T10 | Write with response only; long/prepared writes refused; notifications only | approved |
| T11 | Public address; tracking residual risk recorded (section 16); never an identity | approved |
| T12 | Controller enabled while suspended, communication fully suspended; disabled at `stop()` | approved (section 9) |
| T13 | Final reply before a requested close: terminate after the ring drained + 100 ms grace (bounded by the frame deadline); **best effort only** | approved as best effort; semantics and evidence in section 13.1 — **please confirm** |
| T14 | TX ring 3 frames; suspend/stop wait 1.5 s; transport fragment-error backstop 8 (the application's limit is the effective one) | ring and bounded wait approved; counters and the wait's safety clarified in section 13.2 — **please confirm** |
| — | Generation-bound `send_frame(gen)` / `disconnect(gen)` | approved |
| — | Normative GATT specification (`BLE_PROTOCOL.md` section 2) | approved |

### 13.1 T13 — send-before-close semantics (best effort)

The 100 ms grace lets a reply that was already queued leave the controller before the link is
terminated. BLE notifications have no ATT-level acknowledgement, so it is **not** a delivery
guarantee, and nothing depends on it.

| Condition (review) | How it holds | Evidence |
|---|---|---|
| Submission to NimBLE is never treated as delivery | `notify` → `OK` means "queued to the stack" (`DSP_BLE_NOTIFY_OK`); `tx_frames` counts submissions. The close proceeds identically whether the phone received the reply or not. The Application Service has no delivery notion at all: `send_frame` returns "accepted", and `conn_end` runs as soon as `disconnect(gen)` returns, without waiting for any transmission | `test_close_grace_semantics` (a): a reply accepted by the stack but lost on the air → same terminate, `tx_frames` 1, phone received 0 |
| Lost END_SESSION / MALFORMED: safe, documented client behavior | Device outcome is independent of delivery: the window ended (owner's END_SESSION) or the connection closed, every security state zeroized, the next connection starts from zero (gate counter, fragment counters, session ID). Client rules: `BLE_PROTOCOL.md` 11.3 (no inference from a missing final reply; END_SESSION not repeated after a disconnect; HELLO again after a pre-handshake MALFORMED; durable state read back) | `test_blesec`/`test_ble_e2e` `test_lost_final_replies`: END_SESSION reply lost → `ble_end` COMMAND, zeroized; third MALFORMED lost → closed, next connection answers HELLO, no error carried over |
| Closing is bounded by existing deadlines | `disconnect(gen)` sets `closing` at once (no new sends accepted), so only frames queued before it remain, each with its absolute deadline (`send_frame` + 1000 ms, T5) and the 25-refusal limit. Upper bound from the close request: **≤ 1000 ms drain + 100 ms grace**, then terminate; a frame that cannot be sent fails at its deadline and terminates **without** grace. The application has already ended the connection (`conn_end`), so no service-session deadline (idle 180 s, maximum 900 s, unauthenticated 30 s) waits for it. `static_assert`: the suspend/stop wait (1500 ms) exceeds drain + grace | `test_close_grace_semantics` (c): stack never accepts → terminate at ≤ deadline + one retry tick, ring released; `test_close_after_final_reply` |
| No application or NimBLE task blocks for delivery | the grace and every retry are the one-shot host-task callout (`retry_arm`); `send_frame`, `disconnect` and all `dsp_ble_tr_on_*` callbacks return without waiting. The only wait in the transport is suspend/stop's bounded wait for the **GAP disconnect** (section 13.2), not for notification delivery | `test_close_grace_semantics` (a)+(b): no `wait` across `send_frame`, `disconnect` and the callout; mutation "close waits" caught |
| Disconnect during the grace releases buffers and invalidates the generation | a GAP disconnect clears the record (`conn_clear`: ring flushed, `tx_epoch` advanced, `gen` 0, grace state reset) and reports DISCONNECTED(gen); `send_frame`/`disconnect` of that generation are refused; the late callout finds no closing link and does nothing to a newer link on the same handle | `test_close_grace_semantics` (d) during the grace and (d′) during a busy drain, each followed by a same-handle reconnect and the stale callout: no terminate, no notification, no re-arm; mutations "grace ignores `closing`" and "no flush on clear" caught |

### 13.2 T14 — fragment-error counters and the suspension wait

**Two counters, one effective limit.** They do not count the same failures:

| Counter | Counts | Threshold and action | Reset |
|---|---|---|---|
| Application gate (`dsp_link_t.errors`, `DSP_LINK_ERRORS_MAX` 3) | every pre-authentication **gate error answered with a plain error**: fragment errors reported by the transport, malformed or unparsable frames, unauthenticated requests, HELLO/HS order errors | third error: plain error reply, then close (`link_drop`). After the first handshake message was accepted (and after authentication, during a recovery confirmation) the security layer closes on the **first** fragment error without any reply (T3); decryption failures close at once as before | `dsp_link_open` at every connection opened by the application (new generation) |
| Transport backstop (`rx_faults`, `DSP_BLE_TR_RX_FAULT_LIMIT` 8) | **reassembly failures only** (no FIRST, index gap, duplicate, empty fragment, > 520 B, > 2 s gap), counted on the host task as they arrive, whether or not the application task has run | eighth: immediate terminate (no reply) | `conn_clear` at every GAP connect and disconnect; reassembly reset per generation |
| Application snapshot (`dsp_app_link_t.faults`, `ss.faults_seen`) | the transport's reports for the current generation (delivery only, not a policy) | handled ≤ 3 per reconcile | at CONNECTED of a newer generation / `conn_open` |

**Precedence.** The application's limit is the effective policy: it closes at the third gate
error before a handshake and at the first fragment error after one. The transport's backstop is
larger by construction (`static_assert(DSP_BLE_TR_RX_FAULT_LIMIT > DSP_LINK_ERRORS_MAX)`), so it
**never closes a connection the application would keep open**: any connection with eight fragment
errors has already exceeded the application's limit. It acts only when the application task has
not yet processed earlier reports (it is busy, e.g. a flash write, or the snapshot saturates
during a flood); then the result is the same close, earlier, and some best-effort plain MALFORMED
replies are not sent. A single limit (only the application's) was considered and not chosen:
without the backstop a flood of bad fragments would be bounded only by the application task's
scheduling. Evidence: the end-to-end suite now injects real bad fragments through the transport
and asserts the backstop never fired (`TR.st.rx_fault_closes == 0`) while the application closed:
pre-handshake after three errors (three plain MALFORMED), during the handshake and after
authentication after one (no reply); `test_rx_faults` shows the backstop closing at eight when the
application never runs; mutation "backstop at 3" is rejected by the `static_assert` and, with the
assert removed, fails the end-to-end and transport tests.

**The 1.5 s suspend/stop wait and safety deadlines.** Order of a pump run:
`ble_suspend` → `on_suspend` (best-effort encrypted EV_SUSPENDING_FOR_DOSE) → `suspend()`
(window SUSPENDED and `closing` at once: from here on no write is delivered, no frame is sent and
no connection is accepted; then the bounded wait for the GAP disconnect, ≤ 1500 ms, after which
the transport forces the terminate, clears the record and reports DISCONNECTED itself) →
`conn_end` (security session zeroized, authorization ended) → `ble_active = false` → pump.

- **No pump run with an active security session.** `conn_end` runs unconditionally after
  `suspend()` returns, and `suspend()` always returns within the bound; the pump runs only after
  it, on the same task. While the application waits, it processes no frame, so the session cannot
  authorize anything. A pending CONNECTED cannot open a connection afterwards (`ble_active` false,
  window suspended). Evidence: a probe inside the pump run (`probe_no_session_during_pump`)
  asserts no open connection, `ble_active` false, transport not connected and all security state
  zeroized, both for a timely GAP disconnect and — new e2e test
  `test_suspend_without_gap_disconnect` — when the controller never reports it (synthesized
  disconnect after the wait). Mutation "no `conn_end` in `ble_suspend`" is caught by this probe.
- **No safety deadline is violated.** The wait precedes the pump; it never extends a running
  pump (pump runtime limits are unchanged and enforced in the pump stage). Scheduled doses have a
  600 s due window (`DSP_EXEC_DUE_WINDOW_DEFAULT_S`); a manual dose's decision is durable before
  `ble_suspend`. The service session's maximum and idle deadlines are re-checked in `ble_resume`
  after the pump; at most the 1.5 s (plus the pump run) elapses before that check, as before
  M1.10d. At `stop()` the window may end up to 1.5 s after its deadline; nothing executes meanwhile.
  The wait yields the task (semaphore with timeout), well under the task watchdog; target timing is
  H-item H4/H8.

## 14. Failure handling

| Failure | Handling | Security effect |
|---|---|---|
| NimBLE init or host sync fails | full deinit, `start()` false → `START_FAILED` | none |
| Fragment error | report; app: plain MALFORMED pre-handshake (3rd gate error closes) or close on the first; transport backstop closes at 8 if the app has not acted (13.2) | session zeroized on close |
| Queue full / no window | frame dropped silently | request not executed; client retries |
| Notification pool exhausted | asynchronous retry; 25 refusals or 1000 ms → ring flushed, terminate | session closed |
| Stack error on notify | frame failed, ring flushed, terminate | session closed |
| Unsubscribed client | `send_frame` false | client times out |
| Disconnect during RX / TX | partial frame never continues (per-generation reset) / ring released, stale completions harmless | session closed |
| Stale `send_frame`/`disconnect` generation | refused / ignored | — |
| Connection while suspended/stopping, second link | terminated at once, not reported | — |
| GAP disconnect missing after suspend/stop | forced terminate after 1.5 s; DISCONNECTED reported by the transport | session closed before the pump |
| Advertising start failure | retried at the next `wait()`; `resume()` false ends the window | — |
| Controller not idle at the end | `is_off` false → existing shutdown checklist (fail-safe) | — |

## 15. Verification

### 15.1 Host (M1.10d.1) — results

All on Linux (GCC, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`, ASan + UBSan):
**31/31 CTest tests pass.**

| Test | Checks | Covers |
|---|---|---|
| `test_ble_transport` | 47,346 | fragmentation at every MTU 23–247 (+517 clamped) × boundary lengths (> 2,000 frame round trips), MTU change between frames, every fragment fault, 2 s gap ±1 ms, transport fault limit, 200,000 fuzzed fragments, queue refusal without reply, asynchronous retries / retry limit / deadline / stack error / ring capacity / unsubscribed, disconnect during RX/TX/inside notify (with and without a same-handle reconnect), stale completions/callouts/generations, advertising only from the app task, fast→slow, payload restart, refused links, T12 suspension (no send, write, advertising), suspend with/without GAP disconnect, close after the final reply, **T13** (lost-on-air reply, no waiting, bounded close with a stalled stack, disconnect during the grace and during a busy drain followed by a same-handle reconnect and the stale callout), stop/is_off, **two threads** (simulated host task, one reused handle, ~450–500 links per run: 0 cross-link fragments) |
| `test_ble_transport_symbols` | — | transport object references only the 3 producers, the codec and `mem*` (a mutant calling `dsp_app_end_service_session` is rejected) |
| `test_ble_e2e` ×3 | 1,222 each | the whole noise-c phone suite through the transport at MTU 23 (1,035 writes / 669 notifications), 64, 247; fragment faults injected as real bad fragments through the transport (backstop never fired); suspension with a missing GAP disconnect (pump-time probe) |
| `test_session` | 499 | T2 storms (DISC+CONN, DISC+CONN+DISC, stale producer, 1000 cycles) with the app not running between callbacks; saturation; pre-auth fragment faults; T7 window option |
| `test_blesec` | 1,152 | + T3 policy: pre-auth plain MALFORMED (1 and 3 errors, the third closes); XX handshake and IK session closed without any reply; T13 lost final replies (END_SESSION, third MALFORMED); pump-time probe: no connection or security session while the pump runs |
| others | unchanged | `test_app`, `test_app_ops`, `test_owner`, `test_manual`, `test_link`, `test_platform` (producer now waits for the reply before reconnecting, as a phone does) … |

- **ThreadSanitizer** (separate build, `-fsanitize=thread`, ASLR off for the TSan runtime):
  `test_ble_transport` and `test_platform` — 0 reports.
- **Mutation checks** on the transport (each mutant must fail the tests): no generation check in
  `send_frame` (5 failures), advertising restarted from the host task (1), no ring flush at
  disconnect (5), no reassembly reset per generation (2), unbounded retries (1), `disconnect`
  dropping the final reply (1), no synthesized disconnect (2), no epoch check after `notify` (2,
  after adding the same-handle reconnect case). The slot-generation check inside the pump is
  unreachable defense in depth (the ring is flushed at every change) and survives, as expected.
  T13/T14 additions: backstop at 3 (rejected at compile time; with the assert removed 4 transport
  + 4 e2e failures), grace callout ignoring `closing` (2 transport + 102 e2e failures), no ring
  flush on disconnect (8), `disconnect` waiting (2), `ble_suspend` without `conn_end` (7 + 14 via
  the pump-time probe).
- **Cross-builds:** all nine configurations above, exit 0, 0 compiler warnings (the 5 ESP-IDF
  CMake component-validation notices are present in the baseline too). The production guard
  refuses `sdkconfig.ble.defaults` + bring-up + `CONFIG_DSP_APP_ALLOW_PRODUCTION` at configure time
  (the M1.10c.2a security guard fires first; the new transport/bring-up guard is behind it).
- **Not evidence for:** radio behavior, Android interoperability (M1.10e), the target RNG
  (M1.10c.2b), heap, stack high-water marks, timing, current (M1.10d.2).

### 15.2 Hardware (M1.10d.2, OPEN)

`firmware/HARDWARE_ACCEPTANCE.md` 8b H1–H10 (advertising, GATT/MTU, handshakes over BLE,
lifecycle including reply-before-close, notifications, RNG with the radio (8a R1–R5), target heap
and stacks, current and teardown, contention, stack thread-safety). Neither host tests nor
cross-builds are evidence for these items or for M1.10c.2b.

## 16. Residual risks

- **Device tracking (T11).** The public address is stable and advertised during service windows;
  a nearby observer can recognize the device across windows. Accepted for v1 (short,
  button-initiated windows; the address is never an identity). Revisit with privacy (RPA) if
  windows become frequent.
- **Connection monopolization (T1).** A nearby attacker can repeatedly occupy the single
  connection during a window and delay the owner; measured in H9.
- **Stack boundary.** The stale-delivery guarantee relies on invariant 5 and on NimBLE processing
  events in its host task; verified by design and host tests, confirmed on hardware in H10/H4.
- **Hardware unknowns.** Heap after NimBLE init, stack high-water marks, notification throughput,
  reply-before-close timing (grace 100 ms), current — all H-items.
- **Final replies are best effort (T13).** A final reply before a device-initiated close can be
  lost; the device's outcome does not depend on it and the client rules are in `BLE_PROTOCOL.md`
  11.3. The Android client's conformance to those rules is part of M1.10e.
