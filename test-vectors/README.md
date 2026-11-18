# Shared test vectors

Machine-readable test cases that two implementations of the same rule both run, so that they
cannot drift apart: the Android app and the ESP32 firmware (scheduling), and the ESP32 firmware
and the Cloudflare backend (notification protocol).

| File | Content | Consumers |
|---|---|---|
| `recurrence_v1.txt` | Schedules and expected occurrences (ONCE/DAILY/WEEKLY, DST, boundaries, validation, identity) | `firmware/test/host/test_recurrence_vectors.c`, `android/app/src/test/.../domain/RecurrenceVectorTest.kt` |
| `europe_prague_dst_v1.txt` | Europe/Prague DST transition instants 2000–2099 | same two tests |
| `ble_protocol_v1.txt` | BLE protocol v1 (M1.10a, `docs/BLE_PROTOCOL.md`): frames ↔ fields for every message type, rejections with reasons, encrypted headers, forward compatibility, negotiation, fragmentation/reassembly, Info and advertising records | `firmware/test/host/test_bleproto.c`, `android/app/src/test/.../ble/protocol/BleProtocolVectorTest.kt` |
| `ble_session_v1.txt` | BLE session security (M1.10c.2a, `docs/BLE_PROTOCOL.md` section 11): PSK (HKDF-SHA256 with the context in `info`), negative vectors (context in the salt, swapped salt/info, wrong purpose, bit flips), prologue, `device_id_hash` | `firmware/test/host/test_secderive.c` (libsodium provider), `firmware/test/psk_linux` (mbedTLS/TF-PSA-Crypto provider, ESP-IDF linux target); Android (M1.10e.1): `android/app/src/test/.../crypto/SessionVectorsTest.kt` (JNI), the native runner `android/app/src/test/native/dspnoise_vectors_test.c` (host and NDK builds), instrumented `NativeNoiseInstrumentedTest` |
| `ble_handshake_v1.txt` | BLE handshake transcripts (M1.10e, decision E7): CLAIM, ADD, SESSION and RECOVERY over one device lifetime with pinned keys and ephemerals, prologues, PSKs, payloads, every HS frame, handshake hashes and encrypted frames (header as AD), plus negative records. Dispenser framing, **not** Noise conformance vectors | `firmware/test/host/test_handshake_vectors.c` (real `dsp_app` + `dsp_blesec`, scripted host RNG), `android/app/src/test/.../crypto/HandshakeVectorsTest.kt` (JNI), `android/app/src/test/native/dspnoise_vectors_test.c` |
| `qr_formats_v1.txt` | Setup label (`DSP2:`) and owner invitation (`DSP2I1:`) formats (M1.10e, `docs/ANDROID_BLE_DESIGN.md` 7.1): canonical encodings, manual-entry normalizations, every refusal reason, invitation timing | `android/app/src/test/.../crypto/QrFormatsVectorTest.kt` |
| `notification_protocol_v1.txt` | Notification protocol v1 (M1.8): firmware notes → exact request bodies, canonical payloads and SHA-256; notes the firmware must refuse; raw requests → the backend's exact responses; responses → the firmware's classification | `firmware/test/host/test_proto_vectors.c`, `cloudflare/dispenser-notify/test/protocol-vectors.test.ts` |

Expected values are written by hand from the Android unit tests and the IANA rules (the DST table
was generated from IANA tzdata 2026e with Python `zoneinfo`). They are **never** produced by
either implementation under test. A vector that one engine fails is a parity bug to investigate,
not a value to regenerate.

## Format v1

Plain UTF-8 text, one record per line. `#` starts a comment; blank lines are ignored. A record
is a type followed by space-separated `key=value` tokens (values never contain spaces). `-`
means "absent/empty".

- Instants: UTC, `YYYY-MM-DDTHH:MM:SSZ`. Dates: `YYYY-MM-DD` (local, schedule timezone).
  Times: `HH:MM` (local). Timezone of every record: `Europe/Prague`.
- An occurrence is written `<local date>@<instant>`; lists are comma-separated.
- `scope=c` marks a firmware-only rule (canonical form, supported date range); the Kotlin test
  skips it.

| Record | Keys | Meaning |
|---|---|---|
| `schedule` | `name id rev enabled kind start time interval weekdays end amount_ul` | Defines a schedule under the label `name`. `kind` ONCE/DAILY/WEEKLY, `weekdays` like `MO,TH`, `amount_ul` integer microlitres. May be invalid (validation cases). |
| `validate` | `schedule expect` | `expect` is `OK` or comma-separated error names (Kotlin `ValidationError` names; firmware-only names with `scope=c`). |
| `local_to_utc` | `date time kind expect` | `kind` is `unique`, `overlap` (expect = earlier instant) or `gap` (expect `-`). |
| `next` | `schedule after expect` | Next occurrence strictly after `after`, or `none`. |
| `seq` | `schedule after n expect` | Up to `n` successive next occurrences; `expect` is the exact list. |
| `between` | `schedule after until limit expect` | Occurrences with after < t ≤ until; `overflow` if there are more than `limit`. |
| `occurrence_on` | `schedule date expect` | Occurrence on a local date: instant or `none`. |
| `occurrence_id` | `schedule date expect` | Occurrence identity string. |
| `next_set` | `schedules after expect` | Earliest occurrence across schedules (`-` = none): `<schedule id>@<instant>` or `none`. |
| `tz_transition` | `year dst_start dst_end` | UTC instants of the DST start/end in that year. |

Disabled schedules have no occurrences in any query (Android applies `enabled` in every
consumer of `RecurrenceEngine`; the firmware engine applies it itself).

Adding a record type or changing the meaning of a key requires a new format version (`_v2`).

## BLE protocol vectors (`ble_protocol_v1.txt`)

Generated by `tools/gen_ble_protocol_v1.py` (`python3 -I gen_ble_protocol_v1.py >
../ble_protocol_v1.txt`, deterministic) from a schema transcribed from `docs/BLE_PROTOCOL.md`,
independently of the firmware and Android codecs. The file header documents its record types
(`msg`, `enc`, `bad`, `compat`, `negotiate`, `frag`, `reasm_bad`, `reasm_ok`, `info`, `info_bad`,
`advert`). A change to the protocol changes the specification, then the generator, then both
codecs.

## BLE handshake vectors (`ble_handshake_v1.txt`)

Generated by `tools/gen_ble_handshake_v1/`, a small Rust program using the crate **`snow` 0.10.0**
(a Noise implementation independent of noise-c, which the firmware and the app use), RustCrypto
`hkdf`/`sha2` for the derivations and `chacha20poly1305` for frames with the header as associated
data; every crate version is pinned by the committed `Cargo.lock`:

```sh
cd tools/gen_ble_handshake_v1
cargo run --locked --release -- <noise-c tests/vector dir> ../../ble_session_v1.txt > ../../ble_handshake_v1.txt
```

The noise-c vector directory is the pinned tree fetched by the firmware or app build (e.g.
`android/app/build/dsp_deps/noise-c-62c2fdca7a874ea898761147bb062ca1cde9f36d/tests/vector`). Before
writing anything the tool reproduces the official Noise vectors for both protocols through `snow`
(0 skipped), checks its AEAD path against `snow`'s own transport encryption, and reproduces every
record of `ble_session_v1.txt`; any mismatch exits without output. Deterministic ephemeral keys
exist only in this tool and in host-only test code. The official Noise conformance vectors stay in
the pinned noise-c tree and are run separately (never merged into this file).

## QR format vectors (`qr_formats_v1.txt`)

`python3 -I tools/gen_qr_formats_v1.py > qr_formats_v1.txt` (standard library: `hashlib` and integer
base32 only). The label encoding is cross-checked against `firmware/tools/provision_dev_secrets.py`.

## Notification protocol vectors (`notification_protocol_v1.txt`)

Same line format. Expected values were produced from the `PROTOCOL.md` rules by a standalone
Python script (compact `json.dumps` and `hashlib` SHA-256), never by the firmware or the
backend. The file header documents the record types:

| Record | Firmware test | Backend test |
|---|---|---|
| `encode` | the encoder produces exactly `body`; `canonical` = body without `schema_version`/`attempt` | validator accepts `body`; canonical payload = `canonical`; hash = `sha256`; POST answers 201 then 200 with the exact acknowledgement bodies; D1 stores `canonical` |
| `noencode` | the encoder refuses the note | — |
| `request` | classifies the backend's `response` as `result`/`diag`/`error`/`field` | POST `body` answers exactly `status` + `response` |
| `response` | classifies `status` + `body` for the sent ID | for `scenario != -`: the backend produces exactly this status and body |

A changed protocol needs a new file (`notification_protocol_v2.txt`); v1 vectors stay valid for
as long as v1 is accepted.

## BLE session vectors (`ble_session_v1.txt`)

Generated by `tools/gen_ble_session_v1.py` (`python3 -I gen_ble_session_v1.py >
../ble_session_v1.txt`) with the Python standard library only: RFC 5869 HKDF is written out over
`hmac`/`hashlib` and checked against the RFC's test cases 1–3 before anything is emitted. It shares
no code with the providers under test (mbedTLS/TF-PSA-Crypto on the ESP32-C3, libsodium on the host
and on Android); OpenSSL's HKDF reproduces every `psk` record as an additional cross-check. A vector
an implementation fails is a contract bug, never a value to regenerate.
