# PGR 2 startup investigation

Base: official X1 BOX tag `1.2.9` (`dadced00cea8bcbada5a5650541cc5bac3cc93f4`).
Development application ID: `com.izzy2lost.x1box.dev`.
Latest phone startup build: `1.2.9-pgr2-perf8`.
Its GPU upload code is unchanged from pixel-tested `1.2.9-pgr2-perf7`.

## Reproduced startup cause and correction

On the connected SM-S938B, the unprepared European XISO repeatedly boots
`default.xbe`, returns to the HDD dashboard, and is launched again. The BIOS,
EEPROM region/checksums and XBE section digests validate. Debugging the same
files with desktop xemu locates the failure in XAPI initialization: the game's
DVD-X2 media check sends MODE SENSE page `0x3e`, which returns
`STATUS_INVALID_DEVICE_REQUEST` (`0xc0000010`). The caller then requests a
quick reboot to the dashboard before initializing graphics.

The XISO is missing the media-enable adaptation normally applied by
[extract-xiso](https://github.com/XboxDev/extract-xiso/blob/master/extract-xiso.c).
A controlled test applying its instruction change to an isolated disc copy
reaches the Bizarre Creations introduction and renders a race on the phone.

The APK detects PGR 2 by the XBE certificate's title ID (`0x4d53004b`) and
looks for the exact supported instruction sequence in executable sections
of the root `default.xbe`. If a unique match exists, the ATAPI PIO and DMA
read buffers expose the adapted byte. The original disc image is never
opened for writing. Nonmatching titles, already adapted images, malformed
metadata and ambiguous matches remain unmodified. Disc changes and device
resets invalidate the detected offset.

Synthetic regression cases run under AddressSanitizer/UndefinedBehaviorSanitizer
in CI, including a signature spanning two scan chunks, invalid metadata,
title isolation and source immutability. The native test also supports reading
the actual image on Android. On the SM-S938B, the corrected eight-byte detector
reports offset `0x194964` for the unaltered image, and the `perf1` APK cold-boots
it into the introduction without loading a VM snapshot. Source immutability is
verified with the original image's full SHA-256 before and after the tests.
The final `perf2` APK also cold-boots the unmodified image, both from the private
reproduction copy and through the normal SAF game-library launch.

## Evidence from the three supplied logs

- The selected title is Project Gotham Racing 2, Europe, XISO.
- Vulkan initializes and creates a swapchain on the Qualcomm Adreno 830.
- OpenGL ES also initializes. Later it reports framebuffer texture ID zero
  while the emulated machine remains in `RUN_STATE_RUNNING` (9).
- The same symptom appears with FP JIT and skip-boot-animation disabled.
- All captured launches use `XEMU_ANDROID_INLINE_AIO=1`.
- The `blk_drain()` assertion at 19:15:38 is an earlier crash retained in the
  Android crash buffer, not a fresh crash from these launches.

These initial logs alone did not identify the guest instruction or DVD command
at which startup stopped. The startup diagnosis above comes from the subsequent
controlled reproduction and guest debugging.

## Confirmed configuration defect

The connected SM-S938B has `android.tcg_thread = 'single'`, but the release
launches with `tcg,thread=multi,tb-size=256`. The native configuration loader
uses POSIX `setenv()` after SDL initializes. SDL 3's `SDL_getenv()` reads its
cached environment and therefore misses these changes. Startup option readers
now use the same live process environment as the configuration writer.
This also preserves explicit inline-AIO and renderer environment overrides.

The first diagnostic APK confirms single-thread selection is now honored,
but that correction alone does not resolve PGR 2 startup. The existing native
coroutine test passes 1,000 migrations, nested entry, stack/FP state and
termination on this phone; disabling the inline-AIO workaround also leaves
the original startup loop unchanged.

The CMake optimization switches also previously omitted definitions when OFF,
allowing source-file defaults of 1 to re-enable them. Both native targets now
receive explicit 0/1 definitions; profiling tolerates disabled TB hints.

## Diagnostic changes

- Honor the existing debug-log preference in native code. The original native
  accessor always returned false, suppressing guarded surface diagnostics.
- Sample guest CPU registers every five seconds, on the vCPU thread, with at
  most one request pending. A blocked CPU cannot accumulate queued requests.
- Record DVD commands and error sense codes with bounded output.
- Record nonzero Xbox startup error codes written to the SMC.
- Build the existing native coroutine regression executable alongside the APK.

The new records use the `xemu-boot` log tag and are captured by the existing
debug-log export. They are enabled only with debug logging.

## Device verification

Install the development APK alongside the original app. Configure the same
Xbox files and PGR 2 image, enable debug logs, and reproduce one launch. Run
the coroutine test on the same device before evaluating an asynchronous-I/O
change. Do not delete or format the user's HDD or EEPROM.

The game starting on this device is the startup acceptance criterion; compiling
an APK or seeing a functioning renderer alone is insufficient.

## Performance changes and measurement

Two changes preserve rendering inputs and assertions:

- The draw invariant check now saves only the four values it compares. It no
  longer copies the entire register file, shader constants and lighting context
  into a render-command snapshot on every draw. The old snapshot occupied about
  12% of the PFIFO thread's sampled CPU cycles in the controlled race.
- Contiguous uniform arrays and matrices now use one block copy. Strided
  scalar/vec2/vec3 arrays keep their padding and element-by-element layout.
  Standalone tests use the production packing helper and check partial updates,
  matrices, NaN payloads, signed zero, padding and allocation guard bytes under
  AddressSanitizer/UndefinedBehaviorSanitizer and on Android. The allocation-end
  assertion now measures from the allocation base rather than the uniform's
  offset.

An experiment removing CPU waits from inline texture updates did not establish
a clear FPS gain on this device and was reverted. Depth, textures, shadows,
reflections, native resolution and frame delivery are preserved. No FPS result
is obtained by hiding the player's car, reducing resolution or skipping frames.

The benchmark restores the same private VM snapshot: Florence street race,
Volkswagen New Beetle, third-person camera, stationary player and three AI cars.
Both builds use Vulkan, native scale 1, 64 MiB Xbox RAM, FP JIT off, shader cache
on, draw reorder/merge and asynchronous compilation off. The phone cools between
runs; elevated thermal status invalidates a direct comparison. Warm-up, intro,
menus, paused time and snapshot loading are excluded from gameplay FPS.

The initial cooled `fix1` reference measured 12.27 FPS over eight five-second
windows (11.66–13.47 FPS). The first `perf2` run measured 11.53 FPS
(10.79–12.12 FPS) in the equivalent window. Profiling reduces the PFIFO thread's
`apply_uniform_updates` share from 12.83% to 6.89%, and the invariant snapshot
copy disappears, but these changes do not establish a gameplay FPS gain. Both
runs had thermal status 0; normal scheduling and the moving AI still introduce
variation. Later samples after user input changed the scene are excluded.
A repeat from the normal SAF library, with no sampling profiler or controller
input during measurement, averaged 11.18 FPS over seven five-second windows
(10.72–12.10 FPS). This also does not establish a FPS improvement; its external
disc access path differs from the private-file reference. The remaining speed
problem is unresolved, despite less CPU work in the two targeted renderer paths.
A single scene cannot establish maximum FPS for the entire game.

## Cold texture upload investigation

The next report concerns slowdown while textures are first rendered. A fresh
process and a restored private race snapshot make the GPU texture cache cold;
the existing disk shader cache is retained. This distinguishes texture upload
from compiling every game shader again. The sampling profiler is not active
during FPS comparisons.

On the Adreno 830 system driver, a CPU call graph places most of the regular
texture upload cost inside `vkCmdCopyBufferToImage`, including driver memory
allocation and its kernel ioctl. `perf3` adds a compute upload for plain decoded
RGBA8 textures. `perf4` also stores compatible BGRA byte streams in RGBA8 images
and composes the sampling component map with a red/blue exchange. This retains
the original byte stream, including forced alpha in XRGB textures; it does not
reduce texture resolution or remove effects. Border-addressed BGRA remains on
the original path because custom border colors with nonidentity views require
an additional Vulkan feature. Volumes, virtual borders in swizzled
textures and other formats also retain their original upload paths. `perf5`
corrects the eligibility check for linear rectangles: their border-source flag
does not expand the data, so it must not exclude an otherwise eligible upload.
`perf6` extends the path to RGB565 and each face of a cubemap. RGB565 images
retain their original sampled format and packed bits. A compatible R16_UINT
storage view writes those bits without changing normalization, filtering,
alpha, border color, or the raw surface-copy representation. Mutable format
and extended usage are explicit, and each view restricts its usage to its
role. This follows Vulkan's [format compatibility classes](https://docs.vulkan.org/spec/latest/chapters/formats.html#formats-compatibility-classes)
and [extended usage rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageCreateFlagBits.html).
The RGB565 path requires enabled shaderStorageImageExtendedFormats, storage
format support and supported image creation flags; otherwise it retains
transfer uploads. Small 16-bit mip levels reserve four-byte aligned storage
and initialize padding, so the last half-word read stays inside the allocation.
Each cubemap face uses its own 2D storage view; barriers cover all mips and
faces, and descriptor capacity accounting includes every face.

The default is restricted to the pixel-tested Adreno 830 Qualcomm driver. The
diagnostic property `debug.xemu.vk.compute_upload=0/1` permits comparison on one
APK. Initialization checks storage-image support, the compute queue capability
and the staging buffer range; failure preserves the original transfer path.
Storage views use identity component mappings. Explicit image and host-buffer
barriers separate reads and writes, and descriptor sets are recycled only after
the existing completion synchronization. Cached texture updates retain their
existing CPU waits.

The standalone device test uses the production upload shader and helpers. It
compares GPU readback byte for byte for five sizes, every mip, non-multiple
workgroups, a nonzero staging offset and three writes to each image. All RGBA8
cases pass on the SM-S938B. The expanded test additionally checks actual shader
sampling of BGRA and forced-alpha views against independent byte expectations,
including raw image copies in both directions. All of these comparisons pass
on the same GPU with the `perf4` executable.

The upload diagnostic measures command recording, views, descriptors and image
barriers only. It excludes CPU decoding, staging copies and waits, so a smaller
reported upload time must not be presented as the same percentage gain in FPS.

### Withdrawn phone measurements and recovered controls

The previously reported perf4–perf7 phone FPS comparisons are withdrawn.
Inspection prompted by the user's report of completely unresponsive controls
finds that the external app working directory belonged to the ADB shell after
restoration. The app could not write xemu.toml or access its EEPROM normally.
Its logs record failed configuration writes, failed EEPROM generation,
autostart disabled and a snapshot restore error for missing `smbus-storage`.
The partially restored machine could render a race even though its hardware
and settings did not match the snapshot. It also used default fp_jit=true and
TCG cache size 128, despite the requested fp_jit=false and cache size 256.
A rendered image alone therefore did not validate these sessions.

The repair preserves the old external directory for recovery and recreates
the working directory and unchanged EEPROM through the app's own UID. A new
launch writes the configuration, reports fp_jit=0 and tb-size=256, connects
the virtual controller to port 1 and restores the race without the previous
errors. The user explicitly confirms that touch controls respond again.
No change to the touch-control source or mapping was required. Future phone
restore commands must create these files through run-as, rather than creating
shell-owned files in the app's external working directory.

The standalone GPU comparisons remain valid: their executables do not depend
on the broken emulator session. The old command-recording counters show the
upload code executing, but must not establish a valid-session frame-rate gain.
The previous CPU profile from that session is also superseded. Perf8 aborts
startup with a visible error if creating or writing/flushing/closing the
configuration fails, instead of launching hardware with unintended defaults.

### Valid-session CPU profile

A separate 15-second CPU-cycle profile follows a verified configuration,
EEPROM, controller binding and successful snapshot restore. It records
180,127 samples with none lost. The emulated CPU thread accounts for 50.34%
of sampled CPU cycles; the graphics FIFO thread accounts for 27.29%.
These are shares of sampled CPU work, not GPU utilization, waiting time or
proof of the complete frame's critical path. Generated guest code cannot
be fully attributed from the normal call graph. Among attributable CPU
functions, execution/dispatch of translated blocks dominates; code generation
is a smaller contribution during this warmed scene.

The graphics profile places 17.47% of that thread's cycles in
copy_surface_to_texture, predominantly Adreno driver allocation/ioctl calls
inside vkCmdCopyImage. The CPU thread's samples are almost entirely on cores
6–7: 54,604 samples and 26,159,522,586 cycles, versus 41 samples and 3,501,584
cycles on cores 0–5. The device reports maximum frequencies 4,473,600 kHz
for cores 6–7 and 3,532,800 kHz for cores 0–5. This does not support a claim
that Android is assigning the CPU emulation mainly to slower cores.

Initial valid perf7 diagnostic runs pass the configuration/EEPROM/controller checks,
but the car moves during their preparation and timed windows. A longer LT
hold also selects reverse after braking. They do not establish a paired FPS
gain for uploads or direct-surface binding. The direct-surface diagnostic is
restored to its original empty value; the mobile-driver workaround remains.
The subsequent perf8 comparison below saves a stationary, neutral-input state
and validates both the beginning and end of every accepted capture.
Shader compilation and first-frame texture stalls remain to be separately
measured. The slowdown is unresolved; no full-speed or maximum-FPS claim is
made.

### Stationary race comparison after restoring controls

Perf8 is compared with compute texture uploads forced off and on using the
diagnostic property on the same APK. Each run opens a fresh process, restores
the same private HDD and stopped-car snapshot and restores the same SPIR-V,
shader-module-key and Vulkan pipeline caches. The snapshot starts in PGR2's
pause menu; a complete touch A press/release resumes the race before timing.
No driving input follows. All accepted beginning and end screenshots show
0 km/h, the same car position and coherent road, buildings and HUD.

Each capture lasts 40 seconds. Only complete five-second FPS windows starting
at least five seconds after the measurement marker are included. There are
six accepted windows in each run. This is a stationary race throughput test
with populated disk shader caches; it does not quantify first-frame texture
stalls or shader compilation latency.

| Upload route | First accepted run | Repeat |
| --- | ---: | ---: |
| Buffer/image transfers | 13.123 FPS | 13.004 FPS |
| Compute uploads | 14.293 FPS | 14.375 FPS |

The accepted order is transfer, compute, compute, transfer. The aggregate
counts are 790 flips in 60,474 ms for transfers and 866 flips in 60,416 ms for
compute: 13.063 versus 14.334 FPS, approximately 9.7% higher in this scene.
The compute logs confirm that RGBA8 and RGB565 paths are enabled; all runs
verify fp_jit=0, TCG cache size 256, controller binding and no configuration,
EEPROM or snapshot errors. Android thermal status remains 0 throughout the
accepted runs; this alone does not prove constant CPU/GPU clocks.

A setup capture still in the game's pause menu is excluded. A transfer repeat
overlapping an ADB backup download is also excluded and replaced with a clean
run. These results supersede the withdrawn older comparisons and establish
a modest scene-specific upload-path gain. They are not a comparison between
perf7 and perf8, whose upload code is identical, or a full-speed claim.

## Remaining compatibility work

The user reported intermittent building, road and menu corruption and confirmed
that graphics now look correct with the installed `perf2` APK and conservative
configuration. The sampled Florence screenshots, pause menu and leave-race
confirmation also show coherent textures and text. This verifies the observed
case, not every track or a longer sequence of races and menus. No separate
upstream rendering defect is claimed fixed from this observation alone.

Upstream xemu still tracks [PGR 2 slow gameplay](https://github.com/xemu-project/xemu/issues/1376)
and [intermittent menu corruption](https://github.com/xemu-project/xemu/issues/2339).
The lock-free fence polling optimization from upstream PR #2941 is already in
the official X1 BOX 1.2.9 base; it is not a new correction in this fork.

## Previously delivered binary

`perf2` source commit: `1ba08e1a1983fe9bb05cd0574baf1668ec42080f`.
[Successful build and native test artifacts](https://github.com/jamalnexellis/X1Box/actions/runs/37998447457).
APK SHA-256: `fe1cf9da9491781e50e887c74832b2da62e9fd35fd2309f8a7b054e15959d316`.
The standalone uniform packing test passes on the SM-S938B. The coroutine and
real-image media tests also passed on that device during this investigation.
The installed app has its SAF game folder configured, debug logging disabled
after measurement, and the tested Vulkan settings retained. Temporary device
profiling properties and USB stay-awake changes are restored. Firmware, EEPROM
and HDD tests use isolated copies; the user's original files remain untouched.

## Pixel-tested GPU binary

`perf7` source commit: `804d9072afc1cc86966f6267d29c4a14b6fe6ef3`.
[Successful build and native test artifacts](https://github.com/jamalnexellis/X1Box/actions/runs/38008983718).
APK version: `1.2.9-pgr2-perf7`, version code 36.
APK SHA-256: `50924571fe99695bbcef12ab8c3c0f7d8912e323b969e70fd8b04f8ae877332e`.
The artifact checksum and the delivered APK copy match. This binary includes
the original startup adaptation, uniform-copy optimization, pixel-tested GPU
texture uploads, native ThinLTO and disabled rendering profiling overhead.
Private phone backups, firmware, game images, logs and screenshots are not
included in the repository or CI artifacts.

## Validated configuration-guard binary

`perf8` source commit: `94cd6ecdeff656946b29e12bdf205ed027d29a82`.
[Successful APK build](https://github.com/jamalnexellis/X1Box/actions/runs/38011029972).
APK version: `1.2.9-pgr2-perf8`, version code 37.
APK SHA-256: `daa660de7ff2c2c858585c0095d6ca74fde11c39ae46103f8762129f2f11c703`.
The artifact checksum and delivered APK copy match. Updating the existing
development package succeeds without uninstalling it or losing its data.

A phone failure-injection test preserves the configuration file, temporarily
replaces its path with an empty directory and launches the same game. Native
logs record the expected configuration-write failure and startup abort; Android
records the error dialog producing a frame. The QEMU core does not start.
The test restores the original file through the app's UID even on failure.
The initial UI assertion occurs after the dialog has already been dismissed;
the full native/window log establishes that the dialog was rendered.

Normal startup then writes and loads the intended configuration, opens the
unchanged EEPROM and connects the controller to port 1. The game's intro
renders and its race snapshot loads successfully. Touch Start pauses the game
and touch A resumes it; coherent road, buildings and HUD are visible.
The new configuration guard adds no claimed FPS improvement.

After testing, the private HDD, original snapshot, shader caches and preferences
are restored from the pre-measurement backup. The unchanged EEPROM is copied
through the app's UID and its checksum is verified. The temporary Slot 2 preview
is preserved outside the active snapshot directory; its benchmark HDD remains
in a private backup. Diagnostic properties, debug logging and USB stay-awake
settings are restored. The user's original game image and firmware/HDD files
remain untouched.

## Further opt-in translation candidate (perf13)

The ARM64 [xemu-macos optimization ledger](https://github.com/MichaelJSr/xemu-macos/blob/macos-optimizations/docs/optimizations.md)
describes removing per-block `can_do_io` stores when instruction counting and
record/replay are disabled. Its performance reports concern another game and
host and are not results for this fork or phone. The local translator emits
these stores even into the profiled memory-poll block. MMIO encountered before
the last instruction then takes a state-restore/recompile/re-execution path
that exists for deterministic I/O scheduling.

Perf13 adapts this candidate as `debug.xemu.tcg.elide_candoio=1`, off by default.
The decision is latched before the first translated block and forced off with
instruction counting or record/replay. Plugin, single-step, breakpoint-page,
instruction-counted and memory-only replay blocks retain the existing stores.
The normal CPU initialization, TB return and exception-exit paths keep the
flag true. The MMU, MMIO callbacks, `mem_io_pc`, interrupt checks, exit-request
checks, memory barriers and floating-point behavior are unchanged.
Compilation, device compatibility, emitted-code checks and same-APK performance
validation are pending. This is not a demonstrated FPS improvement.

## Android guest instruction profiling

Perf9 adds opt-in Android perf-map and JIT-dump output in private app storage.
Profiling is disabled in normal gameplay. This supplies generated-code address
ranges for CPU samples that the ordinary call graph cannot attribute reliably.
It is diagnostic instrumentation, with no claimed FPS improvement.

A verified 20-second foreground race capture records 217,639 CPU-cycle samples
with none lost. The emulated CPU thread accounts for 57.50% of sampled CPU
cycles and the graphics FIFO thread for 25.24%. These are CPU work shares,
not GPU utilization or proof of the frame's critical path. The car remains
stationary, controller binding and configuration checks pass, and the activity
does not pause during this capture. Two earlier attempts that leave the game
foreground are excluded.

Within the CPU thread, flat sample attribution places 54.04% of cycles in native
functions, 45.90% in translated guest code and 0.06% in unmapped ranges. This is
a different attribution method from the earlier call-graph profile; the changed
percentages do not establish a performance gain. A guest memory-polling loop
accounts for approximately 19.98% of this thread's sampled cycles. Its wait
target is not established, so it must not be described as a proven GPU or audio
bottleneck. Native block execution and dispatch also consume substantial work.

JIT dumps record generated blocks before subsequent jump patching. Branch
immediates in the saved bytes alone cannot establish the executed block's
current chaining state. Dumps, game code, sample addresses and device logs
remain private and are not included in the repository or build artifacts.

## Latest CPU allocation binary and wireless comparison

Perf10 returns ARM64 registers X26 and X27 to the TCG allocator when the
optional Xbox RAM shortcut has no configured mapping. In this base,
`xbox_ram_size` remains zero, but the old backend reserves both registers
regardless. Normal soft-MMU translation can now use these callee-saved registers.
The ABI save/restore behavior is unchanged. A configured RAM shortcut retains
its reservations, and shortcut emission also checks that they exist.

The diagnostic property `debug.xemu.tcg.reclaim_regs=0` selects the earlier
allocation for same-APK comparisons; the default enables the change. This
does not enable the inactive RAM shortcut, remove MMU checks, change floating
point precision, or reduce rendering quality.

`perf10` source commit: `dec0e79ec477b77003e65b1cb2c43f8d48f96902`.
[Successful APK build and CI checks](https://github.com/jamalnexellis/X1Box/actions/runs/38013731634).
APK version: `1.2.9-pgr2-perf10`, version code 39.
APK SHA-256: `501b7b6386b098ee7202bd507b0add2a62334f4c51be73f88fb4c8d904b61a13`.
The build artifact and delivered APK checksums match. Updating the development
package succeeds without uninstalling it. After authenticated wireless ADB
pairing, four fresh-process race captures pass the configuration, EEPROM,
controller and snapshot checks. Touch A resumes the pause state in both CPU
configurations. All beginning and end screenshots show the same stationary car
at 0 km/h, neutral sticks and coherent road, buildings and HUD. No capture leaves
the game foreground.

The comparison uses the same APK, compute uploads enabled, identical private
HDD/snapshot and populated disk shader caches. Each capture lasts 25 seconds.
Only complete five-second FPS windows beginning at least five seconds after
the measurement marker are counted: three windows per capture. The accepted
order is earlier allocation, reclaimed registers, reclaimed registers, earlier
allocation. No CPU profiler or backup transfer runs in the timed windows.

| CPU allocation | First run | Repeat |
| --- | ---: | ---: |
| Earlier reservations | 14.923 FPS | 14.479 FPS |
| Reclaimed X26/X27 | 14.955 FPS | 14.931 FPS |

Aggregates are 443 flips in 30,133 ms for the earlier allocation and 451 flips
in 30,181 ms for reclaimed registers: 14.701 versus 14.943 FPS, approximately
1.6% apart. The earlier-allocation repeats vary by about 3%, larger than the
aggregate difference. These short captures do not establish a material FPS
improvement or resolve the slowdown. Thermal status remains 0; CPU/GPU clocks
are not held constant. This comparison also does not measure cold texture or
shader-compilation stalls, or a speedup over the earlier perf8 upload tests.

After measuring, the pre-CPU-test private HDD, snapshots, shader caches,
preferences and EEPROM are restored through the app's UID. EEPROM checksums
match and the restored HDD checksum is checked against the private backup.
The latest benchmark previews are preserved outside the active snapshot
directory, and a separate pre-wireless-test backup preserves that intermediate
state. Temporary CPU/compute properties and USB stay-awake are restored to
their original values. The user's original game image and firmware/HDD files
remain untouched.
# Experimental CPU dispatch investigation (perf11)

The valid perf9 vCPU profile attributes 22.48% of sampled vCPU cycles to
`cpu_exec_loop` and 7.90% to `cpu_tb_exec`. Disassembly of generated kernel
blocks shows an STI block followed by a separate block that completes the
interrupt shadow. These are candidate dispatcher costs, not evidence that CPU
translation is the frame-rate bottleneck. The game's memory-poll loop consumes
19.98% of the sampled vCPU cycles, and its dependency remains unidentified.

Perf11 adds opt-in STI chaining (`debug.xemu.tcg.sti_chain=1`). The default is
off. It uses the existing TB lookup to enter the following block while the
interrupt shadow remains active. The following block retains its exit-request
check, completes one instruction, clears the shadow and returns to dispatch.
MOV/POP SS, an already active shadow, TF/RF, instruction-count limits, icount,
debug stepping, disabled chaining, breakpoint pages and plugins retain the
existing path. Cache misses return with the updated EIP and hflags saved.
No guest instructions or interrupt checks in the destination block are removed.
CPU semantic validation remains pending. The same-APK race comparison below
does not establish a material performance gain; the experiment stays off by
default.

`debug.xemu.gdb=1` enables the existing guest debugger on `127.0.0.1:9009` for
ADB forwarding. It is off by default and does not bind a network-facing address.
The Android build now generates QEMU's i386 register feature table from its XML
description instead of linking the empty feature stub.
The register table is required for reading registers and guest memory at the
identified polling instruction. The first device debugging attempt below
exposes an additional Android linking omission. Debugging captures and guest
data stay outside the repository, and FPS comparisons use debugging off.

Perf11 source `897d5d28ccea8af6fafea1a2464be615de56e562` builds successfully
in [CI run 38017930331](https://github.com/jamalnexellis/X1Box/actions/runs/38017930331).
The media/uniform sanitizer tests pass, and the existing Android native
fixtures and Vulkan shaders compile. The APK has SHA-256
`dc32a5014ba79d9216e36f7bc93717135e4abd8d688011484a7a2a88ac7250dd`.
Inspection of the packaged ARM64 library confirms the i386 register feature
table and the experimental switch are included. Its signing certificate
matches perf10, permitting an update without uninstalling the development app.
The private diagnostic client passes offline checks for fragmented packets,
unexpected guest code, breakpoint removal and detach; these do not validate
CPU semantics or device behavior.

Authenticated wireless testing subsequently updates the development package
to perf11 without uninstalling it. Four fresh-process, 25-second captures use
the same stationary race snapshot, populated disk caches, compute uploads on
and reclaimed registers on. The order is STI chaining off, on, on, off.
Complete five-second windows starting at least five seconds after the marker
are accepted; no debugger, CPU profiler or backup transfer runs during them.
Configuration, EEPROM, controller and snapshot checks pass in all four runs.
Every beginning/end screenshot shows a stationary car, neutral controls and
coherent road, buildings and HUD; the game does not leave the foreground.
Touch A resumes the snapshot's pause state in both configurations.

| STI chaining | First run | Repeat |
| --- | ---: | ---: |
| Off | 15.276 FPS | 15.037 FPS |
| On | 15.419 FPS | 15.107 FPS |

The aggregates are 458 flips in 30,218 ms with chaining off and 461 flips in
30,203 ms with it on: 15.157 versus 15.263 FPS, approximately 0.7% apart.
Repeat variation exceeds this difference. Thermal status remains 0 but clocks
are not held constant. There is no demonstrated material FPS gain, proof that
the eligible STI path is used in this scene, or measurement of cold uploads.

The separate debugger-enabled launch aborts before gameplay with
`'socket' is not a valid char driver name`. The foreground guard prevents any
subsequent touch input or screenshot. No wait-target registers are captured,
and this failed diagnostic is excluded from FPS results. The static Android
core omits the socket backend's registration unit because it has no referenced
global symbol. Perf12 adds the same explicit Android link anchor already used
by the null backend and keeps debugging disabled by default. Device validation
of this correction remains pending.

The current pre-test private HDD, snapshots, shader caches, preferences and
EEPROM are restored through the app's UID even after the diagnostic failure.
The downloaded backup checksum and restored HDD/EEPROM checksums match their
pre-test contents. All temporary diagnostic properties are restored, and the
emulator is stopped. The user's original game image and firmware/HDD files
remain untouched.
