# Bounded dependency tracing

Perf44 adds an Android-only diagnostic mode. It changes no guest instructions,
rendering algorithms, controller mappings or synchronization decisions. The
mode is off by default. It is for attributing stalls, not demonstrating an FPS
improvement.

## Capture protocol

Set `debug.xemu.lag.trace=1` before launching the emulator and leave
`debug.xemu.lag.capture=0` while loading the game, state and race. The native
launcher selects an internal `files/lag-trace` directory. With the race ready,
set `debug.xemu.lag.capture` to a fresh positive decimal token (up to UINT32_MAX).
Wait for `xemu-lag: lag_trace_begin token=...` before starting controlled input.
Stop with `debug.xemu.lag.capture=0`. Each capture also expires after 30 seconds.
Do not stop the process before the `lag_trace_end ... written=1` acknowledgement.

The file is `files/lag-trace/lag-<emulator-pid>-<token>.csv`. Export it through
`adb exec-out run-as <test-package> cat <relative-path>`, preserving bytes, and
verify its SHA-256 against the on-device file. Tokens and the exclusive file
creation mode prevent silent overwrite. Logs, game register values, JIT maps,
driver caches, images and scheduler traces remain private.

Example analysis:

```text
python android/tools/analyze-lag-trace.py capture.csv --output report.json --timeline timeline.json --begin-us <native-input-begin> --end-us <native-input-end>
```

The timeline uses the Chrome trace event format for inspection in Perfetto.
The report ranks intervals above the 33.334 ms budget for 30 FPS and separately
counts intervals above 50 ms. These are instrumented intervals, not an accepted
unprofiled FPS result.

## Recorded observations

- Flip boundaries and unique host command-buffer batch identities; slot numbers
  alone are not identities because a slot is reused.
- Eighteen CPU preparation stages: surfaces, textures, shader work, vertex
  preparation/synchronization, primitive assembly, pipeline/descriptor setup,
  command recording and finish paths.
- Existing texture hashing/decoding/staging/allocation/reuse/capacity phases;
  shader loading, preprocessing, parsing, linking, SPIR-V and pipeline creation.
  Valid pipeline feedback distinguishes a reported cache hit from unavailable
  feedback.
- Render/compiler enqueue and dequeue, worker context and queue residence;
  submission acknowledgement, completion waits and renderer idle waits.
- Every existing Vulkan fence-wait call in the renderer. A known frame or
  presentation fence links to the corresponding submission identity. Unknown
  fences retain an explicit missing link instead of an inferred producer.
- Per-submission GPU timestamp intervals for the main and auxiliary command
  buffers, selected-queue valid bits, timestamp period and query results.
- Optional EXT calibrated timestamps using DEVICE and CLOCK_MONOTONIC domains,
  including the reported maximum deviation. Unsupported calibration leaves
  relative GPU durations available without claiming an absolute GPU timeline.
- PFIFO running/idle intervals and observed reasons: flip, NOP, context switch,
  halt and empty DMA queue. These are condition snapshots, not guaranteed wake
  causes.
- PGRAPH/PFIFO register reads grouped into repeated-read bursts, register/value
  transitions, read-lock waits, PGRAPH fence publication and VBlank observations.
  Repeated reads sample the clock every 64 reads and flush roughly every 1 ms;
  a burst includes gaps between reads and is not a CPU execution-time measure.
- Presentation submission and `vkQueuePresentKHR` result/host interval.

From perf46, worker `submit` events encode `c` as flags: bit 0 is deferred
completion and bit 1 means isolated auxiliary uploads wait on the previous
submission chain at the main draw instead of the auxiliary buffer. Inline
render-context submits retain their legacy zero `c` field. Queue timing alone
cannot infer which dependency path was selected.

Most detailed CPU phases below 50 us are filtered. Boundary, identity and
dependency records are retained even when short. Repeated small costs can be
investigated with the separate simpleperf/JIT diagnostic rather than treating
the detailed-event threshold as proof that they are negligible. Guest polling
on ordinary RAM is not intercepted by the NV2A register logger.

## Integrity and observer cost

Producers append to a fixed 262,144-record buffer, approximately 20 MiB, under
a short mutex. Pages are touched before the arming acknowledgement. There is
no per-event file or Android log write in the producer path. A detached exporter
writes the snapshot after recording stops. Overflow is counted without wrapping
over earlier events. The analyzer rejects missing footers, lost records,
sequence/count mismatches, invalid bounds and inconsistent timestamp metadata.

The diagnostic still has observer cost: clocks, mutexes, GPU queries, optional
calibration and scheduler/sampling tools. Bracket diagnostic runs with captures
off at the same resolution/cache settings. Keep both diagnostic and unprofiled
runs separate. Capture screenshots outside the timed driving interval.

GPU timestamp intervals include execution dependencies and pipeline overlap;
they are not pure GPU busy time. Follow [Khronos timestamp semantics](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html)
and the selected queue's [timestamp valid bits](https://docs.vulkan.org/refpages/latest/refpages/source/VkQueueFamilyProperties.html).
The analyzer uses unsigned modular arithmetic, includes calibration/period
encoding uncertainty, and suppresses absolute GPU intervals that contradict
the observed submission/readback bounds. Never add nested or cross-thread
intervals into a frame budget. The report identifies observations and missing
links; it does not automatically declare a proven root cause.

## Moving-race protocol

The private phone harness is prepared for: an unprofiled 3x reference, a warm
3x diagnostic, a warm 1x diagnostic, a cold-resource 3x diagnostic, a repeated
warm 3x diagnostic and another unprofiled reference. An optional additional
case collects CPU samples and JIT mappings. Each case restarts the race and
uses native throttle/steering/release markers. Driving and rivals must remain
visible in the pre/post captures; different trajectories or contact limit
resolution/cache comparisons and must not be called exact scene equivalence.

The scheduler trace correlates thread states with the same slow-frame bounds
and requests CPU frequency/idle events where available. Absence of frequency
events means unavailable telemetry, not a fixed frequency. Trace clock and
data-loss diagnostics must be checked before attribution.

Scheduler integrity needs an explicit coverage check as well as loss counters.
A ring buffer can overwrite the beginning without reporting a trace error.
After translating Perfetto timestamps through its MONOTONIC clock snapshots,
require trace bounds to contain both native driving markers. Exclude uncovered
frames and label partial traces explicitly. The initial 32 MiB/40-second phone
capture retained only the tail; later 128 MiB/40-second captures contained the
entire driving interval. Buffer size alone is not evidence of completeness.

Perfetto can retain thread scheduling while omitting a worker's process mapping.
An empty `JOIN process ... WHERE pid=...` result does not mean that worker was
idle. Anchor thread IDs to contemporaneous `/proc/<emulator-pid>/task` lists,
check ownership before and after the capture, and restrict attribution to the
verified process lifetime. Identify vCPU/PFIFO/render roles from recorder events
instead of assigning roles from the shared `qemu_main` thread name.

For a long completion wait, follow its batch ID through dequeue and submission,
then compare submit return with calibrated auxiliary/main GPU timestamps. Check
which earlier GPU intervals overlap the pre-start gap. Their overlap supports
queued dependencies; it is not a measurement of pure GPU occupancy. Correlate
the same interval with thread states and CPU samples: a sleeping producer and
a running vCPU can coexist, and vCPU polling is not useful simulation progress.
Ordinary-RAM polling still requires separate JIT samples; NV2A MMIO logging
does not identify its producer or justify skipping that loop.

Each phone session uses a fresh protected backup and a bounded deadline,
checks APK identity and focus, stops on interruption, preserves exact helper
errors, and restores its own data/preferences/EEPROM and all 29 diagnostic
properties. Pending cases remain pending if the restoration reserve is reached.
Perf45--47 add opt-in `debug.xemu.vk.direct_upload` (0/1/2) and
`debug.xemu.pgraph.status_read` (0/1). They select implementation experiments,
separately from capture controls. Hold all other settings/cache seeds fixed;
do not treat their default-off builds as evidence that an experiment is faster.
Record thermal status at start/end, stop between cases when it changes, and
exclude thermally mismatched cases from a paired FPS claim. A status change
alone does not attribute a specific slow frame to throttling.
No clocks, governors, root/kernel settings or security restrictions are changed.
All builds and checks are local; CI/CD remains disabled.
