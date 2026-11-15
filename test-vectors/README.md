# Shared test vectors

Machine-readable test cases that the Android app and the ESP32 firmware both run, so that the two
implementations of the same product rule cannot drift apart.

| File | Content | Consumers |
|---|---|---|
| `recurrence_v1.txt` | Schedules and expected occurrences (ONCE/DAILY/WEEKLY, DST, boundaries, validation, identity) | `firmware/test/host/test_recurrence_vectors.c`, `android/app/src/test/.../domain/RecurrenceVectorTest.kt` |
| `europe_prague_dst_v1.txt` | Europe/Prague DST transition instants 2000–2099 | same two tests |

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
