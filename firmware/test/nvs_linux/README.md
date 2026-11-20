# Domain storage over the real ESP-IDF NVS (linux target)

Capacity and power-loss checks: `dsp_storage` / `dsp_schedule_repo` / codec over the firmware's
NVS adapter (`components/dispenser_platform/src/dsp_nvs_kv.c`, unchanged) and the **real ESP-IDF
v6.1 `nvs_flash`** - by default the project's patched copy (`firmware/third_party/nvs_flash`),
with `NVS_UNPATCHED=1` ESP-IDF's original component as negative control - on ESP-IDF's emulated
SPI flash (linux target) with the committed `partitions.csv` (not modified). Not part of CTest
(needs ESP-IDF).

```sh
firmware/test/nvs_linux/run_checks.sh            # checks 1-5 + compatibility, both components
firmware/test/nvs_linux/run_checks.sh --matrix   # + two-cut matrices (about 30-40 minutes)
```

`run_checks.sh` requires the patched component to pass everything AND the unpatched component
to still reproduce the known defects (negative control); see its header. Modes of the program
(`MODE`): unset = checks 1-5 and 11, `matrix` = 6-8, `dump`/`load` = 9 (images exchanged between
the components), `ab` = 10 (redundant-record alternative, kept for comparison).

**Check 11 (M1.10b.3, layout v5):** the v4 → v5 migration of a populated device (maximum-size
schedule set, no `auth`, v4 marker) and two `auth` record writes (provisioned → claimed, claimed →
two phones), each cut at every flash write/erase cycle. Patched component (2026-11-17): migration
153 cycles, 154 cuts, every reopen as v5 with the provisioned `auth` and every other key intact;
`auth` writes 127 cycles, 128 cuts each, always exactly the old (123) or the new (5) record. The
unpatched component loses the record at cycle 115 of each `auth` write (the known defect, also a
negative control). Check 1 with layout v5: dispenser namespace 324 entries (budget 325).

**Phase C capacity check (2026-11-20, review revision with the sequence reservation `evhw`):** check 1
fills the event history to its maximum (64 events with 64-character schedule IDs, twice round the
ring) before measuring, so checks 1, 2 and 4 run with the worst case; check 2b appends 20,000
maximum-size events (312 wraparounds) with a power cycle and reopen every 2,000. Patched component:
namespace 711 entries (layout 324 + history 387 incl. reservation; budget asserted); partition 2,520,
used 849, available 1,545; check 2: 16,364 page erases (0.818 per set write, unchanged), per page max
1,488 (1,023 without the log: more live data moved during reclamation); check 2b: 0.071 page erases
per append, per page max 110, used entries constant, `seq` strictly increasing across every reopen;
check 4 steady-state headroom 140, margin 1,405. Whole run: patched 681,774 checks, 0 failed (the
count of check 4 depends on the fragmentation state the earlier checks leave: 5,601 cut points here,
9,036 in the previous run); unpatched still reproduces the known defect; image exchange passes.

Prerequisite: the host's libbsd development headers (`libbsd-dev`), which ESP-IDF's linux target
needs. Without root they can be unpacked locally (`apt-get download libbsd-dev && dpkg-deb -x
libbsd-dev_*.deb <dir>`) and passed with `CPATH=<dir>/usr/include/x86_64-linux-gnu
LIBRARY_PATH=<dir>/usr/lib/x86_64-linux-gnu`.

Notes:

- **Table offset:** ESP-IDF v6.1's flash emulation reads the partition table correctly only
  below the 64 KiB MMU page, so the emulated table copy sits at 0x8000 (the firmware uses
  0x10000). The partition entries have explicit offsets; the program checks that `nvs` is
  0x14000 bytes at 0x11000, as on the device.
- **Power-off model:** link-time wrappers around `esp_partition_write(_raw)` and
  `esp_partition_erase_range`: at the cut the write is truncated at 4-byte granularity (an
  interrupted erase leaves its sector untouched) and every later flash modification fails;
  NVS is then deinitialized and re-initialized. ESP-IDF's own `esp_partition_fail_after()` lets
  later calls of the same operation succeed; it shows the same defect (see below).
- **Finding (ESP-IDF NVS defect, espressif/esp-idf#19212):** see the header comment of
  `main/nvs_capacity.c`. With the UNPATCHED component checks 3 (format 1 → 2 upgrade) and 5 (plain
  `nvs_set_blob`) fail at exactly one cut point per write, and the matrices lose keys; with the
  patched component everything passes. Do not skip or relax the checks: they are the regression
  gate of the patches (`firmware/third_party/nvs_flash/README.md`).

## Results of the unpatched component (2026-11-16, ESP-IDF v6.1 `fff9895c`, precise power-off model)

| Check | Result |
|---|---|
| 1. maximum layout | `sched` 3,160 B; dispenser namespace 304 entries; with PHY/Wi-Fi co-tenants 2,520 total, 442 used, 1,952 available (empty partition: 2,393) |
| 2. 20,000 maximum-size `sched` writes + 2,000 `config` writes | 16,364 page erases (0.818 per write), per page min 0 / max 1,023; used entries stable at 442 |
| 3. format 1 → 2 upgrade, cut at every cycle (846 cuts) | **FAIL:** cut at cycle 251 of 845 → `sched` missing (`ESP_ERR_NVS_NOT_FOUND`), storage open → CORRUPT; other 845 cuts: old set 836, new set 9 |
| 4. low free space | full partition: write refused (NO_SPACE), old set intact; steady state needs about 140 available entries; cut at every cycle of a write at that headroom: 6,745 cuts, all old (6,734) or new (11) |
| 5. plain `nvs_set_blob` A → B, cut at every cycle | **FAIL: exactly 1 lost key per scenario, 14 of 14 scenarios** (2504→3160, 3160→3160, 3160→2504, 100→3160, 2504→2504, 1000→2000, 32→64 B; fresh and aged partition) |
| total | 702,924 checks, 3 failed (all from the defect) |

Minimal reproducer: 32 → 64 B blob, cut at cycle 26 of 37 (the state-bitmap write that marks the
new data chunk WRITTEN; the new blob index at cycles 27–35 is not written). Page dump before
recovery: old chunk (entry 1–2, chunk 0x00), old index (entry 3), new chunk (entries 4–6, chunk
0x80) all WRITTEN. During `nvs_flash_init_partition()` NVS purges entries 1–2 first (old chunk,
`Page::load` "check that last item is not duplicate" looks up the last item without its chunk
index), then entry 3 (old index, `eraseMismatchedBlobIndexes`), then 4–6 (`eraseOrphanDataBlobs`).
