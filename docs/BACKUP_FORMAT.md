# Backup format (`dispenser-backup`, schema version 1)

Configuration backups are exported and restored from **Nastavení → Záloha a obnovení**
(milestone A1.4c.1). Files are chosen with the Android Storage Access Framework; the app
needs no storage permission and never uploads a backup anywhere.

Android system backup is disabled for the app (`allowBackup="false"`, all domains excluded in
`backup_rules.xml` and `data_extraction_rules.xml`, A1.5): neither Google cloud backup nor
device-to-device transfer carries app data. This manual export is the only way to move data.

Encoding: UTF-8 JSON (a leading BOM is tolerated). Maximum size: 20 MB.
Implementation: `data/backup/BackupCodec.kt` (encoder + strict validating decoder).

## What a backup contains (and what it never contains)

| Included | Never included |
|---|---|
| Aquarium display name | Credentials, tokens, signing keys, BLE pairing secrets |
| All readable schedules with their ids and revisions | Device synchronization state (`PENDING` / `DEVICE_APPLIED`) |
| Optional, off by default: simulation state, processed occurrences, simulation history | Room table layout or other database internals |
| | Unreadable (corrupted) schedule records |

## Top level

```json
{
  "format": "dispenser-backup",
  "schemaVersion": 1,
  "exportedAt": "2026-11-15T10:00:00Z",
  "applicationVersion": "0.4.0 (4)",
  "settings": { "displayName": "Obývák" },
  "schedules": [ … ],
  "simulation": null
}
```

| Field | Type | Rules |
|---|---|---|
| `format` | string | must be `dispenser-backup` |
| `schemaVersion` | integer | must be `1`; anything else is reported as an unsupported version |
| `exportedAt` | ISO-8601 instant | |
| `applicationVersion` | string | informational (`versionName (versionCode)`) |
| `settings.displayName` | string or `null` | `null` = default name; at most 30 characters after normalization |
| `schedules` | array | at most 1000 entries; ids unique |
| `simulation` | object or `null` | `null` unless the user chose "Zahrnout simulaci a historii" |

Every field is required (nullable fields are written as `null`). **Unknown fields are
rejected.** Any extension of the format increments `schemaVersion`.

## Schedule

```json
{
  "id": "2f0c7c1e-6d0a-4c39-9a51-0c1f0c7b8a11",
  "revision": 3,
  "name": "Ranní",
  "enabled": true,
  "amountMicrolitres": 12500,
  "startDate": "2026-11-01",
  "localTime": "08:00",
  "zoneId": "Europe/Prague",
  "recurrence": { "kind": "WEEKLY", "interval": 1, "weekdays": ["MONDAY", "THURSDAY"] },
  "endDate": null
}
```

| Field | Rules |
|---|---|
| `id` | 1–64 characters `[A-Za-z0-9._-]` (app-generated UUIDs); preserved on restore |
| `revision` | integer ≥ 1; preserved on restore |
| `name` | string (kept byte-identical) |
| `amountMicrolitres` | integer, 1 … 2^53−1 (1 µl precision; 12.5 ml = `12500`) |
| `startDate`, `endDate` | ISO `yyyy-MM-dd`, strictly valid calendar dates; `endDate` inclusive or `null` |
| `localTime` | `HH:mm` (24 h, no seconds) |
| `zoneId` | IANA region id (e.g. `Europe/Prague`); fixed offsets are rejected |
| `recurrence.kind` | `ONCE`, `DAILY` or `WEEKLY` |
| `recurrence.interval` | every N days (`DAILY`) / weeks (`WEEKLY`); must be `1` for `ONCE` |
| `recurrence.weekdays` | `MONDAY` … `SUNDAY`, no duplicates; non-empty for `WEEKLY`, empty otherwise |

`ONCE` schedules must have `endDate: null`. Each schedule is additionally checked by the
domain `ScheduleValidator` (interval, weekdays, end before start, and a `ONCE` time that does
not exist because of the spring DST change).

## Simulation (optional, simulated data only)

```json
"simulation": {
  "dataOrigin": "SIMULATION",
  "state": {
    "runId": "…", "startedAt": "…", "clock": "…", "readiness": "READY", "nextSeq": 42,
    "reservoir": { "capacityMicrolitres": 500000, "remainingMicrolitres": 120000,
                   "updatedAt": "…", "blocked": false, "lowWarningLatched": false },
    "processedOccurrences": [ { "occurrenceId": "<scheduleId>@2026-11-21", "localDate": "2026-11-21" } ]
  },
  "history": [ { "id": "…", "seq": 1, "time": "…", "type": "DISPENSE_SUCCESS", "origin": "SIMULATION",
                 "reason": "SCHEDULED", "scheduleId": "…", "scheduleNameSnapshot": "Ranní",
                 "occurrenceId": "…@2026-11-21", "requestedMicrolitres": 10000, "dispensedMicrolitres": 10000,
                 "reservoirBeforeMicrolitres": 130000, "reservoirAfterMicrolitres": 120000,
                 "capacityMicrolitres": 500000 } ]
}
```

- `dataOrigin` must be `SIMULATION`; every history event must have `origin: SIMULATION`.
  A backup can never introduce real-device history.
- `state` is `null` when no simulation run was active (history may still exist).
- `processedOccurrences` restores the at-most-once guard: an occurrence already processed is
  never executed again, even after its schedule is edited.
- Consistency: `clock ≥ startedAt`; reservoir `0 ≤ remaining ≤ capacity`; event ids and `seq`
  unique; with a state, every `seq` < `nextSeq`; occurrence ids are `<scheduleId>@<localDate>`.

## Restore semantics

- **Replace, never merge.** All readable schedules and the display name are replaced.
- Restored schedules are stored as `PENDING` ("Uloženo v telefonu"), never as device-confirmed.
- Unreadable stored records are left untouched; an imported id equal to one of them rejects
  the whole backup.
- Simulation:
  - backup with simulation → current simulation state, processed set and history are replaced;
  - backup without simulation, schedules different (ignoring revisions) and a run active →
    the run is stopped (state + processed set removed); history is kept; a new run must be
    started explicitly (its event sequence continues after the kept history);
  - identical schedules or no active run → simulation unchanged.
- The confirmation dialog lists the number of schedules, the aquarium name, whether
  simulation data is included and what will be replaced. Nothing is written before the user
  confirms.

## Crash safety (internal, not part of the format)

The three stores (`dispenser.db`, `simulation.db`, DataStore) cannot share one transaction.
Restore therefore runs under an exclusive lock, non-cancellable:

1. decode and validate the whole file (invalid input never reaches storage);
2. write an exact safety snapshot of all stored rows, including unreadable ones, and the raw
   settings value to `files/backup/pre-restore.json` (temp file, fsync, atomic rename);
3. write the journal `files/backup/restore.journal` that references the snapshot id;
4. replace schedules (one Room transaction), simulation (one Room transaction) and settings
   (one DataStore edit);
5. delete the journal; this is the commit point.

A failure in steps 4–5 rolls every store back to the snapshot immediately. If the process dies
after step 3, the next start finds the journal and rolls back before any repository, simulation
or screen can access the stores (the user sees "Obnovení bylo přerušeno"). If the snapshot is
missing or the rollback fails, the app shows a blocked screen with "Zkusit znovu" and does not
continue with partially restored data. The last snapshot stays in `pre-restore.json` until the
next restore.
