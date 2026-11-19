# Firmware simulator ThreadSanitizer driver (test-only)

`fwsim_tsan` runs the connection lifecycle of the firmware-in-the-loop simulator
(`../fwsim.c`, the source of `libdsp_fwsim`) natively under **ThreadSanitizer**. Inside the JVM
the simulator cannot be instrumented, so this driver compiles `fwsim.c` directly with a minimal
fake `JNIEnv` and calls its JNI entry points from plain POSIX threads, exactly as the Android
test harness (`FirmwareSim` / `FirmwareGattPort`) does.

**Test-only.** It is never part of a firmware image, the APK, the Gradle build or the host CTest
suite (that suite uses ASan/UBSan, which cannot be combined with TSan). It links the same
**unmodified** firmware sources as `libdsp_fwsim`: `dsp_app`, `dsp_blesec`, `dsp_ble_transport`,
the domain, noise-c and libsodium, in the host harness world (fake storage, pump and clocks; the
deterministic test RNG stays confined to host tests).

## Run

```sh
cd firmware
DSP_JDK_HOME=<jdk> test/fwsim/tsan/run.sh            # 200 rounds, 3 repeats
DSP_JDK_HOME=<jdk> test/fwsim/tsan/run.sh 500 5      # rounds, repeats
```

- Needs CMake ≥ 3.22, a C11 compiler with the TSan runtime (GCC or Clang on Linux), and a JDK
  only for `jni.h` (`DSP_JDK_HOME`; without it CMake's `FindJNI` is used). The Gradle-provisioned
  test JDK works, e.g. `~/.gradle/jdks/eclipse_adoptium-25-amd64-linux.*`.
- The build goes to `firmware/build-fwsim-tsan/` (git-ignored; override with `BUILD_DIR`). It is
  kept out of `test/fwsim/`, which the Gradle `buildFirmwareSim` task hashes as its input.
- The binary runs under `setarch $(uname -m) -R` (no address randomization): on recent kernels the
  TSan runtime otherwise aborts with "unexpected memory mapping".
- `TSAN_OPTIONS=halt_on_error=1`: the first data race aborts the run with a non-zero status.
  A functional check failure or an incomplete round also exits non-zero.
- Expected output: `rounds 200/200, stale discarded N, failures 0` per repeat (N is usually 0 or
  1, see below), then `fwsim_tsan: 3 x 200 rounds, 0 ThreadSanitizer reports, 0 failures`.

## Reused connection handle

Real Bluetooth controllers reuse connection handles after a disconnection, so the simulator does
too: every connection gets the same handle, `FS_HANDLE_DEFAULT` (0x0001). The driver asserts this
on every round. Correctness never depends on unique handles. The harness binds queued events,
writes and phone-side disconnects to a separate, never-reused **link id** (`connect` returns
`link << 16 | handle`), and the firmware itself sees only the reused handle, so its own
stale-event guard in `dsp_ble_transport` is exercised on every reconnect.

## Concurrency scenarios (per round, all on the one reused handle)

| Thread | What it does |
|---|---|
| firmware application task | the simulator's own thread: `dsp_app` + `dsp_blesec` + the transport, virtual clock |
| left-over poller (previous link) | keeps calling `poll(previousLink)` after its link ended, across the reconnect, until it has seen `GONE` |
| late caller (previous link) | 20 × `write(previousLink)` + `disconnect(previousLink)` racing the new `connect` |
| injector (current link) | pushes 40 numbered notifications on the current link |
| main thread | `connect`, then polls the current link for exactly those 40 notifications |
| garbage writer (current link) | 8 malformed fragments through the real firmware transport (its fault/close path on the application task) |
| main thread | phone-side `disconnect(currentLink)`, racing the garbage writer |

The current link's poller is then left running into the next round as that round's left-over
poller, so every reconnect happens while the previous poller is still polling.

Checks (failures counted, reported at the end):

- the new connection uses the reused handle and a strictly newer link id;
- the current link receives all 40 notifications, in order, each tagged with its own link: none
  lost, duplicated or reordered;
- the left-over poller receives `GONE` and **never** an event of another link;
- every late `write` of the previous link is refused (`-2`); late `disconnect`s are no-ops (otherwise the current
  link, polled concurrently, would end early and lose notifications);
- after all rounds, a day of virtual time and a wake: no firmware `DSP_CHECK` failures, no dropped
  radio events, the security layer is idle and wiped.

"stale discarded" counts unread events of an ended link that a later poll threw away (for
example a `DISCONNECTED` event nobody polled before the reconnect). Those events are dropped,
never delivered to the new link.

## What it found

In M1.10e.3 it reported a data race on the simulator's virtual clock: the transport's `now_ms`,
read on a phone-write (host-task role) thread, raced the application task's clock update. The
harness now gives host-task-role calls the clock copy published under the simulator lock
(`fs_host_role` in `fwsim.c`). On the device the clock is the thread-safe hardware timer, so no
firmware changed. Re-introducing the race (`fr_now` always reading `mono_ms`) makes this driver
report it at `fr_now`/`fr_wait` and exit non-zero.
