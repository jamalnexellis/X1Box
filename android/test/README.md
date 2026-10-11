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
