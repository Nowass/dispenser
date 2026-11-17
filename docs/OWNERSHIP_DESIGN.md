# Dispenser V2 — device ownership and credential persistence (M1.10b.3, revision 2)

Status: **approved (2026-11-17, R1–R5 accepted) and implemented in the firmware domain
(M1.10b.3, host-tested; committed after the final review).** Implementation notes and
measured resources: section 16. Revision 1 was reviewed on 2026-11-17:
O1, O2, O4–O11 approved with conditions; **O3 changed** (credential corruption stops automatic
dosing). This revision incorporates the decisions and the review's requirements for RTC state,
factory reset and admission control (section 14 lists every change).

Baseline: `docs/BLE_PROTOCOL_DESIGN.md` revision 3 (approved: D1 setup code, D2 Noise XXpsk3/IK,
D3 no pairing, D5 four OWNER slots and invites, D6 recovery procedure, D8 manual-dose anti-replay
as implemented in M1.10b.2 `aa0532f`) and `docs/BLE_PROTOCOL.md` v1.0.

Scope of M1.10b.3: the credential record, storage layout v5 and its migration, the ownership
state machines and their Application Service operations, the RTC-retained short-lived state,
manual-dose admission control, the interface the M1.10c security layer will use, and developer
provisioning of the setup code. **Not in scope:** Noise (M1.10c), NimBLE and the button driver
(M1.10d), Android (M1.10e), the Wi-Fi store and factory reset (M1.10b.4, semantics constrained in
section 10), eFuses, flash encryption, hardware, production services.

---

## 1. Principles

1. **The domain never does cryptography.** Key generation, X25519, HKDF and the handshakes belong
   to the pinned noise-c/libsodium in the M1.10c security layer (`dsp_blesec`). The domain stores
   and validates key bytes, decides ownership transitions and enforces their preconditions. No
   custom cryptographic protocol, no primitive code of our own.
2. **Every ownership change is one atomic record write** (`auth`), except recovery, which first
   writes the restricting change (readiness SERVICE_REQUIRED, `dosing`) and then `auth`.
3. **Three distinct credential states, never confused:** a valid UNCLAIMED record, a valid
   CLAIMED record, and CRED_DEGRADED (missing, corrupt, unsupported or faulted credentials on an
   initialized device). CRED_DEGRADED is never treated as unclaimed and **stops all dosing** until
   recovery or maintenance (O3 as changed).
4. **RTC state is a precondition, never authority.** Invites and the recovery arm live in RTC
   memory (O1). Their presence alone authorizes nothing: every ownership commit additionally needs
   a completed Noise handshake with the setup code (plus the invite token, or the 3-press
   confirmation), and the RTC entry must be bound to the current durable ownership epoch.
5. **Ownership never touches dosing state** except recovery forcing SERVICE_REQUIRED. History,
   schedules, configuration, the manual-dose high-water marks, the time record, the BLOCKED latch,
   refill intent and notifications are byte-identical before and after every ownership operation.
6. **Secrets are never logged, printed, returned over BLE or committed.** RAM copies of the
   device private key are wiped after use.

---

## 2. Persistent identity and owner records (review item 1)

### 2.1 What is stored where

| Item | Where | Lifetime | Secret? |
|---|---|---|---|
| Device ID (factory MAC, already used in notification IDs) | eFuse (read-only, existing) | device | no |
| Setup code (128 bit) | **separately**: `nvs_sec` partition, namespace `dsp_sec`, key `setup_code` (16-byte blob), read-only for the firmware | label | **yes** |
| Device identity key pair (X25519) | `auth` record (`nvs` / `dispenser`) | claim → next recovery / factory reset | private key **yes** |
| Phone slots (4 × public key, name, times), ownership epoch, counters | `auth` record | as above | no |
| Invite (token, creator, epoch, deadline) | RTC-retained block | ≤ 10 min, deep sleep only | token **yes** (PSK input) |
| Recovery arm (epoch, deadline, aborts) | RTC-retained block | ≤ 30 min, deep sleep only | no |
| Manual-dose high-water marks, result ring | `dosing` record (M1.10b.2, unchanged) | device | no |
| Manual-dose admission counter and date | `dosing` record format 3 (section 11) | device | no |
| Session keys, handshake state, pending identity | RAM of `dsp_blesec` (M1.10c) | session | **yes** |

### 2.2 Credential record `auth` (format 1, 416 B, record type 8)

All little-endian, explicit sizes, zero padding, decoded strictly like every other record.

```text
0        type (8)
1        format (1)
2        claim_state: 0 UNCLAIMED, 1 CLAIMED
3        flags: bit 0 KEY_FAULT (identity key pair found inconsistent); other bits zero
4..7     owner_epoch u32: +1 at every ownership change (claim, add, remove, recovery, key fault,
         future factory reset); never decreases; RTC invites and arms are bound to it
8..39    identity private key (32)       zero when UNCLAIMED
40..71   identity public key (32)        zero when UNCLAIMED
72..79   claimed_at i64                  current ownership began (claim or recovery); 0 = clock untrusted
80..87   recovered_at i64                last recovery; 0 = never / clock untrusted
88..91   recoveries u32                  saturating
92..95   failed_sessions u32             sessions with >= 1 failed handshake; saturating, diagnostic
96..415  4 x slot (80 B):
         0       used (0/1)
         1       name_len (0..24)
         2..3    zero
         4..35   phone static public key (32)
         36..59  name, UTF-8 (dsp_text rule: well formed, no control characters), zero padded
         60..63  zero
         64..71  enrolled_at i64 (0 = clock untrusted)
         72..79  last_seen i64   (0 = never / clock untrusted)
```

**Validity (decoder rejects otherwise → CRED_DEGRADED):**
- UNCLAIMED: identity, flags and every slot all zero (epoch and counters may be non-zero).
- CLAIMED: identity private and public key non-zero; at least one used slot.
- Used slot: public key non-zero, not equal to the identity public key, distinct from every other
  used slot's key; name valid; instants 0 or in the supported range. Unused slot: all zero.
- KEY_FAULT set (only valid with CLAIMED) → the record decodes but the state is CRED_DEGRADED.

**Provisioning value:** UNCLAIMED, epoch 0, all zero except type and format.
**NVS cost:** 2 + 13 = 15 entries (live data 309 → 324 of about 2,500).

The setup code is **not** in this record and nothing derived from it is stored anywhere.

### 2.3 Domain module `dsp_auth` (pure C11, no crypto, no I/O)

```c
#define DSP_AUTH_SLOTS 4          /* == DSP_APP_PHONE_SLOTS == DSP_MANUAL_SLOTS */
#define DSP_AUTH_KEY_BYTES 32
#define DSP_AUTH_NAME_MAX 24
#define DSP_AUTH_RECORD_BYTES 416

typedef struct {
    bool used;
    uint8_t name_len;
    uint8_t pub[DSP_AUTH_KEY_BYTES];
    char name[DSP_AUTH_NAME_MAX];
    dsp_utc_t enrolled_at, last_seen;
} dsp_auth_slot_t;

typedef struct {
    uint8_t claim_state, flags;
    uint32_t owner_epoch;
    uint8_t id_priv[DSP_AUTH_KEY_BYTES];
    uint8_t id_pub[DSP_AUTH_KEY_BYTES];
    dsp_utc_t claimed_at, recovered_at;
    uint32_t recoveries, failed_sessions;
    dsp_auth_slot_t slot[DSP_AUTH_SLOTS];
} dsp_auth_t;

bool dsp_auth_is_valid(const dsp_auth_t *a);
void dsp_auth_provisioned(dsp_auth_t *a);
int  dsp_auth_find(const dsp_auth_t *a, const uint8_t pub[32]);   /* slot or -1 */
int  dsp_auth_free_slot(const dsp_auth_t *a);                     /* lowest free or -1 */
uint8_t dsp_auth_used_count(const dsp_auth_t *a);

/* Pure transitions on a copy; *a unchanged on refusal; every ownership transition increments
 * owner_epoch (saturation at UINT32_MAX refuses further changes: unreachable in practice). */
dsp_auth_result_t dsp_auth_claim(dsp_auth_t *a, const uint8_t id_priv[32], const uint8_t id_pub[32],
                                 const uint8_t phone_pub[32], const char *name, size_t name_len,
                                 dsp_utc_t now_or_0);                      /* -> slot 0 */
dsp_auth_result_t dsp_auth_add(dsp_auth_t *a, const uint8_t phone_pub[32], const char *name,
                               size_t name_len, dsp_utc_t now_or_0, uint8_t *slot_out);
dsp_auth_result_t dsp_auth_remove(dsp_auth_t *a, uint8_t slot);   /* refuses the last used slot */
dsp_auth_result_t dsp_auth_recover(dsp_auth_t *a, const uint8_t id_priv[32], const uint8_t id_pub[32],
                                   const uint8_t phone_pub[32], const char *name, size_t name_len,
                                   dsp_utc_t now_or_0);   /* from any state incl. degraded: new record */
dsp_auth_result_t dsp_auth_mark_key_fault(dsp_auth_t *a);
bool dsp_auth_touch(dsp_auth_t *a, uint8_t slot, dsp_utc_t now);  /* true = write due (O8) */
void dsp_auth_wipe(dsp_auth_t *a);                                 /* volatile zeroization */

dsp_codec_result_t dsp_codec_encode_auth(const dsp_auth_t *a, uint8_t *out, size_t cap, size_t *len);
dsp_codec_result_t dsp_codec_decode_auth(const uint8_t *buf, size_t len, dsp_auth_t *out);
```

Results: `OK, INVALID, STATE, SLOTS_FULL, DUPLICATE_KEY, NOT_FOUND, LAST_SLOT`. A re-used slot
inherits `manual_hw[slot]`; the new phone reads `manual_floor`, so anti-replay holds across slot
reuse, removal and recovery.

---

## 3. Credential states, storage layout v5 and migration (review items 2, 8)

### 3.1 The three states

| State | Condition (at open, or set at runtime) | Dosing | Ownership | HELLO / Info |
|---|---|---|---|---|
| **UNCLAIMED** | `auth` valid, claim_state 0 | normal gates (a new device is SERVICE_REQUIRED with no limits, so nothing runs) | CLAIM only | claim_state UNCLAIMED |
| **CLAIMED** | `auth` valid, claim_state 1, no KEY_FAULT | normal gates | SESSION, ADD, owner operations, RECOVERY | CLAIMED |
| **CRED_DEGRADED** | v5 marker and `auth` missing, malformed or of a newer format; or KEY_FAULT set; or a key mismatch reported in this wake | **blocked**: new gate `DSP_DOSING_G_CREDENTIALS` refuses automatic and manual dosing; DEVICE_ERROR (storage) condition raised | **RECOVERY only** | claim_state CLAIMED + flag CRED_DEGRADED (O9); never UNCLAIMED |

CRED_DEGRADED ends only by a successful recovery (a new valid record; the device is then
SERVICE_REQUIRED, so the owner must review and set READY) or by maintenance. The other records are
still loaded (that is what lets recovery force SERVICE_REQUIRED durably); if any of them is also
unusable, the global storage failure of today applies (nothing loaded, read-only session) and
recovery is refused (DEGRADED): maintenance.

"Initialized device" means a layout marker exists. Only a first boot (no marker and every key
absent or at its provisioning value) or the v4 → v5 migration ever writes an UNCLAIMED record.

### 3.2 Layout v5

Layout v5 = layout v4 + key `auth`. The marker keeps its format (version field 5).
`DSP_STORAGE_LAYOUT_VERSION` becomes 5. `dosing` format 3 (section 11) is a lazy record upgrade
inside v5, like format 2 was inside v4.

### 3.3 Open (explicit presence, extended)

| Found at open | Result |
|---|---|
| v5 marker, every key valid | OK; credential state UNCLAIMED or CLAIMED (or CRED_DEGRADED by KEY_FAULT) |
| v5 marker, other keys valid, `auth` missing / malformed / newer | OK with `auth_status` CORRUPT / UNSUPPORTED → **CRED_DEGRADED** (dosing blocked) |
| v5 marker, any other key missing or malformed | CORRUPT as today (nothing loaded) |
| v4 marker, v4 keys valid, `auth` absent or at its provisioning value | migration (3.4) if writes are allowed, else `MIGRATION_REQUIRED` |
| v4 marker, `auth` with any other content | CORRUPT, nothing modified |
| v1/v2/v3 marker | as today, plus `auth` provisioned before the marker, straight to v5 |
| no marker, every key absent or at its provisioning value | first boot / interrupted provisioning: missing keys written (incl. UNCLAIMED `auth`), marker last |
| no marker, any key with real data (incl. any non-provisioning `auth`) | CORRUPT |

Residual: a v5 device would be re-provisioned UNCLAIMED only if its marker reverted to a valid v4
value **and** `auth` vanished; NVS replaces a key atomically (old versions erased after the new
one is complete, patched `nvs_flash`), so a marker cannot revert to an older valid value.

### 3.4 Migration v4 → v5 and power loss

1. Validate the v4 data exactly as today (any problem → CORRUPT, nothing written).
2. `auth` absent → write the provisioning value (UNCLAIMED, epoch 0), read back, compare;
   present at the provisioning value (earlier interrupted migration) → keep.
3. Write the layout marker v5 **last**.

| Cut | State after reboot | Effect |
|---|---|---|
| before or during step 2 | v4 marker, `auth` absent or provisioned (per-key atomic) | step 2 repeats idempotently |
| after step 2, before or during step 3 | v4 marker, provisioned `auth` | step 2 keeps it; step 3 repeats |
| after step 3 | v5 | done |

`NO_SPACE` during migration leaves v4 and the open fails as any storage failure does today (no
automatic dosing). A v4 device has never been claimable, so UNCLAIMED is its true state, not a
reinterpretation of lost data. Rollback (R3): older firmware reports layout v5 and `dosing`
format 3 as UNSUPPORTED and fails closed; an OTA must never roll back across them.

### 3.5 Storage API

```c
dsp_storage_status_t dsp_storage_load_auth(dsp_storage_t *st, dsp_auth_t *out); /* caller wipes */
dsp_storage_status_t dsp_storage_save_auth(dsp_storage_t *st, const dsp_auth_t *a); /* write, read back, compare */
dsp_storage_status_t dsp_storage_auth_status(const dsp_storage_t *st);          /* from open */
```

The static `buf` and `readback` of `dsp_storage_t` are reused and wiped after every `auth`
load/save; a `dsp_auth_t` scratch (≈ 420 B) in `dsp_storage_t` holds transition copies.

---

## 4. RTC-retained short-lived state (O1 with the review's requirements)

### 4.1 Contents (`dsp_power_retained_t` format 2)

```c
typedef struct {
    uint8_t valid;            /* 0/1 */
    uint8_t creator_slot;
    uint32_t owner_epoch;     /* auth owner_epoch when created */
    uint8_t token[16];        /* device RNG inside the creator's BLE session (RF on) */
    dsp_utc_t created_raw, deadline_raw;   /* deadline = created + 600 s, never recomputed */
} dsp_invite_retained_t;

typedef struct {
    uint8_t armed;            /* 0/1 */
    uint8_t aborts;           /* failed confirmations in this arm (0..2) */
    uint32_t owner_epoch;     /* auth owner_epoch at arming (UINT32_MAX = CRED_DEGRADED) */
    dsp_utc_t armed_raw, deadline_raw;     /* deadline = armed + 1800 s, never recomputed */
} dsp_recovery_retained_t;

/* plus, for every block: */
dsp_utc_t saved_raw;          /* raw time of the last save (monotonicity check) */
```

The block grows from 48 to ≈ 112 B (RTC slow memory: 8 KB, 192 B used). It is saved **immediately**
whenever an overlay is created, consumed or cleared (not only at shutdown).

### 4.2 Validity rules (all must hold, checked at every wake and before every use)

1. **Reset reason.** The overlays survive **only deep-sleep wakes** (TIMER, BUTTON). Every reset
   (power-on, brownout, software including the fail-safe reset, watchdog, panic) clears them, even
   though the rest of the retained block survives soft resets as today. Repeated resets therefore
   cannot keep an invite or arm alive.
2. **Integrity.** CRC and format must match, and the block's own fields must be self-consistent:
   `deadline - created` exactly 600 s (invite) or 1800 s (arm), `valid`/`armed` 0 or 1, `aborts`
   ≤ 2. Any violation clears both overlays. **CRC is only a torn-write check, not authentication.**
3. **Clock continuity.** `now_raw >= saved_raw` and `now_raw >= created/armed_raw`; any applied
   clock step in between (NTP or phone time, M1.4 rules) clears both overlays. Without trusted
   UTC the raw clock still runs through deep sleep; trust in UTC is not required, continuity is.
4. **Expiry.** Valid only while `now_raw < deadline_raw`. The deadline is fixed at creation, so
   neither deep sleep nor any wake extends it (and resets clear it anyway).
5. **Epoch binding.** Valid only while `owner_epoch` equals the durable `auth` epoch (arm in
   CRED_DEGRADED: the sentinel, while still degraded). Any ownership change (add, remove, recovery,
   key fault) therefore invalidates every older invite and arm, and a stale RTC copy can never
   authorize anything after the change it predates.

### 4.3 Single use

- **Invite consumption is ordered before the commit:** at ADD completion the invite is cleared and
  the block saved **before** the `auth` write; then the write raises `owner_epoch`. Even an RTC
  copy restored by an unexpected path would fail rule 5 after a successful ADD. If the write fails,
  the invite is gone (strict single use) and the owner creates a new one.
- **Arm consumption:** at recovery APPLY the arm is cleared and saved before the writes; recovery
  raises the epoch. A failed APPLY requires re-arming.
- INVITE_CREATE replaces an existing invite (O2); INVITE_CANCEL clears it; PHONE_REMOVE of the
  creator clears it (and raises the epoch anyway).

### 4.4 What RTC state can and cannot do

| RTC content | Without it | With it (valid) |
|---|---|---|
| Invite | ADD refused at msg 1 | ADD still needs PSK(setup code ‖ token) + button press + epoch match |
| Arm | RECOVERY refused at msg 1 | RECOVERY still needs PSK(setup code) + button press + 3-press confirmation + epoch match |

Arbitrary RTC contents (bug, CRC collision, physical tampering) can at most enable a handshake
purpose; they cannot complete one without the setup code.

---

## 5. Ownership state machines and authorization (review items 3–7)

### 5.1 Durable state (`auth`)

```text
                  CLAIM commit (1 write, epoch+1)
   UNCLAIMED ───────────────────────────────► CLAIMED(n=1)
                                                 │  ADD commit (n<4)  ──► CLAIMED(n+1)
                                                 │  REMOVE (n>1)      ──► CLAIMED(n-1)
                                                 │  RECOVERY apply    ──► CLAIMED(1), new identity
                                                 │  key mismatch      ──► CRED_DEGRADED (KEY_FAULT written)
   CRED_DEGRADED (dosing blocked) ── RECOVERY apply (record rewritten) ──► CLAIMED(1)
   (factory reset: M1.10b.4, section 10)
```

### 5.2 Overlays

```text
Invite:    NONE ─INVITE_CREATE(owner)─► ACTIVE ─ADD completion: clear+save, then commit─► NONE
           ACTIVE ─ cancel / replace / deadline / epoch change / any reset / clock step ─► NONE
Recovery:  IDLE ─verified hold >= 10 s─► ARMED ─RECOVERY handshake done─► CONFIRMING (RAM, 20 s)
           ARMED ─ deadline / RECOVERY_CANCEL / epoch change / any reset / clock step ─► IDLE
           CONFIRMING ─exactly 3 presses (5.7)─► APPLY (arm cleared+saved first) ─► IDLE
           CONFIRMING ─timeout / long press / other count / session end / due dose─►
                      ARMED (aborts+1; the third abort ─► IDLE)
```

### 5.3 Authorization matrix

Every BLE session begins with a physical button press (the device advertises only in a
button-wake service session); that press is the physical-presence proof for CLAIM and ADD.

| Operation | Caller | Preconditions (checked at HS msg 1 **and** at commit) | Proof |
|---|---|---|---|
| HELLO / Info | anyone in range during a session | — | none (claim state, RECOVERY_ARMED, CRED_DEGRADED) |
| CLAIM (XXpsk3) | unauthenticated | UNCLAIMED (valid record), setup code present | PSK(setup code) + button press |
| ADD (XXpsk3) | unauthenticated | CLAIMED, invite valid (4.2), free slot, key not enrolled | PSK(setup code, token) + button press |
| SESSION (IK) | enrolled phone | CLAIMED (not degraded), key in a used slot | phone key + pinned device key |
| RECOVERY (XXpsk3) | unauthenticated | arm valid (4.2), setup code present; CLAIMED or CRED_DEGRADED | PSK(setup code) + boot hold + button press + 3 presses |
| PHONES_LIST | OWNER | — | session |
| PHONE_REMOVE(slot) | OWNER (also itself) | slot used, not the last used slot | session |
| INVITE_CREATE | OWNER | a free slot exists | session |
| INVITE_CANCEL, RECOVERY_CANCEL | OWNER | — | session |

- Without OWNER authorization every owner operation returns FORBIDDEN before any storage or RTC
  access. Enrollment commits are not dispatcher operations (section 7).
- PHONE_REMOVE ends the current session after the response when the removed slot is its own.
- ADD with an already enrolled key: `DUPLICATE`; a phone that lost ENROLL_RESULT reconnects with
  IK (it learned the device key in XX message 2).

### 5.4 Initial claim (item 3)

1. Button press → session; HELLO reports UNCLAIMED (only for a valid UNCLAIMED record).
2. HS msg 1 (CLAIM): `dsp_app_sec_admit(CLAIM)`; `dsp_blesec` generates a **pending** identity
   key pair (RAM, RNG with RF on) used as the XX static key in msg 2.
3. Msg 3 completes: `dsp_app_sec_enroll(CLAIM, …)` re-checks UNCLAIMED and writes `auth` =
   {CLAIMED, identity, slot 0, epoch+1} in one write; AUTHENTICATED(slot 0); ENROLL_RESULT(0).
4. Cut or failure before the write: still UNCLAIMED, pending identity wiped, the phone repeats the
   claim. Cut after: HELLO reports CLAIMED; the phone connects with IK using the msg-2 key.

### 5.5 Adding a phone (item 4)

1. Owner A: INVITE_CREATE → token (16 B, device RNG), deadline 600 s, bound to the current epoch,
   RTC (saved at once). A shows a QR with the token and the device identity public key (O6).
2. A ends its session; the user presses the button; B connects.
3. B: HS msg 1 (ADD) with PSK = HKDF(setup code, ADD ‖ token).
4. Completion: re-check the invite (4.2) and a free slot → clear the invite and save → `auth` slot
   write (epoch+1) → ENROLL_RESULT(slot).

### 5.6 Revocation (item 5)

`PHONE_REMOVE(slot)` = `dsp_auth_remove` + one `auth` write (epoch+1). Only that slot changes;
the other phones' keys, names and times and every `manual_hw` are untouched. The removed phone's
next IK handshake fails. Last used slot: refused (REFUSED, detail LAST_SLOT).

### 5.7 Ownership recovery (item 6)

- **Arm (O4):** the button driver measures a continuous debounced press (no release longer than
  the 30 ms debounce) of ≥ 10 s **starting at the first sample after boot** (time before boot,
  e.g. the press that woke the device, is not counted), on a power-on reset or a button wake. It
  hands the measured duration to the wake (`recovery_hold_ms`); the domain arms only if
  ≥ 10,000 ms and only for that wake (the input is never retained). FR-026 keeps the pump off (the
  button is unarmed until a release). Effect: RTC ARMED (epoch, deadline), RECOVERY_ARMED shown;
  nothing else changes. Ignored on a valid UNCLAIMED device. Hold accuracy on the board is a
  hardware acceptance item.
- **Window:** fixed deadline of 30 min (4.2). Owners see the flag and may RECOVERY_CANCEL.
- **Claim:** a new session (separate short press) within the window; HS RECOVERY with
  PSK(setup code); the pending identity is a new key pair.
- **Confirmation bound to this recovery operation:** when the handshake completes, the app creates
  a confirmation context {session id, handshake transcript hash from `dsp_blesec`, monotonic start
  time, deadline start + 20 s}, flushes every button event already queued, and sends
  `EV_CONFIRM_ON_DEVICE {RECOVERY, 20}`. Only presses whose debounced press edge lies after
  start + 300 ms and before the deadline count (a press held at entry counts only after a release);
  presses are short (≤ 400 ms), separated by ≤ 500 ms, and the sequence ends after 700 ms idle
  (ARCHITECTURE timings). Exactly 3 confirm; a long press, any other count, the deadline, the end
  of the session or a due dose (BLE suspension) aborts. While the context exists, button events go
  only to it (FR-026 gestures and priming suspended). The result is consumed once, only by
  `dsp_app_sec_recovery_apply` with the same session id and transcript hash; aborting or applying
  destroys the context, and counters never carry over to another attempt. Button events carry the
  platform's monotonic timestamp, so delayed or duplicated queue entries cannot be replayed into a
  later context.
- **Apply:** (1) clear the arm and save the RTC block; (2) readiness SERVICE_REQUIRED via
  `dsp_dosing_set_readiness` if not already (restricting write, `dosing`); (3) `dsp_auth_recover`
  → one `auth` write (new identity, slot 0 = the new phone, other slots cleared, recoveries + 1,
  recovered_at, epoch+1, KEY_FAULT cleared). AUTHENTICATED(slot 0); ENROLL_RESULT(0). A failure in
  (2) or (3): abort, session closed, re-arm required. Refused with DEGRADED if the dosing state is
  not loaded (recovery never applies without a durable SERVICE_REQUIRED).
- No extra delay (O5).

### 5.8 What recovery preserves (item 7)

| Data | Recovery effect |
|---|---|
| History `h00`–`h31`, schedules, configuration (calibration, limits, reservoir) | unchanged (not written) |
| `time` (trust evidence, rollback hold, dosing floor) | unchanged |
| `dosing`: BLOCKED latch, refill intent, manual marker, ledger, last start, **`manual_hw[0..3]`**, result ring, admission counter | unchanged; only `readiness` may go READY → SERVICE_REQUIRED |
| `notify` | unchanged |
| Fail-safe inhibit (RTC) | unchanged |
| Phone slots, identity key, invite, arm | replaced / cleared |

---

## 6. Interfaces

### 6.1 Owner operations (dispatcher; `dsp_app.h`)

```c
typedef struct {
    uint8_t slot, name_len; char name[24];
    dsp_utc_t enrolled_at, last_seen; bool is_self;
} dsp_app_phone_t;

dsp_app_op_status_t dsp_app_phones_list(dsp_app_t *app, const dsp_app_auth_t *auth,
                                        dsp_app_phone_t out[DSP_APP_PHONE_SLOTS], uint8_t *n);
dsp_app_op_status_t dsp_app_phone_remove(dsp_app_t *app, const dsp_app_auth_t *auth, uint8_t slot,
                                         bool *session_ends);
dsp_app_op_status_t dsp_app_invite_create(dsp_app_t *app, const dsp_app_auth_t *auth,
                                          uint8_t token_out[16], uint16_t *expires_s);
dsp_app_op_status_t dsp_app_invite_cancel(dsp_app_t *app, const dsp_app_auth_t *auth);
dsp_app_op_status_t dsp_app_recovery_cancel(dsp_app_t *app, const dsp_app_auth_t *auth);
```

### 6.2 Security-layer operations (`dsp_app_sec.h`, only `dsp_blesec` and tests may include it)

```c
typedef enum { DSP_SEC_SESSION = 1, DSP_SEC_CLAIM, DSP_SEC_ADD, DSP_SEC_RECOVERY } dsp_sec_purpose_t;

typedef struct {
    uint8_t cred_state;          /* UNCLAIMED, CLAIMED, CRED_DEGRADED */
    bool recovery_armed, invite_active, slot_free;
    uint8_t id_hash[8];          /* HELLO / prologue (O6) */
} dsp_app_sec_view_t;

void dsp_app_sec_view(const dsp_app_t *app, dsp_app_sec_view_t *out);
dsp_app_op_status_t dsp_app_sec_admit(dsp_app_t *app, dsp_sec_purpose_t p);   /* msg 1 */
dsp_app_op_status_t dsp_app_sec_identity(dsp_app_t *app, uint8_t pub[32], uint8_t priv[32]);
                                         /* fresh read; caller wipes priv; refused if degraded */
dsp_app_op_status_t dsp_app_sec_lookup(dsp_app_t *app, const uint8_t phone_pub[32],
                                       dsp_app_auth_t *out);              /* IK → OWNER, slot */
dsp_app_op_status_t dsp_app_sec_invite_token(dsp_app_t *app, uint8_t token[16]);
dsp_app_op_status_t dsp_app_sec_enroll(dsp_app_t *app, dsp_sec_purpose_t p,   /* CLAIM / ADD */
                                       const uint8_t phone_pub[32], const char *name, size_t len,
                                       const uint8_t *new_priv, const uint8_t *new_pub,
                                       dsp_app_auth_t *out);
dsp_app_op_status_t dsp_app_sec_recovery_begin(dsp_app_t *app, uint32_t session_id,
                                               const uint8_t transcript_hash[32]);  /* CONFIRMING */
dsp_app_recovery_state_t dsp_app_sec_recovery_state(const dsp_app_t *app);
dsp_app_op_status_t dsp_app_sec_recovery_apply(dsp_app_t *app, uint32_t session_id,
                                               const uint8_t transcript_hash[32],
                                               const uint8_t phone_pub[32], const char *name, size_t len,
                                               const uint8_t new_priv[32], const uint8_t new_pub[32],
                                               dsp_app_auth_t *out);
dsp_app_op_status_t dsp_app_sec_key_fault(dsp_app_t *app);   /* writes KEY_FAULT → CRED_DEGRADED */
void dsp_app_sec_handshake_failed(dsp_app_t *app);           /* counted; <= 1 write per session */
```

The pending identity stays in `dsp_blesec` RAM until it is passed to the commit; `dsp_app` never
keeps a private key beyond one call.

### 6.3 Platform inputs

```c
uint32_t recovery_hold_ms;   /* wake input: verified continuous hold from the first sample after boot */
bool dsp_app_post_button(dsp_app_sync_t *s, dsp_button_event_t ev);  /* {kind SHORT|LONG, press_ms, mono_ms} */
typedef struct { void *ctx; bool (*read_setup_code)(void *ctx, uint8_t out[16]); } dsp_secret_port_t;
                             /* used ONLY by dsp_blesec, never by dsp_app */
```

---

## 7. Integration boundaries (review item 9)

```text
NimBLE task (M1.10d)        frames only: reassembly, dsp_app_post_frame(); never parses or decrypts
        │ bounded queue (M1.9b lock contract); button events (M10) share the queue
        ▼
Application task
  dsp_blesec (M1.10c)       Noise XXpsk3/IK (noise-c), PSK from the setup code (dsp_secret_port),
        │                   pending identity, transport keys, frame-level limits; uses ONLY dsp_app_sec.h
        ├──► dsp_app_sec_*  admission per purpose, identity read, IK lookup, enrollment and
        │                   recovery commits, key-fault report
        ▼
  dispatcher (M1.10c/d)     decrypted request → exactly one dsp_app_* operation with the
        │                   dsp_app_auth_t produced by dsp_blesec
        ▼
  dsp_app ──► dsp_auth (pure) ──► dsp_storage (auth) ──► dsp_kv (NVS)
          ──► dsp_dosing (credentials gate, readiness, manual-dose anti-replay + admission)
          ──► dsp_power retained block (invite, arm)
```

- **Header boundary checked by the build:** a CMake check (like `test_endpoint_guard.cmake`) fails
  if any file other than `dsp_blesec.c` and host tests includes `dsp_app_sec.h`.
- `dsp_app_auth_t` is produced only by `dsp_blesec` (lookup or commit); the dispatcher passes it
  through. Anti-replay unchanged: MANUAL_DOSE uses `auth->slot` from the verified session.

---

## 8. Failure handling and power loss (review item 8)

| Failure | Behavior |
|---|---|
| `auth` missing / CRC-dropped / malformed / newer under v5 | CRED_DEGRADED: dosing blocked, DEVICE_ERROR, RECOVERY only |
| Identity key pair inconsistent (`dsp_blesec`, first use per boot) | `dsp_app_sec_key_fault`: KEY_FAULT written (epoch+1) → CRED_DEGRADED persistently; if the write fails, degraded for this wake |
| Setup code missing / wrong length | CLAIM/ADD/RECOVERY refused (HANDSHAKE_FAILED); IK works; Diagnostics flag ("present/missing" only) |
| `auth` write fails (error, NO_SPACE, read-back differs) | STORAGE; durable state is old or new → reloaded at once; dosing ends for the wake (existing rule) |
| Torn / implausible RTC block | overlays cleared (4.2) |
| Duplicate key / key equal to identity / all-zero key | enrollment refused; a stored one makes the record invalid |

| Operation | Writes (in order) | Cut before the last write | Cut after |
|---|---|---|---|
| Claim | `auth` | UNCLAIMED | CLAIMED; phone uses IK |
| Add | RTC invite clear+save, `auth` | not enrolled, invite consumed | enrolled; IK |
| Remove | `auth` | slot still there | removed |
| Recovery | RTC arm clear+save, `dosing` readiness, `auth` | between: SERVICE_REQUIRED with the old owners (safe), re-arm | recovered |
| Key fault | `auth` | degraded for the wake only | degraded until recovery |
| last_seen / failed_sessions | `auth` | old value | new value |
| Migration | `auth`, marker | v4 (resumes) | v5 |

Every cut here is a power loss or reset, which also clears the RTC overlays (4.2 rule 1).

---

## 9. Secure generation, provisioning and storage (review item 10)

- **Setup code (O7):** `provision_dev_secrets.py` gains an optional `setup_code` entry. If absent,
  it generates one with `secrets.token_bytes(16)`, stores it (hex) in the owner-only secrets JSON
  outside the repository (0600, existing checks) and writes it to `nvs_sec` (`dsp_sec` /
  `setup_code`). The label `DSP2:<device-id-hex>:<code>` (26 Crockford base32 characters + 2 check
  characters = first 10 bits of SHA-256("DSP2 setup" ‖ code)) goes to a separate 0600 file outside
  the repository (`--label-out`), **never printed**; `--device-id` from the developer (e.g.
  `esptool read_mac`; the tool touches no hardware); rotation only with `--new-setup-code`.
  Production: manufacturing, out of scope. On the device: read only by `dsp_blesec` through
  `dsp_secret_port_t`, wiped after the PSK derivation.
- **Identity key:** generated on the device at claim and recovery by the security layer (noise-c
  key generation via its RNG hook → `esp_fill_random`) while BLE is active (RF on: true RNG;
  hardware acceptance item); stored in `auth`; public key pinned by phones; private key read
  afresh per handshake and wiped by the caller; storage buffers wiped. Lifecycle: none
  (UNCLAIMED) → created at claim → used for every handshake → checked once per boot (KEY_FAULT on
  mismatch) → replaced at recovery → wiped by a future factory reset.
- **O6 encoding (to be verified in M1.10c before freezing):** `id_hash` = first 8 bytes of
  SHA-256("DSP2 id" ‖ identity public key) when claimed, of SHA-256("DSP2 dev" ‖ device ID) when
  unclaimed; it identifies, it is not a secret and gives no authentication by itself (the Noise
  handshake authenticates). The invite QR carries the identity public key so B can compare it with
  the static key in XX message 2.
- **Test material:** visibly synthetic patterns generated in test code; a spy over all output
  enforces that no fixture key, token or setup code is printed.
- **Residual (T8/T10):** NVS and `nvs_sec` are not encrypted (needs flash encryption / eFuses: out
  of scope).

---

## 10. Factory reset vs. ownership recovery

| | Ownership recovery (M1.10b.3) | Factory reset (M1.10b.4, to be specified) | NVS erase |
|---|---|---|---|
| Trigger | boot hold + setup code + session + 3 presses | owner session + confirm constant (physical confirmation to be decided in M1.10b.4) | **never** by the firmware (existing rule) |
| Ownership | new identity, new phone in slot 0 | all slots and identity cleared → UNCLAIMED (epoch+1, never reset) | — |
| Dosing safety state | unchanged; SERVICE_REQUIRED forced | SERVICE_REQUIRED first | — |
| History, time record, `manual_hw`, admission counter, notifications | preserved | **preserved** (O11; safety history is never erased by a user action) | — |
| Configuration, schedules, Wi-Fi | preserved | returned to provisioning values (no limits, no calibration → no dosing possible), schedules emptied keeping tombstones, Wi-Fi cleared | — |

Constraints already fixed for M1.10b.4: a selective, ordered rewrite of named records, never an
erase; restricting writes first (a `dosing` write with SERVICE_REQUIRED and a reset intent), then
configuration, schedules, Wi-Fi, `auth` last, then the intent cleared; resumable at boot from the
intent (roll-forward like refill); refused while a refill or a manual marker is pending. It is a
separate design review; M1.10b.3 implements none of it.

---

## 11. Admission control (O10) and the M1.10b.2 wear risk

### 11.1 Risk (documented in `firmware/README.md` and the baseline 4.7)

A durable refusal costs 0.0048 erases of the most-erased page (measured); an enrolled phone that
keeps sending valid, fresh but refused requests adds about 4.8 cycles per 1,000. Schedule writes
are costlier still (0.051 per maximum-size write, measured in M1.10b.1).

### 11.2 Manual-dose decisions: durable counter, no extra write

`dosing` format 3 (212 B, still 9 NVS entries; format 2 decodes with counter 0 and date 0):

```text
206..209  adm_date (local date of the counting day; 0 = none yet)
210..211  adm_count u16 (fresh decisions counted on adm_date or since, see below)
```

Order in `dsp_app_manual_dose`:

1. validate (invalid → INVALID, nothing written, not counted);
2. **decided lookup** (`seq <= manual_hw[slot]` → DUPLICATE with the stored result): always
   answered, never counted, never limited;
3. **admission:** compute the effective count (below); if it has reached the limit → `BUSY` with
   detail `RATE_LIMITED` (protocol v1.0 amendment), **nothing written, request stays undecided**;
4. decide as in M1.10b.2. The counter update rides in the **same** write that makes the decision
   durable (write 1 for an accepted request, the single refusal write otherwise). No admission
   check ever writes flash on its own.

Counting day and time:
- With a trusted clock: if today's local date > `adm_date`, the count restarts at 0 and `adm_date`
  becomes today (stored with the decision write); otherwise the stored count applies.
- **Without a trusted clock:** the stored count applies and never restarts (`adm_date` unchanged);
  decisions keep counting (a manual dose without a trusted clock is a durable GATES refusal). The
  budget refreshes only after a trusted date later than `adm_date` is observed (NTP or SET_TIME).
- **Clock rollback:** a trusted date ≤ `adm_date` never restarts the count, so moving the clock back
  (or a rollback hold) cannot refill the budget. A forward step can refill it at most once per
  date advance, bounded by the M1.4/M1.10b.1 time rules.
- **Reconnect / reboot / power loss:** the counter is durable with the decision itself, so none of
  them resets it; a cut before the decision write leaves both decision and counter unchanged.
- **Firmware rollback:** older firmware reports `dosing` format 3 as UNSUPPORTED (fails closed, R3).

Limits (configurable at build time, never over BLE): `CONFIG_DSP_MANUAL_DECISIONS_PER_DAY`
(default 32, range 1–1000) carried in `dsp_app_config_t`; an optional per-session limit (default 8,
RAM) only smooths bursts. At 32 per day the worst refusal flood adds ≈ 0.15 worst-page erases a day
(≈ 560 cycles in 10 years).

Anti-replay is unchanged: a not-admitted request writes and executes nothing, `manual_hw` does not
move, REFUSED keeps meaning "durably decided", and only the enrolled phone can resend the request
(Noise session keys and nonces).

### 11.3 Other durable BLE writes (M1.10c)

Configuration, schedule and ownership changes write only on success. Proposed for M1.10c: a
per-session limit and an RTC-window weighted budget (by NVS entries). These reset on power loss,
which needs physical access and a new button press; documented residual (decision R2 below).

---

## 12. Test plan

| Area | Tests (host, ASan/UBSan) |
|---|---|
| Codec | golden bytes (UNCLAIMED; CLAIMED with 1 and 4 slots; KEY_FAULT) from an independent Python `struct` generator; every validity rule rejected individually; exact length, zero padding, newer format |
| Transitions | claim/add/remove/recover/key fault pre- and postconditions, epoch increments, SLOTS_FULL, LAST_SLOT, DUPLICATE_KEY, names, unchanged input on refusal |
| Three states | UNCLAIMED vs CLAIMED vs CRED_DEGRADED from every open path (missing, CRC-dropped, malformed, newer, KEY_FAULT); degraded never reported or treated as UNCLAIMED; dosing gate blocks automatic and manual dosing; DEVICE_ERROR raised; only RECOVERY admitted; recovery heals and leaves SERVICE_REQUIRED |
| Migration | v4 → v5 and v1/v2/v3 → v5 with a cut at every write; `MIGRATION_REQUIRED` read-only; v4 marker + non-provisioning `auth` → CORRUPT; no marker + real `auth` → CORRUPT; real NVS (`test/nvs_linux`): migration and `auth` writes under the single/two-cut matrices |
| RTC | every reset reason (power-on, brownout, software, fail-safe, watchdog, panic) clears overlays; TIMER and BUTTON keep them; expiry at deadline − 1 s / deadline / + 1 s; many deep sleeps do not extend; repeated resets; clock step, backward raw clock, `saved_raw` regression; torn block; CRC-valid but inconsistent block (wrong window length, flags); epoch mismatch after add/remove/recovery; consumed invite with the old RTC image restored → refused |
| Recovery confirmation | exactly 3 presses; 2, 4, long press, press before start + 300 ms, press held at entry, queued pre-context events, events with old timestamps, timeout, session end, due dose; wrong session id / transcript hash at apply; second apply refused; abort counting to disarm; FR-026 gestures suppressed |
| Interrupted recovery | cut at each step (arm clear, readiness write, `auth` write) with every reset reason; invariants: never UNCLAIMED from CLAIMED, never recovery without durable SERVICE_REQUIRED, arm never reusable |
| Authorization | every owner op × {NULL, NONE, OWNER} → FORBIDDEN before any KV/RTC access (spy); purposes × states × overlay states at admit and commit; state change between msg 1 and commit |
| Preservation | snapshot every key: byte-identical after claim/add/remove/recovery except `auth` and the readiness byte |
| Anti-replay | manual floor continuity across remove + re-add, recovery, exhaustion; no earlier `seq` executes |
| Admission | limit − 1 / limit / limit + 1 (configured 1, 32, 1000); decided retries at the limit still answered and not counted; invalid not counted; counter in the decision write (cut before/after); reboot and power loss keep it; date advance restarts; untrusted clock keeps counting; trusted rollback never restarts; format 2 → 3 upgrade; no write on a rate-limited answer (KV write spy) |
| Faults | NO_SPACE, write error, read-back mismatch on each write → STORAGE, reload, old or new record |
| Secrets | output spy over all suites; storage buffers and scratch zero after `auth` load/save |
| Boundary | build check for `dsp_app_sec.h` includes |
| Tool | setup code generated, stored, label 0600 outside the repo, never printed (captured output scanned), check-character vectors |
| Wear / size | wear harness for `auth`, `dosing` format 3; cross-builds, stack per operation |

---

## 13. Resource estimates (before implementation; measured values in section 16)

| Item | Estimate |
|---|---|
| Code (runtime image) | +6–7 KB; default image unchanged |
| Static DRAM | +0.8 KB (`dsp_auth_t` scratch ≈ 0.42 KB, public slot cache ≈ 0.35 KB, confirmation context) |
| RTC memory | retained block 48 → ≈ 112 B (8 KB available) |
| Stack | ≤ 600 B per ownership operation (static storage buffers) |
| NVS | +15 entries (`auth`); `dosing` format 3 stays at 9 entries |
| Wear | `auth` ≈ 0.008 worst-page erases per write (estimate by entry count), a few writes a week; admission adds no writes |

---

## 14. Decisions

### 14.1 Status after the review of 2026-11-17

| # | Decision | Status |
|---|---|---|
| O1 | RTC storage for invites and recovery arm | approved; conditions implemented in section 4 (deep-sleep only, fixed deadlines, epoch binding, consume before commit, CRC not authority) |
| O2 | one active invite | approved |
| O3 | corrupt/unsupported credentials | **changed**: CRED_DEGRADED blocks all dosing until recovery or maintenance; three states distinguished (3.1) |
| O4 | arming from power-on or a button wake | approved; hold measured from the first sample after boot, verified by the domain (5.7) |
| O5 | no recovery delay | approved |
| O6 | HELLO hash and invite QR binding | approved pending verification of encoding and properties in M1.10c (section 9) |
| O7 | provisioning-generated setup code and label | approved |
| O8 | rate-limited last_seen / counters | approved |
| O9 | CRED_DEGRADED indicator | approved |
| O10 | admission control in the Application Service before BLE exposure | approved; durable counter in the decision write (11.2), implemented in M1.10b.3 |
| O11 | preserve manual-dose counters | approved; factory reset kept separate (section 10) |

### 14.2 New, needing approval

| # | Decision | Recommendation |
|---|---|---|
| R1 | Overlays survive only deep-sleep wakes; every reset (including the fail-safe software reset) clears them | yes (strictest reading of "untrusted RTC state"; a reset during a 10/30-minute window is rare and only costs a retry) |
| R2 | Admission for non-manual BLE writes (config, schedules, ownership) via session + RTC-window budget in M1.10c, resetting on power loss (documented residual) | yes; making them durable would need counters in each record |
| R3 | `owner_epoch` in `auth` and KEY_FAULT persisted on a key mismatch | yes |
| R4 | `dosing` format 3 for the admission counter (lazy upgrade inside layout v5) | yes |
| R5 | Recovery apply bound to the session id and handshake transcript hash supplied by `dsp_blesec` | yes |

---

## 15. Proposed split

| Step | Content |
|---|---|
| M1.10b.3 | `dsp_auth` + codec, layout v5 + migration, three credential states and the credentials gate, owner and security-layer operations (with synthetic key bytes), RTC overlays with the section 4 rules, recovery confirmation on button events (domain input), admission control with `dosing` format 3, header boundary check, setup-code provisioning in the dev tool, docs |
| M1.10b.4 | Wi-Fi store; factory reset (separate design review, section 10 constraints) |
| M1.10c | `dsp_blesec`: noise-c, key generation, PSK, HELLO/prologue (O6 verified), identity check, frame-level limits, non-manual write budgets (R2) |

---

## 16. Implementation notes (M1.10b.3)

Implemented as designed, with the final clarifications of the approval (2026-11-17):

- **KEY_FAULT:** `dsp_app_sec_key_fault` inhibits dosing in RAM first and keeps it inhibited for
  the rest of the wake even if the KEY_FAULT write fails; any later reload of the record in the
  same wake cannot make it CLAIMED again. The flag is diagnostic for later wakes.
- **Authorization at runtime:** the restricted header and its configure-time check are defense
  in depth. Owner operations require an authorization issued by the Application Service for the
  current session and epoch (random 64-bit token); security-layer calls require a registered
  security layer (`cfg->security`, NULL in every build before M1.10c), a button-wake session, and
  a single-use handshake handle (purpose, random nonce, 30 s, epoch). Tested: missing, expired,
  mismatched, replayed and forged evidence.
- **Rate limiting:** the durable manual-dose counter (`dosing` format 3) as designed; session
  limits for all operations (256), durable writes (48) and writes per minute (16). The write
  defaults were raised from the 32 / 8 first written in code: a first setup (16 schedules,
  configuration, time) must fit one session. All are build-time configuration
  (`dsp_app_config_t`), never changeable over BLE; session limits reset on reconnect or reset
  (documented v1 limitation).
- **Not in this milestone (explicit M1.10c deliverable):** HELLO/Info flag bit 1 (CRED_DEGRADED,
  O9), implemented in one change in the normative specification, the shared vector generator and
  vectors, the C codec and the Kotlin codec; until then the wire format stays bit 0 only (no
  undocumented difference). The domain exposes the state through `dsp_app_sec_view`.
- **Integration blocker:** configuration, schedule, time, manual-dose, REFILL and SET_READINESS
  operations must not become reachable through BLE until M1.10c provides security-layer-issued
  authorization and they verify it (session ID, token, epoch) like the ownership operations; a
  caller-provided or hand-crafted owner authorization must never suffice for an externally
  reachable operation. BUSY details 1 (RATE_LIMITED) and 2 (CONFIRMING) and STATUS
  gate bit 9 (CREDENTIALS) need no codec change (carried as-is).

Measured (ESP32-C3 runtime image, simulated pump, against M1.10b.2 `aa0532f`):

| Item | Measured |
|---|---|
| Image | 238,224 B (+6,352 B); default image unchanged (148,480 B) |
| Static DRAM | 84,487 B (+1,384 B); `s_app` 17,472 B (+1,360 B) |
| RTC slow memory | 256 B (+64 B; domain block 120 B) |
| Stack | worst wake chain 3,152 B (+48 B, same method); ownership operations ≤ 688 B (recovery apply) |
| NVS | dispenser namespace 324 entries (real NVS, `test/nvs_linux` check 1) |
| Real-NVS power loss | v4 → v5 migration: 154 cuts, all reopen as v5; `auth` writes: 2 × 128 cuts, always the old or the new record |
