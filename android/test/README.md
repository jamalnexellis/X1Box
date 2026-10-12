# Generated Vulkan pixel checks

These native fixtures use generated pixels only. They do not need a game,
firmware, HDD, saved state or emulator installation. Passing them checks the
shared copy/packing primitives and resource lifetime; it does not measure FPS
or establish compatibility with an entire game.

From the repository root on a Linux host with a C compiler, Vulkan development
headers/loader, `glslangValidator` and the Khronos validation layer installed:

```sh
task_dir=$(mktemp -d)
cc -I . -Wall -Wextra -Werror android/test/cpp/emit-depth-stencil-pack.c \
  -o "$task_dir/emit-pack"
"$task_dir/emit-pack" > "$task_dir/pack.comp"
glslangValidator -V "$task_dir/pack.comp" -o "$task_dir/pack.spv"
glslangValidator -V android/test/cpp/test-depth-view.comp \
  -o "$task_dir/depth-view.spv"
cc -I . -UNDEBUG -Wall -Wextra -Werror \
  android/test/cpp/test-cubemap-copy-vk.c -lvulkan \
  -o "$task_dir/copy-check"
XEMU_TEST_VALIDATE=1 "$task_dir/copy-check" \
  "$task_dir/pack.spv" "$task_dir/depth-view.spv"
```

The first optional argument enables D24/S8 integer-pack checks, including
three queued versions with earlier destination reads. The second enables
D24/D32 sampled-view reuse, ownership transfer, queued sampling and retirement
checks. With no arguments the earlier cube/color/D16 fixtures still run.
The device must support the formats exercised by the selected fixtures.

The buffer fixture also exercises the shared exact vertex-upload comparison
before and after queue submission. Three generated versions retain their
earlier readback bytes, and changed bytes take the completion-before-overwrite
fallback. This checks buffer byte preservation and ordering; it does not
exercise the full emulator's vertex fetch or measure a game.

The separate guarded CPU fixture detects a change at every byte of a 64 KiB
range, checks page/end boundaries, rejects unuploaded and oversized ranges,
and compares read-only mappings without writing to them:

```sh
cc -I . -UNDEBUG -Wall -Wextra -Werror -fsanitize=address,undefined \
  android/test/cpp/test-vertex-upload.c -o "$task_dir/vertex-check"
"$task_dir/vertex-check"
```

For Android, build the existing `test-cubemap-copy-vk` native target with the
NDK and pass the two generated SPIR-V files as arguments to that binary.
Synchronization validation requires a validation layer on the executing
device; generated-pixel checks can also run without that layer.

# Native presentation reuse policy

The CPU fixture checks the shared presentation decision with a C compiler and
Vulkan headers installed; it requires no Vulkan device or loader linkage:

```sh
cc -I . -UNDEBUG -Wall -Wextra -Werror -fsanitize=address,undefined \
  android/test/cpp/test-present-reuse.c -o "$task_dir/present-check"
"$task_dir/present-check"
```

It checks offscreen binding churn, recycled Vulkan handles with a fresh binding
identity, counter exhaustion, clock extremes, pending uploads, video, captures,
changed content/geometry and the bounded refresh deadline. The Android native
target is `test-present-reuse`. This fixture checks policy; it does not exercise
an actual swapchain, measure FPS or establish full-game graphics correctness.
It also checks that completed suboptimal presentations can commit a cache key,
while the tested errors and other positive status codes discard it.

In the emulator, the opt-in `debug.xemu.vk.present_reuse=1` must be set before
launch. With `debug.xemu.vk.stall_profile=1`, `XEMU_PRESENT_REUSE` logs totals
over approximately five seconds. Rejection-reason counts can overlap and must
not be summed as a frame budget. Use separate profiling-off driving runs to
measure FPS and compare reviewed images before accepting a performance change.
The first eight profiled native present results are also logged as
`XEMU_PRESENT_RESULT` for checking the driver's actual return status.

# Per-slot vertex RAM propagation

The generated CPU fixture reproduces the former three-slot hand-off defect:
copying only the current slot's new range misses changes from an earlier slot.
The shared replacement tracks pending pages separately for each destination.
Both bounding-range and sparse-run copies are checked against an independent
fully updated byte oracle across 12000 rotations with one, two and three slots,
idle turns, overlapping writes, distant pages and a partial final page:

```sh
cc -D_GNU_SOURCE -I . -UNDEBUG -Wall -Wextra -Werror \
  -fsanitize=address,undefined android/test/cpp/test-vertex-propagation.c \
  -o "$task_dir/vertex-propagation-check"
"$task_dir/vertex-propagation-check"
```

The Android native target is `test-vertex-propagation`. The existing Vulkan
copy fixture additionally checks 24 generated GPU readback versions for each
slot count and each copy mode, waiting for the destination's prior readers
before updating it. Prior readbacks must retain their earlier bytes. These
transfer reads test byte preservation and ordering, not full vertex fetch or
game rendering. Host validation layers and generated fixtures do not establish
device compatibility or an FPS gain.

The emulator always accumulates changes per destination. Its conservative
default copies the bounding range of that destination's pending changes;
`debug.xemu.vk.vertex_pages=1` opts into coalesced changed-page copies before
launch. GPU fence waits and host-to-vertex barriers remain in place. A work
flush must retain pending mirror changes. Equal-data upload skips perform no
host write and add no pending changes.

With `debug.xemu.vk.stall_profile=1`, `XEMU_VERTEX_PROPAGATE` reports roughly
five-second totals for rotations, copied bytes, the equivalent bounding-range
bytes, copy runs and host copy time. Compare separate profiling-off moving
races for FPS; fewer copied bytes alone do not establish faster emulation.

# Render worker drain ordering

`test-render-idle` exercises the same mutex/condition wait used by the render
worker's callers. An empty queue with an active command must keep waiting.
Two waiters, spurious broadcasts, queued work after completion, the last
command leaving the queue and completion before a waiter starts are checked
across 200 rounds. The former queue-only predicate is the negative control.

```sh
cc -std=c11 -I . -UNDEBUG -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined android/test/cpp/test-render-idle.c \
  -o "$task_dir/render-idle-check"
"$task_dir/render-idle-check"
```

Use `-fsanitize=thread` instead for a separate ThreadSanitizer run. The Android
native target is `test-render-idle`. This fixture checks host drain ordering;
it does not submit Vulkan work or establish game stability or a speed gain.

# GLSL compiler startup preparation

The native target `test-glslang-prewarm` links the same pinned glslang library
and uses the production preprocessing helper. Run it in separate processes
without arguments and with `--warm`. It compiles GLSL 450 vertex, fragment and
geometry fixtures for Vulkan 1.1/SPIR-V 1.3, Vulkan 1.2/SPIR-V 1.5 and Vulkan
1.3/SPIR-V 1.6. Warming again must preserve each complete SPIR-V output byte
for byte; compare the nine sizes/hashes across the two fresh processes too.

The helper preprocesses a minimal shader, warming glslang's process-wide lazy
built-in tables. It creates no Vulkan module, pipeline or disk-cache entry.
Actual shader compilation retains its existing inputs, validation and SPIR-V
options. `debug.xemu.vk.compiler_prewarm=0` disables startup preparation on
Android for comparisons; the default is enabled. The startup log reports
whether preparation succeeded and its host elapsed time. A failure retains
normal shader compilation. This shifts initialization work to startup; it
does not eliminate compilation of new shaders or establish a game FPS gain.

# Optional regular VBlank cadence

`debug.xemu.vblank.adaptive=0` before launch selects a fixed, phase-preserving
deadline grid. Late callbacks skip missed periods and schedule one future
deadline; they do not deliver a catch-up burst. Existing period selection,
PCRTC interrupt assertion and acknowledgement, flip waiting and render fences
remain in place. The default keeps the existing adaptive deferral behavior.
This switch is an experiment, not a verified hardware timing correction.

The native target `test-vblank-timing` compares 30,000 callbacks against an
independently stepped reference clock at the existing NTSC/PAL periods and a
one-nanosecond period. It checks exact/late deadlines, phase preservation,
large gaps and signed-clock saturation:

```sh
cc -std=c11 -I . -UNDEBUG -Wall -Wextra -Werror \
  -fsanitize=address,undefined android/test/cpp/test-vblank-timing.c \
  -o "$task_dir/vblank-timing-check"
"$task_dir/vblank-timing-check"
```

With `debug.xemu.vk.stall_profile=1`, `XEMU_VBLANK` reports approximately
five-second timer-delivery totals: cadence, deferred fires, IRQ already
pending before assertion, flip wait state, callback intervals and deadline
lateness. The window also reports the current mode registers. Timer delivery
is not proof of guest interrupt handling, simulation speed or displayed FPS.
Measure FPS separately with diagnostics disabled.

# Primitive index assembly

`test-prim-rewrite` compiles the production assembler twice: the original
scalar emitters with renamed entry points, and the default batched emitters.
It compares complete index sequences, counts and passthrough/null behavior
across 48,960 cases covering every supported primitive, polygon mode, flat
shading/provoking-vertex combination, indexed input and multiple draw ranges.
Inputs include zero counts, incomplete tails, repeated/full-width indices,
negative range counts and long strips. Android links the real QEMU/GLib
headers and allocator. Assertions remain enabled in the native CMake target.

For standalone host ASan/UBSan checks, the fixture-only shim supplies standard
C declarations and a checked realloc; production does not include it:

```sh
task_flags="-std=gnu11 -D_GNU_SOURCE -O3 -UNDEBUG -Wall -Wextra -Werror"
task_flags="$task_flags -fsanitize=address,undefined"
task_flags="$task_flags -Iandroid/test/cpp/prim-rewrite-host-shim -I."
cc $task_flags -DNV2A_PRIM_REWRITE_BATCHED=0 \
  -Dpgraph_prim_rewrite_init=reference_prim_rewrite_init \
  -Dpgraph_prim_rewrite_finalize=reference_prim_rewrite_finalize \
  -Dpgraph_prim_rewrite_get_output_mode=reference_prim_rewrite_get_output_mode \
  -Dpgraph_prim_rewrite_ranges=reference_prim_rewrite_ranges \
  -Dpgraph_prim_rewrite_indexed=reference_prim_rewrite_indexed \
  -c hw/xbox/nv2a/pgraph/prim_rewrite.c -o "$task_dir/primitive-reference.o"
cc $task_flags android/test/cpp/test-prim-rewrite.c \
  hw/xbox/nv2a/pgraph/prim_rewrite.c "$task_dir/primitive-reference.o" \
  -o "$task_dir/primitive-check"
"$task_dir/primitive-check"
```

The six alternating timings measure only strip assembly in this fixture.
They do not measure game FPS, guest simulation speed or complete renderer
correctness. Compare the actual APKs in moving gameplay with diagnostics off.

# Android driving measurement markers

`android/test/java/DrivingTouch.java` is a standalone shell test helper for
the separate `.dev.local` test installation. It injects steering and throttle
touches only while that installation's `MainActivity` has focus. Each phase
checks focus repeatedly, and the helper releases the gesture at completion.
Protect and restore the test installation's data and emulator properties
around each session. The helper has been exercised on Android SDK 36.

Build it with the Android 36 SDK and build tools used for the local APK:

```sh
mkdir -p "$task_dir/touch-classes" "$task_dir/touch-dex"
javac --release 17 -cp "$task_sdk/platforms/android-36/android.jar" \
  -d "$task_dir/touch-classes" android/test/java/DrivingTouch.java
"$task_sdk/build-tools/36.1.0/d8" --min-api 29 \
  --lib "$task_sdk/platforms/android-36/android.jar" \
  --output "$task_dir/touch-dex" "$task_dir/touch-classes/DrivingTouch.class"
jar cf "$task_dir/driving-touch.jar" -C "$task_dir/touch-dex" classes.dex
adb push "$task_dir/driving-touch.jar" /data/local/tmp/x1box-driving-touch.jar
adb shell 'CLASSPATH=/data/local/tmp/x1box-driving-touch.jar app_process /system/bin DrivingTouch 2340 1080 "10:0:1,3:-0.2:0,2:-0.45:0,9:0:1" perf36-example'
```

Arguments are screen width, height, a sequence of
`seconds:steering:throttle` phases, and an optional unique label beginning
with `perf36-`. Steering ranges from -1 to 1; throttle is 0 or 1. The complete
sequence is limited to sixty seconds. Coordinates assume the default test
touch-control layout. The example drives for twenty-four seconds.

With a label, native `x1box-bench` log entries mark `touch_begin` after the
first steering/throttle injection and `touch_end` after releasing it. Both
include `SystemClock.uptimeMillis`. Wait for the native start before scheduling
mid-run captures. Windows PowerShell job startup and the outer ADB process
can take different amounts of time between runs.

Collect `x1box-bench` and `xemu-fps` with `logcat -v threadtime -v monotonic`.
Check that the marker timestamps agree with their embedded uptime values.
Select the emulator PID, then include only complete flip windows between the
native touch markers. Keep long windows when they satisfy those bounds;
discarding them could hide stalls. Confirm moving player/rivals and capture
timing separately. Countdown, different trajectories and barrier contact
remain comparison limits. These counters measure five-second averages;
frame-time percentiles require a separate recorder.

Bound APK installation waits separately from gameplay. A committed Android
installation can finish after its ADB client closes, so verify the final
package version and APK hash again before releasing the phone. Preserve
original lab data independently of installer completion, and keep platform
verification settings in place.

# Graphics pipeline lookup keys

`test-pipeline-key` compares the compact lookup predicate with the former
complete-state key predicate across 132,000 generated cases. It covers state
changes, forced hash collisions, render-pass/register/vertex-description
differences, shader-cache memory reuse, clear operations without shader
state, and clear/draw mismatches. The production comparator is shared with
the fixture. Assertions stay enabled in the native CMake target.

Android uses the actual renderer's `PipelineKey` and `ShaderState` types.
The host fixture supplies standalone field layouts to exercise the
size-independent comparator under ASan/UBSan:

```sh
cc -std=c11 -O3 -UNDEBUG -Wall -Wextra -Werror \
  -fsanitize=address,undefined -I. android/test/cpp/test-pipeline-key.c \
  -o "$task_dir/pipeline-key-check"
"$task_dir/pipeline-key-check"
```

The compact key borrows the shader LRU's previously computed hash, while
each pipeline binding owns a complete shader-state copy. Every candidate
with matching compact bytes still checks the complete state. Equal hashes
alone never establish equality, and shader-cache eviction must not change a
pipeline's saved state. Clear lookups do not require a shader-state pointer.
Keep render-pass load semantics and every keyed register/descriptor field.
Passing equivalence checks or reducing lookup-key bytes does not establish
a game FPS gain; measure moving gameplay separately with diagnostics off.

# Owned dynamic vertex inputs

`test-vertex-input` exercises the production snapshot/command helper across
1,088 cases with zero through sixteen bindings/attributes. It checks every
converted Vulkan field, deterministic unused storage, null arrays for empty
input and preservation after the caller overwrites both source arrays.
Android's native target keeps assertions enabled. Run the same helper under
host ASan/UBSan with Vulkan headers installed:

```sh
cc -std=c11 -O3 -UNDEBUG -Wall -Wextra -Werror \
  -fsanitize=address,undefined -I. android/test/cpp/test-vertex-input.c \
  -o "$task_dir/vertex-input-check"
"$task_dir/vertex-input-check"
```

`test-vertex-input-vk` requires Vulkan 1.3 and the vertex-input dynamic-state
feature on the selected test GPU. Compile its three generated shaders, then
run the native CMake target with those SPIR-V paths:

```sh
glslc android/test/cpp/vertex-input.vert -o "$task_dir/vertex-input.vert.spv"
glslc android/test/cpp/vertex-input.frag -o "$task_dir/vertex-input.frag.spv"
glslc android/test/cpp/vertex-input-empty.vert -o "$task_dir/vertex-input-empty.vert.spv"
"$task_dir/test-vertex-input-vk" "$task_dir/vertex-input.vert.spv" \
  "$task_dir/vertex-input.frag.spv" "$task_dir/vertex-input-empty.vert.spv"
```

Four static/dynamic/dynamic/static rounds verify 65,536 exact RGBA pixels
each while changing strides, offsets and float/normalized color formats.
Two empty-input draws add 8,192 pixels. Dynamic draws use the production
owned-state helper after overwriting the live descriptions and after binding
a static pipeline in the same command buffer. These generated pixels do not
exercise all guest formats or prove correctness for a complete game. Creation
timings use warmed driver state and exclude a preparatory static pipeline;
they are synthetic timings, not cold-game compilation or FPS measurements.

The production route stays opt-in: set Android property
`debug.xemu.vk.vertex_input_dynamic=1`, or desktop environment variable
`XEMU_VK_VERTEX_INPUT_DYNAMIC=1`, before starting the emulator. It requires
the extension, queried feature and callable entry point; otherwise it uses
static input. Check the Android startup message's requested/enabled values.
Each immediate/merged draw emits its current layout; reordered draws retain
their own layout. Empty-input draws also set state. Static clear/display
pipelines retain their existing behavior, and shader-state collision checks
remain complete. See the [Khronos command requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdSetVertexInputEXT.html).

Compare modes with profiling off, identical seed files and moving rivals.
A warm shader seed must include the exact driver-identity file written by
that driver and shader-cache ABI. Do not fabricate it or disable production
cache invalidation. Reject a matched-cache comparison if startup reports a
driver-identity mismatch or different module warm-up counts. A newly isolated
test files directory can lack this identity even on the same phone; imported
shaders will then be correctly discarded. Record this separately from the
unprofiled FPS measurements.

# Pipeline compilation policy and feedback

The generated-pixel target also accepts a final `0`/`1` argument for the
production pipeline-compilation policy. `1` applies Vulkan's
`VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`; omitted or `0` leaves normal
optimization enabled. Run both modes with the three shader paths above and
compare pixels before measuring moving gameplay. The flat shader's warmed
cache timings cannot establish the cost of a new game material.

The emulator's experiment is opt-in through Android property
`debug.xemu.vk.pipeline_fast_compile=1` or desktop environment variable
`XEMU_VK_PIPELINE_FAST_COMPILE=1`. It applies to ordinary graphics pipelines
in both synchronous creation and the existing async worker. A separate key
bit keeps policy variants apart, while full shader-state collision checks
remain unchanged. Clear, display and compute pipeline creation keep their
existing policy. No guest instructions, shader operations or geometry are
removed. The existing async draw-skipping setting is separate; leave it
disabled for this comparison.

Vulkan specifies that disabling optimization may reduce creation time;
measure execution cost as well. See [Khronos pipeline flags](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCreateFlagBits.html).
With the stall profiler enabled, supported devices also report creation
feedback and hashes of the actual vertex/geometry/fragment SPIR-V modules.
Vulkan 1.3 provides feedback directly; earlier devices require the optional
extension. Unprofiled FPS runs do not enable that extension, attach its
feedback structure or hash modules. Duration/cache-hit fields are read only
when feedback has its VALID bit, following [Khronos feedback requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCreationFeedback.html).
Module-hash groups help investigate repeated code; they do not prove complete
pipeline equality or justify relaxing the production state comparison.

# Complete pipeline recipes

`debug.xemu.vk.pipeline_prewarm` (Android) or `XEMU_VK_PIPELINE_PREWARM`
(desktop) selects `0` disabled/default, `1` replay and record, or `2` record
only. Shader caching must be enabled and async compilation disabled. Replay
runs after renderer initialization, before guest rendering starts. It creates
ordinary pipelines in the driver's existing cache, then destroys the replay
pipelines, layouts and shader modules. Guest draws still use their ordinary
pipeline lookup, complete shader-state collision checks and current state.
This uses the driver's [pipeline-cache semantics](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCache.html).

Successful synchronous draw-pipeline creation can save a pointer-free recipe
in private `vk_pipeline_recipes.bin` and SHA-256-addressed SPIR-V copies in
`pipeline_spv`. Recipes retain stages, vertex descriptions, fixed/dynamic
state, attachment formats and push-constant ranges; Vulkan handles are never
persisted. Replay uses the current descriptor layouts. The header must match
ABI, record size, driver identity, enabled physical features and relevant
renderer capability/policy bits. Bump the recipe ABI when the descriptor
interface or recipe interpretation changes. Recipe deduplication checks both
SHA-256 and complete bytes. Missing, corrupt or incompatible records fall
back to ordinary pipeline creation; only complete checked records survive a
torn append. Reads are bounded to 1,024 records and 4 MiB per shader. The
SPIR-V check validates instruction bounds and the stage's `main` entry point;
it is not a complete semantic validator. Successful original pipeline
creation and SHA-256 verification establish the provenance of replay code.

`test-pipeline-recipe` runs 1,088 owned-state capture/rebuild round trips,
including overwritten caller arrays, geometry, depth/color attachments,
push ranges and compilation flags. It also checks unaligned records,
overflowing indices, every-byte corruption/truncation and SPIR-V bounds/stage
rejection. Run the production header on a host with GLib/Vulkan headers:

```sh
cc -std=c11 -O2 -UNDEBUG -Wall -Wextra -Werror \
  -fsanitize=address,undefined -I. $(pkg-config --cflags glib-2.0) \
  android/test/cpp/test-pipeline-recipe.c $(pkg-config --libs glib-2.0) \
  -o "$task_dir/pipeline-recipe-check"
"$task_dir/pipeline-recipe-check"
```

The generated-pixel Vulkan fixture above also captures and rebuilds its
pipeline descriptions after overwriting caller state. Run both compilation
flags. CPU round trips and generated pixels establish limited correctness,
not a game FPS gain. Measure replay startup separately from moving gameplay.
Seed every comparison with the same opaque Vulkan cache, shader/recipe files
and actual driver identity; exclude the record-only training pass from FPS
comparisons. Confirm `xemu-pipeline-recipe` loaded/warmed/failure counts and
inspect fresh driver cache-hit feedback with profiling in separate runs.

# Exact uniform updates

Perf41 adds an opt-in CPU packing route through Android property
`debug.xemu.vk.uniform_exact=1` or desktop environment variable
`XEMU_VK_UNIFORM_EXACT=1`, set before renderer startup. The default is off.
Check the startup `exact uniform updates` value before comparing modes.
The route compares the actual reflected value bytes, copies changed elements,
and reads bulk vertex/light constants directly from the same source arrays
used by the existing getter. Scalar computation, shader interfaces and guest
instructions stay unchanged. Existing binding/frame/reset force-upload flags
remain effective. Comparisons are byte-exact, including signed zero and NaN
payloads; padding is left untouched. It does not rely on hashes or dirty flags
to declare values equal.

Run the production packing helper against the existing copy implementation:

```sh
cc -std=c11 -O2 -UNDEBUG -Wall -Wextra -Werror \
  -fsanitize=address,undefined -I. -Iinclude \
  android/test/cpp/test-uniform-update.c -o "$task_dir/uniform-update-check"
"$task_dir/uniform-update-check"
```

The native Android CMake target is `test-uniform-update`. The fixture compares
all bytes of guarded buffers across 36,864 generated updates with one to four
components, lengths through 192, contiguous/strided layouts, partial/empty
updates, repeated values and cold allocation replacement. It checks the
returned change flag and raw-bit/padding preservation. These checks establish
packing equivalence for the generated inputs; they do not exercise every
shader binding, queued draw or game. Measure unprofiled moving gameplay with
identical cache seeds and settings, and review graphics separately before
accepting an FPS improvement or enabling the route by default.

When collecting timestamped JIT attribution, read the map and dump from the
preserved stopped test directory after restoration and verify source hashes
before/after each read. Sequential reads from a running translator can contain
different numbers of blocks. Select the actual native library build ID and
check temporal load records; reject unsupported overlapping code intervals
instead of silently assigning an old block. Keep sampling/JIT diagnostics out
of unprofiled FPS comparisons and retain warning/sample-loss evidence. A
kernel-symbol warning is not permission to change device security settings.

# Exact descriptor-command reuse

Vulkan descriptor bindings persist in their command buffer until replaced or
disturbed, and dynamic offsets are consumed when the bind command is recorded;
see [Khronos descriptor binding semantics](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBindDescriptorSets.html).
The opt-in Android property `debug.xemu.vk.desc_memo=1` or desktop environment
variable `XEMU_VK_DESC_MEMO=1` omits a single graphics-set bind only when the
command buffer, exact pipeline-layout handle, set number, descriptor handle,
offset count and copied offsets match the last recorded bind. The default is
off; push and bindless paths retain their existing binds.

The renderer invalidates this state at every recording start, pipeline bind
and push-descriptor write. It tracks actual recorded commands through merged
and reordered draws; it is not rolled back with saved logical guest state.
Compute and presentation use separate bind points or command buffers. No
shader operation, draw, texture update or guest instruction is removed.
Check the startup mode and use the separate `descriptor_bind` and
`descriptor_bind_skip` profiler counters to measure calls actually avoided.
Profiling is disabled for FPS comparisons.

```sh
cc -std=c11 -O2 -UNDEBUG -Wall -Wextra -Werror \
  -fsanitize=address,undefined -I. android/test/cpp/test-descriptor-bind.c \
  -o "$task_dir/descriptor-bind-check"
"$task_dir/descriptor-bind-check"
glslc android/test/cpp/vertex-input-empty.vert -o "$task_dir/descriptor.vert.spv"
glslc android/test/cpp/test-descriptor-bind.frag -o "$task_dir/descriptor.frag.spv"
cc -std=c11 -O2 -UNDEBUG -Wall -Wextra -Werror -I. \
  android/test/cpp/test-descriptor-bind-vk.c -lvulkan \
  -o "$task_dir/descriptor-bind-vk-check"
"$task_dir/descriptor-bind-vk-check" "$task_dir/descriptor.vert.spv" \
  "$task_dir/descriptor.frag.spv"
```

The Android native targets are `test-descriptor-bind` and
`test-descriptor-bind-vk`. The CPU fixture performs 57,344 checks, including
overwritten caller offsets, individual offset changes, layout/set/set-number
changes, offset counts, invalidation and repeated command-buffer handles.
Four GPU rounds compare 131,072 exact generated RGBA pixels while changing
dynamic offsets, descriptor sets and incompatible pipeline layouts. Foreign
binds intervene and the same command handle is reset between submissions.
Each reused-state round emits 18 of 128 test requests and skips 110. Generated
repeated requests establish limited state/pixel correctness; that synthetic
skip ratio is not a game skip ratio, FPS improvement or full compatibility test.
### Small uniform value comparisons

With exact uniform packing enabled, `debug.xemu.vk.uniform_small=1`
(`XEMU_VK_UNIFORM_SMALL=1` on desktop) additionally compares 4/8/12/16-byte
values using owned integer temporaries. Fixed-size `memcpy` loads preserve
unaligned-pointer and aliasing rules; a 12-byte value reads exactly 8+4 bytes.
Other sizes keep `memcmp`. Equality includes every raw bit, including floating
NaN payloads and signed zero. Padding, shader interfaces, dirty-state handling
and force uploads are unchanged. This separate experiment defaults off.

`test-uniform-update` exercises both ordinary and small-value comparison modes
against the existing packing implementation. `test-uniform-value-compare`
independently checks equality against libc for all sizes through 32 bytes,
independent pointer alignments and every possible single-bit change. Guarded
allocation tails additionally exercise empty, small and 3072-byte spans, with
an inaccessible next page and a read-only source.

```sh
cc -std=c11 -D_GNU_SOURCE -O2 -UNDEBUG -Wall -Wextra -Werror \
  -fsanitize=address,undefined -I. -Iinclude \
  android/test/cpp/test-uniform-value-compare.c -o /tmp/uniform-values
/tmp/uniform-values
cc -std=c11 -O2 -UNDEBUG -Wall -Wextra -Werror \
  -fsanitize=address,undefined -I. -Iinclude \
  android/test/cpp/test-uniform-update.c -o /tmp/uniform-updates
/tmp/uniform-updates
```

These are generated byte-equivalence checks. Native instruction selection,
moving-game FPS and graphics coverage require separate evidence; passing the
fixture does not establish a speed gain.
