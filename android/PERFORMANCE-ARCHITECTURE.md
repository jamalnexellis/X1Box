# Barcelona: selecting an architectural change

Status: source audit and a completed active-race diagnosis, **not an implemented
parallel renderer or an accepted FPS gain**. The reference is the user's rainy
Barcelona race in Slot 1, with the player and moving rivals. Slot 1 is load-only.

The objective is sustained original game speed and 30 FPS, followed by a
validated 3x render resolution (1920x1440). Raising resolution before identifying
the limiting dependency would confound CPU and GPU costs. Original game speed
also needs a game-timer/host-time comparison; frame presentation alone is
insufficient.

## Why another local copy optimization is insufficient

Perf48's byte-preserving image/buffer/image route passes its generated pixel
fixtures, but the Barcelona repetitions do not establish a sustained speedup.
It remains off by default. Several earlier results also mixed the introduction,
countdown and racing. The replacement capture gate requires the active numeric
race timer; saved placeholder screenshots fail this guard.

The existing Perf47 instrumented Barcelona capture provides a ten-second late
window with the following interval unions. These describe this diagnostic,
not an unprofiled comparison:

| Observation | Time in the same 10-second window |
| --- | ---: |
| vCPU scheduled as Running | 7,490.405 ms |
| PFIFO scheduled as Running | 4,486.036 ms |
| Render worker scheduled as Running | 264.743 ms |
| Calibrated GPU dependency intervals | 4,578.732 ms |
| Texture decoding | 26.186 ms |
| Pipeline creation | 4.680 ms |
| Shader compilation / texture image creation | 0 / 0 ms |
| PFIFO Running inside recorded draw-dispatch spans | 1,344.813 ms |
| vCPU Running inside recorded MMIO-read burst spans | 704.824 ms |

The unions overlap. GPU intervals include device dependencies; MMIO bursts
include gaps between reads. Neither is pure occupancy or a proved removable
wait. Fine host phases below the recorder's 50-us threshold are not individually
retained. Summing these rows into a frame budget would be incorrect.

An exclusive interval partition additionally shows 1,045.306 ms with only the
vCPU Running, 570.590 ms with only PFIFO Running, and 2,908.211 ms with both
Running and no observed GPU interval or render-worker execution. These are
four-observation partitions, not whole-device idle measurements. Another
2,521.636 ms has vCPU Running alongside a GPU dependency interval while PFIFO
and the worker are not Running. A busy vCPU can therefore coexist with graphics
dependencies; it does not prove useful game computation.

About 45% of the late vCPU sampled cycle weights remain unresolved executable
addresses in that capture. They must not be assigned to a guest polling loop
or to translation generation without the same process's JIT records.

## What the source audit establishes

PFIFO calls graphics methods under the PGRAPH lock. The Vulkan draw path binds
vertices, prepares shaders/textures/pipelines/descriptors and records commands
there. The existing render worker handles submission, download/display
synchronization, flushing and vertex RAM updates.

The command enum includes DRAW, CLEAR_SURFACE, IMAGE_BLIT and SURFACE_UPDATE,
but the worker switch implements none of those cases. The large
RenderCommandSnapshot function has no callers, and active_snap is never
assigned a draw snapshot. The current code is not an asynchronously prepared
draw renderer. Merely enabling those enum entries would execute no draw.

~~~mermaid
flowchart LR
    C["Xbox CPU translation"] --> F["Guest command stream and fences"]
    F --> P["PFIFO: graphics state, resource preparation, Vulkan recording"]
    P --> W["Render worker: submission and completion"]
    W --> G["Device dependencies and rendering"]
    G --> F
~~~

The feedback arrow represents observed guest/graphics dependencies, not a
claim that every vCPU instruction is waiting on the GPU.

## Diagnosis prepared before selecting a rewrite

Use the already verified Perf48 binary, original copy route, the user's current
DVD/HDD and preferences, and the load-only Barcelona Slot 1. Begin collection
after an active numeric timer is visible, with player and rivals advancing.
Collect one aligned set of:

1. Scheduler states and CPU frequency observations.
2. Native CPU-cycle samples with the exact packaged ELF build ID.
3. Fresh-PID JIT code loads, instruction ranges and load timestamps.
4. Existing PFIFO/worker/submission/fence/presentation dependency identities
   and calibrated device timestamp intervals.
5. Before/after race images and timer readings outside the timed input.

Keep this instrumented diagnosis separate from FPS acceptance. The private
JIT reader refuses overlapping/reused address ranges, incomplete records,
foreign PIDs and samples before code load. Its generated checks pass, and it
also parses the earlier independent 56,609-load diagnostic without reusing
those maps for Barcelona. No opcode is inferred from an unverified game file.
The phone is restored before exporting the bulk samples/JIT data and before
offline analysis.

## Completed active-race diagnosis

The first two attempts are excluded: one never drove off the starting line,
and the second sent its approach input during the introduction. No profile
from either attempt is used. Both restored the saved scene and properties.
The corrected sequence establishes the race HUD first, approaches the starting
line with released input, then requires a numeric lap timer before collection.

The valid diagnostic uses the unchanged Perf48 APK and default shared draw
storage at 1x. Its native library matches the packaged/unstripped ELF build
ID. The 24.255-second input interval contains 274 complete flip intervals,
255 longer than 50 ms, with a median of 88.188 ms. These instrumented intervals
are not an unprofiled FPS baseline. Images show lap 1/3, the player progressing
from 106 to 86 km/h through the corner, advanced rival markers, and a timer
changing from 0:05.623 to 0:18.823. Screenshot timestamps are outside input
bounds; those readings are not a calibrated simulation-speed ratio.

The dependency export has zero dropped records, 179 device clock calibrations
and no rejected device alignments. Scheduling covers the complete input with
owned threads, matching MONOTONIC clocks and no positive trace error/loss
counters. Thermal status is 1 at both ends. CPU sampling contributes 334,455
in-input samples; 50 kernel-space samples were lost and no application-space
samples were lost. Fresh JIT files contain 56,985 non-overlapping code loads
and 361,119 owned instruction ranges.

Within vCPU sampled cycle weights, 40.471% resolve to generated guest
instruction execution, 51.151% to emulator native code and 1.535% remain
unresolved executable addresses. Generated code is execution, not translation
construction or automatically useful guest work. The remaining shares include
other native modules and unresolved native ELF code.

**The first architectural target is draw-storage retirement.** There are 150
synchronous completion waits whose batch reason is NEED_BUFFER_SPACE. They
total 6,546.060 ms inside the input, with median 43.552 ms and maximum 46.475 ms.
Recovered native call stacks identify the capacity finish in the uniform
allocation branch of pgraph_vk_update_descriptor_sets (shaders.c:747), not
merely a generic driver wait. Runtime allocations confirm the 8-MiB uniform
capacity. Sampling does not independently type every capacity event, but this
is the only recovered capacity-finish caller in the sample set.

In the last ten seconds, PFIFO explicit wait intervals have a union of
3,561.017 ms, while vCPU Running overlaps them for 2,844.246 ms. Device
dependency intervals have a union of 5,233.356 ms. The largest linked
completion waits follow their own submitted batch's device intervals, rather
than a long host submission or a recovered queue of older batches. These
observations identify a concrete synchronization path; they do not prove that
all its device work disappears when the host waits are avoided.

Keep the mobile submission-size bound: it protects against GPU watchdog
failures. Separate that bound from ownership/reuse of uploaded draw data.
The existing opt-in per-slot GPU draw-storage route gives each active slot
distinct index/vertex/uniform destinations and retains its fence retirement.
It can submit capacity-limited batches without immediately recycling the
shared destination. The next experiment compares shared storage with that
ownership route in the same load-only Barcelona scene, leaving the 8-MiB
capacity and guest work intact. Generated pixel validation of that route is
already recorded in the Perf45 investigation; Barcelona performance and
whole-race compatibility still need their own evidence.

The valid diagnosis restored Slot 1, all existing snapshot sidecars,
preference values and affected diagnostic properties before offline analysis.

The subsequent protected eight-minute comparison is incomplete. Its first
attempt stops at thermal status 2 before measurement. After passive cooling
with emulation stopped, one shared-storage moving start gives a descriptive
13.282 FPS, but thermal status changes from 1 to 2. That run is excluded from
a matched performance comparison. The independent-storage compatibility probe
is refused by the remaining-time reserve, before changing its mode. No ON
result, completed ABBA, accepted FPS gain or new default is established.
The phone is restored again; the comparison harness remains prepared.

### Implemented uniform ownership correction (Perf49)

Perf49 implements the narrower uniform-only ownership change. Each active
slot gets a device-local uniform destination with the existing byte capacity;
index/vertex ordering and the old submission dependency remain intact.
Uniform-capacity finish reason 9 can defer FIFO completion until fenced slot
reuse. Descriptor rebinding includes uniform destination changes, and the
production transfer barrier now covers both shader stages that read uniforms.
Generated host and Adreno pixel checks pass all 393,216 comparisons.

The completed unprofiled 1x Barcelona pair gives 13.154 FPS ON and 13.393 FPS
OFF with thermal status 1 throughout both runs. Later windows give 11.776 and
11.573 FPS. Controls, numeric timer progression and advancing rival markers
are observed; reviewed images are coherent. Different end positions/speeds
limit comparison, and no sustained improvement is accepted. All original
preferences, affected runtime properties and snapshot sidecars are restored;
Slot 1 is never saved over. Full HD remains unmeasured in this pair.

Removing an immediate host wait has not produced an appreciable throughput
gain here. The next attribution must separate reason-9 completion waits from
slot-recycling waits and the retained shared-resource/GPU dependency chain,
using Perf49's actual native build. The current pair alone does not show
where the former wait moved or establish a new dominant bottleneck.

## Architectural decision gates

**If useful translated execution dominates the delayed frames:** identify
the hottest instruction sequences and their memory/FP helpers first. Develop
a generic translation path that retains architectural register state over
those sequences and uses appropriate ARM operations, with exact reference
results and exception/MMU/interrupt handling. Reject a change whose maximum
affected part of the measured critical path cannot explain a meaningful
improvement. Existing direct block chaining and FP paths must be audited
before adding an overlapping mechanism.

**If the guest mostly waits for graphics preparation:** investigate a
producer/consumer draw path with immutable state packets and resource versions.
PFIFO owns guest-visible state and publishes ordered packets; a worker owns
renderer caches, descriptor resources and command-buffer recording. Keep draw
order, byte reinterpretation, upload/readback coherence and guest fence effects
exact. Use bounded backpressure and retire resources only after their own
completion. Do not put borrowed guest RAM pointers or mutable PGRAPH fields
into a delayed command, and do not copy the full register/shader/lighting
snapshot for every draw without measuring its bandwidth and allocation cost.
Moving the same serial work to a different thread alone is not a speedup.

**If device execution/dependencies dominate:** identify the responsible
command-buffer/resource chain and its render-pass/copy transitions. Rework
resource ownership or pass construction only with a correctness proof for
all prior readers/writers. A different copy command cannot by itself remove
those dependencies. Direct surface views also have a documented mobile-driver
lifetime workaround in the current code; forcing the old path is not a
resource-ownership redesign.

Choose the first prototype from the Barcelona attribution, not from which
flag is easiest to toggle. Repeat the same moving race at 1x and 3x after a
causal target exists; validate pixels, controls, game-timer progress, thermal
state and repeated race trajectories. No game ID, skipped draw, removed
barrier or guessed guest polling shortcut is part of this design.

Raw profiles, translated/game code, device addresses, game assets and local
phone harnesses remain private. Build/test work stays local; Judge and CI/CD
remain disabled.
