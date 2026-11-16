# ESP-IDF `nvs_flash` with power-loss fixes (project-scoped)

The firmware stores every record as an NVS blob. ESP-IDF v6.1's NVS can lose a key on a reset
at specific points of an update, which the dispenser storage then has to treat as corruption
(fail closed: no automatic dosing until maintenance). This directory fixes that **without
modifying the ESP-IDF installation**: at configure time a verified copy of ESP-IDF's
`nvs_flash` component is patched in the build directory and overrides the ESP-IDF component.

## Pinned upstream

| Item | Value |
|---|---|
| ESP-IDF | v6.1, commit `fff9895c82d744c7237be8847347bdd1b07c6643` |
| Component | `components/nvs_flash` |
| Source manifest (SHA-256 over `<sha256>  <path>` of `CMakeLists.txt`, `Kconfig`, `project_include.cmake`, `include/`, `private_include/`, `src/`; 48 files) | `42454d481da5127019a290af2d2c79a554764ad120c6e96f2b06bc3330db25ea` |
| `src/nvs_page.cpp` before / after the patches | `e77a3cb5…fd49` / `a159b269…9813` (full values in `nvs_flash_override.cmake`) |

`nvs_flash_override.cmake` refuses to configure when the ESP-IDF git commit (if `IDF_PATH` is a
checkout) or the source manifest differs, when a patch does not apply with zero fuzz, or when
the patched file's hash differs. Requirements: GNU `patch`.

## Patches (applied in order; each touches only `src/nvs_page.cpp`, `Page::mLoadEntryTable()`)

| Patch | Upstream issue | Defect | Change |
|---|---|---|---|
| `0001-last-item-duplicate-check-uses-chunk-index.patch` | [espressif/esp-idf#19212](https://github.com/espressif/esp-idf/issues/19212) (IDFGH-18441) | a reset after the new blob version's last data chunk and before its blob index: the next init erases the old version's chunk as a "duplicate" of the new one (lookup without chunk index), then the old index and the orphaned new chunks: **the key is lost** | pass `item.chunkIndex` to the last-item duplicate lookup (as `PageManager::load()` does) |
| `0002-verify-hash-match-before-duplicate-erase.patch` | [espressif/esp-idf#19213](https://github.com/espressif/esp-idf/issues/19213) (IDFGH-18442) | page load erases an earlier item whose 24-bit hash merely collides with a later one (no power cut needed) | erase only a candidate whose namespace, chunk index and key really match |
| `0003-half-written-entry-check-reads-every-word.patch` | [espressif/esp-idf#19214](https://github.com/espressif/esp-idf/issues/19214) (IDFGH-18475) | a cut write that left an entry's first word erased and later words programmed is taken as free; the next write there is silently lost (with the previous value of an updated key) | check every word of the entry, not only the first |

None of them changes the on-flash format or any write: they change only recovery decisions at
`nvs_flash_init()`. Images written by the patched and the unpatched component are byte-identical
for the same operations, and each reads the other's partitions (`test/nvs_linux`).

Upstream status (checked 2026-11-16): all three issues open, assigned, no maintainer response;
ESP-IDF master still has the defective lookup. The #19212 report traces the defect to commit
`cad09ea4619` (v5.3, 2024) and its backports (v4.4.8, v5.0.7, v5.1.4, v5.2.2).

## Use

- Firmware: `firmware/CMakeLists.txt` calls `dsp_nvs_flash_override()` before `project()` and
  `dsp_nvs_flash_check_selected()` after it (configuration fails if `nvs_flash` resolves to any
  other directory). Every configuration (default, diagnostics, application runtime) uses it.
- `firmware/test/nvs_linux`: patched by default; `NVS_UNPATCHED=1` builds the negative control.
- Standalone: `cmake -DDST=<dir> -P nvs_flash_override.cmake` prints `<dir>/nvs_flash`.

## Verification

| What | How | Result (2026-11-16) |
|---|---|---|
| ESP-IDF's own NVS host suite (128 test cases, 322,676 assertions, Catch2) | `tools/run_upstream_suite.sh` (work copy + mirror root; ESP-IDF untouched) | patched and unpatched: all pass, two clean runs each (see notes) |
| Dispenser storage + plain NVS, single cuts, format upgrade, low space | `test/nvs_linux/run_checks.sh` | patched passes; unpatched reproduces the defect (negative control) |
| Two-cut matrices (cut at every write cycle, then at every recovery cycle; prefix and "first word erased" tear models) | `run_checks.sh --matrix` | patched: 0 losses; unpatched: losses (negative control) |
| Compatibility | `run_checks.sh` (dump/load) | identical images, cross-readable both ways |

Notes on ESP-IDF's suite (properties of the upstream TESTS, not of the patches):

- **Uninitialized test variable:** "nvs multiple write with same key blob and string involved"
  passes an uninitialized `size_t blob_read_size` as the in/out buffer length (also noted in
  #19212), so it passes or fails depending on stack contents. `run_upstream_suite.sh` initializes
  it to `blob_data_len` **in its work copy only** (it fails if the line is not found); the
  ESP-IDF installation keeps the original test.
- **Order-dependent test (unresolved upstream test-harness observation):** Catch2 v3 runs the
  test cases in random order (new seed per run, printed as "Randomness seeded to: N" and recorded
  by the runner). "Check for nvs version incompatibility" leaves an older-version page in the
  shared emulated `nvs` partition, and "NVSHandleSimple CXX api open invalid arguments"
  (`test_nvs_cxx_api.cpp:28`) initializes `nvs` without erasing it. When a random order puts the
  second right after the first (with no erasing test between), `nvs_flash_init_partition("nvs")`
  correctly returns `ESP_ERR_NVS_NEW_VERSION_FOUND` (0x1110). Observed once (patched build, seed
  not recorded at the time) in about 30 full runs; reproduced deterministically on BOTH the
  patched and the unpatched component with
  `nvs_host_test.elf --order decl "Check for nvs version incompatibility,NVSHandleSimple CXX api open invalid arguments"`;
  each test alone passes. Every later full run passed on both builds (21 repeated runs, then two
  runs from independently initialized clean work directories, 2026-11-16). Not a patch defect;
  not worked around (the random order is kept); a failure of exactly this test must be replayed
  with its seed before it is attributed to the code.

## Lifecycle

- **ESP-IDF update:** the configuration fails until this directory is re-pinned. Check each
  issue above: if upstream fixed it, remove that patch (below); otherwise rebase the patch onto the
  new sources. Update the commit, manifest and hashes in `nvs_flash_override.cmake`, then run
  `tools/run_upstream_suite.sh` and `test/nvs_linux/run_checks.sh --matrix`.
- **Removing a patch after an upstream fix:** delete its file, remove it from `DSP_NVS_PATCHES`,
  update `DSP_NVS_PATCHED_PAGE_SHA256` (the hash of the file after the remaining patches) and the
  table above; keep the reproductions: `run_checks.sh` must then show the ESP-IDF component
  itself passing the corresponding checks. For 0001 the "unpatched reproduces" expectation in
  `run_checks.sh` changes to "passes" for the new ESP-IDF version.
- **Removing the whole override:** when ESP-IDF contains all three fixes, delete this directory
  and the calls in `firmware/CMakeLists.txt` and `test/nvs_linux/CMakeLists.txt`; the negative
  control then becomes a plain regression check.
- **License:** the patched files are ESP-IDF's (Apache-2.0); only the patches live here, the
  sources are copied from the local ESP-IDF installation at build time.
