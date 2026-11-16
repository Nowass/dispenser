# M1.8 notification integration test (local, no hardware)

End-to-end verification of the notification pipeline with the **real firmware domain code**
and the **committed Cloudflare Worker**, without hardware, production services, real Gmail or
real credentials.

```text
notify_harness (C, host)                                   notify_e2e.mjs (Node)
  M1.5 engine + M1.3 storage (fake KV, power cuts)          ├─ fault-injection proxy (TLS)
  M1.7 protocol, HTTPS transport, connectivity session      │    pass / hang / drop / rewrite /
  posix_https_port.c: real TLS (OpenSSL), test CA,  ──TLS──►│    corrupt / delay / status ...
  hostname check, timeouts                                  ├─ wrangler dev --local (the Worker)
                                                            │    + throwaway local D1
                                                            └─ fake Gmail API ◄─ scheduled run
```

| Boundary | Real in this test | Simulated |
|---|---|---|
| Notification engine, storage codec, protocol encoder/classifier, HTTPS transport, connectivity session | yes (same C sources as the firmware) | — |
| Flash | — | fake KV with power-cut injection (NVS-like "durable on set" and staged modes) |
| TLS | yes, OpenSSL with certificate-chain and host-name verification against a throwaway test CA | not mbedTLS / `esp_http_client` (the ESP-IDF adapter is verified on hardware only) |
| HTTP API, authentication, validation, idempotency, D1 writes | yes, the committed Worker under `wrangler dev --local` | — |
| Scheduled e-mail delivery | yes (`/__scheduled`), Worker code unchanged | Gmail and OAuth endpoints are a local fake |
| Wi-Fi, SNTP, clock | — | fakes inside the harness |
| Network faults | DNS, refused connection, untrusted certificate, wrong host name, TLS black hole, resets, hangs | 429/5xx and malformed acknowledgements are injected by the proxy |

## Run (one command)

```sh
firmware/test/integration/run.sh                  # full suite, 1,000 stress sessions (about 1.5 min)
firmware/test/integration/run.sh --sessions 100   # quicker
```

**Prerequisites:** Node.js ≥ 22.12 (for `wrangler` and `node:sqlite`), the `openssl` command and
OpenSSL development files, CMake ≥ 3.22 and a C11 compiler, and `npm ci` in
`cloudflare/dispenser-notify` (the suite uses that package's `wrangler`). `run.sh` checks these
first and exits with code 2 if one is missing.

**What it does:** builds `notify_harness` with ASan/UBSan in a temporary directory, then runs
`notify_e2e.mjs`, which creates a second temporary directory for throwaway certificates, the
local D1 database and a random synthetic `DEVICE_API_TOKEN`, and starts `wrangler dev --local`,
the fault proxy and the fake Gmail on 127.0.0.1 only. **No Cloudflare account, production D1,
Google endpoint or real credential is used.**

**Cleanup and exit codes:** every child process (`wrangler` and its `workerd` processes run in
their own process group; the harness) and both temporary directories are removed at the end,
also on a failed check, an error, Ctrl-C/SIGTERM (exit 130) or the 30-minute watchdog. Exit
code 0 means every check passed; anything else is a failure (1 = a check or the run failed,
2 = missing prerequisite or bad argument). The driver can also be run directly:
`node test/integration/notify_e2e.mjs <path to notify_harness> [--sessions N]`.

## Scenarios (each check prints only on failure; the summary counts all checks)

1. **Basic delivery:** one event → strict 201 with the exact body → D1 row with the
   canonical payload and its SHA-256 → scheduled run → fake Gmail → `SENT`.
2. **Idempotency:** response lost after the Worker stored the event (dropped connection,
   hanging response) → `UNCERTAIN` → retry with the same ID and identical canonical payload
   (only `attempt` differs) → 200 duplicate → exactly one D1 row and one e-mail, no resend
   after `SENT`.
3. **Network failures and recovery:** Wi-Fi down, DNS failure, connection refused, untrusted
   certificate, wrong host name, TLS black hole (timeout before transmission), reset after
   the handshake, hang and close after a complete request, 429, 503, 500; nothing reaches
   the Worker, the queue and payloads stay unchanged, recovery delivers.
4. **Permanent rejection:** a 400 `invalid_field` produced by the real Worker and a genuine
   409 (storage reset with a colliding epoch) → `REJECTED`, logged atomically with the queue
   removal, later events continue, the log survives a restart, the stored event is never
   modified.
5. **Systemic failures:** 401, 404, 405, 413, 415 and an ambiguous 400 (all produced by the
   real Worker), a mismatched 409, mismatched and invalid 2xx acknowledgements → six
   sessions each never drain the queue, the persistent diagnostic is set and survives a
   restart, everything is delivered after the fix.
6. **Power loss** before/during the freeze write, during transmission, after the Worker
   accepted but before the ACK removal was saved, and during the rejection-log write (both
   flash modes): stable IDs, identical payloads, never lost, never both queued and logged.
7. **Session orchestration:** WARNING waits, CRITICAL on an existing wake (WARNING rides
   along, CRITICAL first), the hourly limit, the Wi-Fi backoff, dosing/permit/BLE and
   next-dose guards (no Wi-Fi, no network), the trusted-time requirement, the session budget
   with a real monotonic clock (the rest stays queued and untouched), Wi-Fi always off.
8. **Stress:** 1,000 randomized sessions (500 per flash mode) with mixed faults, power cuts
   and restarts; then: queue drained, no duplicate D1 row, every event either in D1 or
   durably rejected (rejection count matches the persistent log total), no 409, identical
   payload on every retry, D1 payload = sent payload, every row e-mailed exactly once with a
   unique Message-ID, and the run must really have executed all sessions (never vacuous).
   **Queue-overflow accounting** (approved M1.5 policy): an event is dropped only when 16
   CRITICAL entries are queued (checked at every drop: nothing of lower severity was left to
   evict, CRITICAL is never evicted); it gets no ID, is counted persistently and reported
   later in a `DEVICE_ERROR/QUEUE_OVERFLOW` event whose values must sum to the number of
   drops, with the persistent counter back at 0. **No silent loss:** a queued entry may only
   disappear after an ACK or REJECTED was observed for its ID (checked after every command,
   including restarts). The run prints the final accounting per flash mode.
