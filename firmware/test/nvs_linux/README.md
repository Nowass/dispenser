# Domain storage over the real ESP-IDF NVS (linux target)

M1.10b.1 capacity and power-loss check: `dsp_storage` / `dsp_schedule_repo` / codec over the
firmware's NVS adapter (`components/dispenser_platform/src/dsp_nvs_kv.c`, unchanged) and the
**real ESP-IDF v6.1 `nvs_flash`**, on ESP-IDF's emulated SPI flash (linux target) with the
committed `partitions.csv` (not modified). Not part of CTest (needs ESP-IDF); run manually.

```sh
. $HOME/.espressif/tools/activate_idf_v6.1.sh
cd firmware/test/nvs_linux
idf.py -B /tmp/dsp-nvs-linux build
/tmp/dsp-nvs-linux/dsp_nvs_linux.elf
```

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
- **Finding (ESP-IDF NVS defect, UNRESOLVED, release and hardware-acceptance blocker):** see
  the header comment of `main/nvs_capacity.c`. Checks 3 (format 1 → 2 upgrade) and 5 (plain
  `nvs_set_blob`) fail at exactly one cut point per write. The program therefore exits non-zero;
  this is the expected, documented state until the defect is fixed. Do not skip or relax the
  checks.

## Results (2026-11-16, ESP-IDF v6.1 `fff9895c`, precise power-off model)

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
