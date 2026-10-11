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

The CPU fixture checks the shared presentation decision with no Vulkan device:

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

In the emulator, the opt-in `debug.xemu.vk.present_reuse=1` must be set before
launch. With `debug.xemu.vk.stall_profile=1`, `XEMU_PRESENT_REUSE` logs totals
over approximately five seconds. Rejection-reason counts can overlap and must
not be summed as a frame budget. Use separate profiling-off driving runs to
measure FPS and compare reviewed images before accepting a performance change.
