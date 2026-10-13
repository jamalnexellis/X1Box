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
by the null backend and keeps debugging disabled by default. The subsequent
perf12 device diagnostic below validates this correction.

The current pre-test private HDD, snapshots, shader caches, preferences and
EEPROM are restored through the app's UID even after the diagnostic failure.
The downloaded backup checksum and restored HDD/EEPROM checksums match their
pre-test contents. All temporary diagnostic properties are restored, and the
emulator is stopped. The user's original game image and firmware/HDD files
remain untouched.

## Validated guest-wait diagnostic (perf12)

Perf12 source `f59605b5abc64ac9286ecdf5756bb2a02ac1b5ae` builds successfully in
[CI run 38019041384](https://github.com/jamalnexellis/X1Box/actions/runs/38019041384).
The version is `1.2.9-pgr2-perf12` (41), with APK SHA-256
`236d9af098efb761bf07c1b5f11306792cbf3cd2678c68eaab8c1d4bfbbedccd`.
Inspection of the packaged ARM64 library confirms the socket registration
anchor and i386 register features. The signing certificate matches perf11;
an authenticated wireless update succeeds without uninstalling the app.

In a separately authorized phone window, a fresh process loads the stationary
race snapshot and touch A resumes it. Configuration, controller, EEPROM and
snapshot checks pass. Both screenshots show coherent graphics and neutral
controls, and the game remains in the foreground. This debugging session is
excluded from FPS comparisons.

The loopback debugger now starts successfully. The private RSP client verifies
the guest instruction bytes, stops at the profiled poll, checks the stopped PC,
and reads the i386 registers and only the four-byte polled word. The observed
wait targets ordinary guest RAM and the word is nonzero. This identifies the
wait location, but does not identify its writer or establish that GPU or audio
work limits the frame rate. Raw registers, addresses and guest code stay private.

The client removes its breakpoint and detaches. The temporary ADB forward is
removed, and the current pre-test HDD, snapshots, caches, preferences and EEPROM
are restored through the app's UID. The downloaded backup checksum and restored
HDD/EEPROM checksums match this session's archive; diagnostic properties return
to their original values and the emulator is stopped. Original user game and
firmware/HDD files remain untouched.

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

Source `a20c4623fc893a23961d65f3cebcf2debdf1d936` builds successfully in
[CI run 38019626143](https://github.com/jamalnexellis/X1Box/actions/runs/38019626143).
The media/uniform sanitizer tests pass, and the Android native fixtures and
Vulkan shaders compile. These checks do not validate CPU translation semantics.
Version `1.2.9-pgr2-perf13` (42) has APK SHA-256
`a5a02ba1b230bb49f4eb340db5814bd6b93bff9622f4f1e9d6a02684a3fa2e14`.
The downloaded checksum matches CI, and packaged-library inspection confirms
the mode log, socket registration anchor and guest register feature table.
Its signing certificate matches perf12, and an authenticated wireless update
installs without uninstalling the app.

During a newly authorized five-minute window, four fresh-process, 25-second
captures repeat the same stationary snapshot with populated disk caches.
The order is elision off, on, on, off; STI chaining stays off, compute uploads
and reclaimed registers stay on, and CPU profiling and debugging stay off.
Both requested and effective elision modes match the intended settings in the
logs. All configuration, controller, EEPROM, snapshot and foreground checks
pass. The eight screenshots show coherent graphics, neutral controls and a
stationary car; touch A resumes the paused snapshot in both configurations.
Only complete five-second windows beginning at least five seconds after each
measurement marker are accepted, with three windows per run.

| `can_do_io` elision | First run | Repeat |
| --- | ---: | ---: |
| Off | 14.362 FPS | 14.433 FPS |
| On | 14.503 FPS | 14.370 FPS |

Aggregates are 435 flips in 30,213 ms with elision off and 436 flips in
30,201 ms with it on: 14.398 versus 14.437 FPS, approximately 0.27% apart.
Repeat variation is larger than this difference. Thermal status stays 0 but
clocks are not held constant; these are warmed-scene results and do not measure
cold texture uploads or shader compilation. The experiment remains off by
default. This comparison does not demonstrate a useful FPS gain or establish
that translation of the wait loop limits the frame rate. CPU semantic and
emitted-code validation remain pending before any default activation.

The current pre-test private state is restored through the app's UID and the
emulator is stopped. The downloaded archive checksum and the restored HDD and
EEPROM checksums match this session's backup. EEPROM retains app ownership,
all diagnostic properties match their pre-test values, and no ADB forward is
left behind. Original game and firmware/HDD files remain untouched.

## Bounded RAM-writer diagnostic (perf14)

Perf12 identifies a RAM word polled by the game, and the perf14 device capture
below confirms matching zero writes by GP scratch DMA. Speeding up the polling
loop has not demonstrated a material frame-rate improvement. Attribution of
the producer does not establish the duration or criticality of this dependency.

Perf14 adds two minimal Android HMP commands reachable through the opt-in
loopback guest debugger: `xemu-apu-watch` and `xemu-apu-unwatch`. While the guest
is stopped, the watch command resolves the supplied aligned virtual word using
the current CPU page tables and rejects unmapped or non-RAM targets. Tracing
starts off in every process and is excluded from saved machine state.

The APU DMA callbacks retain their original copying and dirty tracking. After
each write, an armed diagnostic checks whether it contains the watched word.
At most 16 matching writes record their APU producer and submitted value;
only the already submitted four source bytes are inspected. The probe never
acknowledges a command, bypasses a wait or changes DSP execution budgets.
It covers GP/EP scratch and FIFO DMA, not all possible RAM writers, so an absent
event would leave the dependency unresolved. Captures stay private and are
excluded from FPS comparisons.

Source `abf6e9de376c013dc53a80d2643e317029037a67` builds successfully in
[CI run 38021000818](https://github.com/jamalnexellis/X1Box/actions/runs/38021000818).
Version `1.2.9-pgr2-perf14` (43) has APK SHA-256
`bcf231fe44fdc524f38ed61750a533832f0f6daad89b8f3eb13a229a29870613`.
The downloaded artifact checksum matches CI, its certificate matches perf13,
and the packaged ARM64 library contains both diagnostic entry points and
mode strings. The private RSP client passes offline checks for fragmented
packets, multiple monitor output packets, rejected-watch cleanup, mismatched
guest code, breakpoint removal and detach. These checks exercise the client
protocol and cleanup; they do not validate CPU translation semantics.

A newly authorized seven-minute window completes both tests on perf14. The
wireless device reconnects through its existing mDNS pairing; an update installs
without uninstalling the app. The existing full-DSP preference changes
off/on/on/off within the same APK and stationary snapshot. DSP JIT stays on;
compute uploads and reclaimed registers stay on, while STI chaining, I/O elision,
profiling and debugging stay off. The preference variants differ in only the
full-DSP setting, and the effective audio and CPU configuration is checked in
each process's log. Each run lasts 25 seconds; three complete five-second
windows beginning at least five seconds after the marker are accepted.

| Full DSP | First run | Repeat |
| --- | ---: | ---: |
| Off | 14.939 FPS | 14.914 FPS |
| On | 13.110 FPS | 14.827 FPS |

The off aggregate is 451 flips in 30,215 ms (14.926 FPS), and the on aggregate
is 422 flips in 30,219 ms (13.965 FPS), a decrease of approximately 6.44%.
The on repetitions vary by approximately 13.10%, so the size of a repeatable
regression is unresolved. This comparison does not demonstrate an improvement
from full DSP. Thermal status stays 0, but clocks are not held constant. The
eight screenshots show coherent buildings, road and HUD, neutral controls and
a stationary car; touch A resumes the snapshot. All controller, EEPROM,
snapshot, configuration and foreground checks pass. This is a short warmed
scene, not a test of cold uploads or shader compilation. Full DSP changes both
processing and the audio output path, so it does not isolate a single cause.

The separate debugger-enabled launch restores the original DSP-off benchmark
configuration and passes the same health and two-screenshot review. The RSP
client verifies the code before installing a breakpoint and again at the stopped
PC, then reads the four-byte polled word. The monitor resolves that virtual word
through the current page tables into RAM. After breakpoint removal and detach,
exactly 16 matching writes come from `gp-scratch`, all submitting zero to the
watched word. This confirms that GP scratch DMA writes the value that can
release this particular guest wait. It does not measure request-to-completion
latency, prove that audio limits FPS, or exclude other writers. Raw game code,
registers, addresses and captures remain private. No FPS from this diagnostic
is accepted as a benchmark.

Both tests and restoration finish within the agreed phone window. The current
pre-test state is restored through the app's UID. The downloaded archive
checksum and restored HDD/EEPROM checksums match this session's backup;
EEPROM retains app ownership. All six temporary properties return to their
pre-test values, the debugger forward is removed, and the emulator is stopped.
The user's original game and firmware/HDD files remain untouched. The existing
default DSP setting is restored. The next useful measurement is the dependency's
latency and the producer's execution versus scheduling/pacing time before
selecting another synchronization or translation change.

## First-use graphics stall capture (perf15)

The user's new report distinguishes steady rendering from FPS drops as new
objects appear. Stationary warmed-scene comparisons do not reproduce this
trigger. The existing upload timer covers only command recording and omits
decoding, staging and waits; it cannot attribute these first-use stalls.

Perf15 adds `debug.xemu.vk.stall_profile=1`, off by default and latched before
Vulkan worker creation. It records host wall times for texture hashing,
decoding, staging, image allocation/view creation, upload recording, texture
reuse/capacity waits, shader disk loads, compilation and persistence, graphics
pipeline creation/persistence, submission and fence waits. Each guest flip
reports its elapsed interval and the count/total/maximum/bytes of operations
completed since the previous flip. At most 512 operations lasting at least
2 ms also report their start/end times. No guest addresses, texture bytes,
shader source or registers are recorded. The mode does not change texture
formats, resolutions, shader behavior, cache identities or synchronization.

These times include driver execution and host scheduling. Nested categories
and operations on different threads can overlap, so totals are not additive
GPU timings or proof of a critical path. Operations spanning a flip are charged
to the interval in which they complete; individual slow events expose their
actual bounds. The first flip contains initialization and must be excluded.
Profiling itself adds overhead and cannot establish an FPS gain. The prepared
capture must start a fresh process to exercise cold GPU texture state, retain
the existing shader disk cache, and include movement into new scenery as well
as the initial state load. Gameplay health and start/end images must pass review.
Source `b8cbe9a6227432ced9dc17a17af765139ca16ffd` builds successfully in
[CI run 38063138109](https://github.com/jamalnexellis/X1Box/actions/runs/38063138109).
Version `1.2.9-pgr2-perf15` (44) has APK SHA-256
`4b47ee4d29c2f0beca23bb1f5085a1b34d54a156dfdd47058afbc043d726d8a2`.
The artifact checksum and signing certificate identity match the expected
build, and the packaged ARM64 library contains the profiler entry points.

The authorized capture starts a new emulator process, retains shader disk
caches, resumes the Florence snapshot and then holds the mapped right trigger
for 25 seconds. Start/end screenshots show coherent road, buildings and HUD;
the final image shows the car in different scenery. Controller, EEPROM,
configuration, foreground and unpaused-session checks pass. However, the
initial segment's final screenshot already shows the car moving, and its first
image includes a volume overlay. Its marker name in the private analyzer does
not establish a stationary baseline. No controlled stationary-versus-movement
comparison or optimization gain is claimed.

The movement segment contains 360 complete flip intervals: median 68.053 ms,
p95 84.298 ms and maximum 107.924 ms. These are diagnostic frame intervals with
profiling overhead, not an FPS improvement. Relevant completed operations are:

| Movement phase | Operations | Total host time | Longest operation |
| --- | ---: | ---: | ---: |
| Texture decode | 587 | 238.452 ms | 1.297 ms |
| Texture staging | 587 | 25.249 ms | 0.245 ms |
| Texture upload recording | 587 | 793.415 ms | 4.075 ms |
| Texture image/view creation | 209 | 3.837 ms | 0.153 ms |
| Graphics pipeline creation | 31 | 37.264 ms | 23.331 ms |
| Pipeline cache persistence | 1 | 8.879 ms | 8.879 ms |
| Finish submission region | 1,184 | 10,315.114 ms | 36.448 ms |
| Finish rotation/wait region | 1,184 | 908.008 ms | 6.747 ms |

There are no recorded texture reuse/capacity flushes or shader compilations in
this segment. The 107.924 ms interval includes a 23.331 ms pipeline creation and
a 35.239 ms finish operation. The other interval above 100 ms includes ten
finishes. Nested phases and different threads overlap, so these times must not
be added into a frame budget. Slow-event quota 512 expires before the movement
segment, leaving per-frame aggregates but no individual event bounds there.
The instrumented submission region includes render-worker acknowledgement and
synchronous fence completion; its large time cannot yet be attributed to
`vkQueueSubmit` alone, GPU execution alone or a specific finish reason. The
capture does not prove the cause of every reported new-content stall.

The current pre-test archive is restored through the app UID. Archive checksum,
HDD and EEPROM hashes match this session's backup, EEPROM retains app ownership,
all seven diagnostic properties return to their prior values, the debugger
forward is absent and the emulator is stopped. Raw images, logs and game data
remain private.

## Submission acknowledgement experiment (perf16)

The next candidate addresses busy yielding while PFIFO waits for the render
worker to acknowledge a deferred submission. `debug.xemu.vk.submit_wait=1`, off
by default and latched before worker creation, replaces this loop with a
condition wait. The atomic submitted predicate is published after successful
`vkQueueSubmit`; publication and the wait use the same mutex, and spurious
notifications recheck the predicate. Frame rotation uses the same helper.
GPU fence waits, command ordering, semaphores, callbacks, texture formats and
controller handling stay in place. This is a scheduling experiment, not a
demonstrated FPS gain or a fix for pipeline compilation latency.

The profiler additionally separates worker queue residence, queue submission,
GPU fence waits, submission acknowledgement, display image acquisition,
presentation, display fences and nine finish reasons. The slow-event quota is
4,096 for this larger phase set; per-flip aggregates still continue afterwards.
Categories remain nested host wall times. A standalone fixture exercises the
production condition handshake with POSIX-backed QEMU API adapters: prior
notification, spurious wakeups, two independent frame predicates and 100 rounds
of slot reuse. It does not validate Android scheduling, the QEMU adapters or
Vulkan correctness.

Source `21ca92d1dae84ae6f363d779f2ec7a761949b2b1` builds successfully in
[CI run 38064601850](https://github.com/jamalnexellis/X1Box/actions/runs/38064601850).
Version `1.2.9-pgr2-perf16` (45) has APK SHA-256
`26fe6a31ac96c682985f1f9ff1813b2cedc7dede468a7e7b49f165347494d064`.
The handshake, XISO and uniform fixtures pass with ASan/UBSan on the CI host;
the Android native fixtures and texture shaders compile. The downloaded APK
matches CI's checksum and retains perf15's signer certificate identity. Its
ARM64 library contains the submission-wait property, helper and extended
finish-reason names. These checks do not verify the APK payload's signature
cryptographically or establish a device performance improvement.

The private analyzer passes eight offline checks for both phase schemas,
partial/mixed-schema rejection, frame continuity, marker boundaries and event
timestamps. Its phase names match the 31 production categories, whose frame
line fits the Android log payload limit even with maximum-width counters.
The next capture is prepared with checksum and foreground guards, fresh
processes for both wait modes, the same snapshot and complete press/release
inputs. Both traces use profiling, and shader disk caches may grow between
them; they are diagnostic comparisons, not accepted FPS benchmarks. It will
preserve the then-current state and all eight diagnostic properties through
the app UID. A fresh phone-availability window and device validation remain
pending. The candidate stays disabled by default.

The first attempt to open Load State fails before measurement and restores
the current state. A retry gives startup five seconds and uses a complete
press/release on the overlay menu button. All three resulting images show
coherent road, buildings and HUD: both initial images have a stationary car
and neutral controls; the final image shows movement into different scenery.
Foreground, controller, configuration and unpaused-session checks pass. The
second wait mode is omitted to finish within the original five-minute window.
The condition-wait candidate has no device A/B validation or accepted FPS gain.

The retry's movement segment contains 378 complete flips, median 67.915 ms,
p95 82.887 ms and maximum 94.410 ms. It creates 236 texture images/views and
31 pipelines. Texture decode peaks at 1.243 ms; pipeline creation peaks at
30.202 ms. The finer categories resolve the large submission region:

| Movement phase | Operations | Total host time | Longest operation |
| --- | ---: | ---: | ---: |
| Worker queue residence | 2,708 | 105.279 ms | 3.883 ms |
| Queue submission | 3,085 | 133.137 ms | 2.242 ms |
| GPU fence completion | 462 | 9,911.396 ms | 35.198 ms |
| Deferred submission acknowledgement | 379 | 37.643 ms | 2.274 ms |
| Finish: surface download | 378 | 9,851.714 ms | 36.080 ms |
| Display image acquisition | 1,867 | 31.984 ms | 0.848 ms |
| Display queue presentation | 1,866 | 222.046 ms | 22.133 ms |

The surface-download finish averages 26.063 ms per flip; acknowledgement
averages about 0.099 ms per call. Fence and surface-download regions overlap
and must not be added. The dominant measured wait is graphics completion for
a surface operation, not render-worker submission acknowledgement. The
acknowledgement experiment stays off and is not the priority optimization.
The reason identifies a family of paths, not the precise readback caller or
the GPU work responsible for every stall. Pipeline first-use latency also
remains relevant. There is no accepted FPS gain from these diagnostic traces.

Both attempts restore their own current archives through the app UID. Archive,
HDD and EEPROM checksums match the corresponding backup, app ownership and
all eight properties are restored, debugger forwarding is absent and the
emulator is stopped. The phone is released before the agreed deadline.

## Reusing identical native presentations (perf17)

The finer capture records 1,866 display presentations for 378 game flips.
This suggests redundant presentation work but does not prove that every extra
presentation is identical: a surface can change between guest flips. Perf17
therefore tests identity explicitly before retaining the already presented
image. `debug.xemu.vk.present_reuse=1` is off by default and latched before
worker creation. It applies only to native Vulkan presentation.

The key includes source image/view, drawing generations, surface-list
generation, scanout address/stride, source/output geometry, viewport and
swapchain format/extent. Pending CPU uploads, active or previously visible
PVIDEO, and thumbnail capture force presentation. Only a successful queue
presentation commits a key; failure, early returns and swapchain destruction
invalidate it. An unchanged idle image is presented again at least every
250 ms so output changes and errors are discovered within a bounded interval.
Guest flips, GPU fence waits and required surface readbacks are preserved.
The render worker still completes synchronization requests when an identical
presentation is retained. Controls and distinct images follow their existing
paths. Profiling counts retained presentations in `display_reuse`.

The standalone production-policy fixture covers changes to every key field,
pending uploads, video, capture, failed presents, cache invalidation, clock
rollback and the fixed refresh deadline. It does not exercise a Vulkan
swapchain or prove that every image mutation is tracked.

Source `0040c0fdb98405ad439dabf24f493b59d7a01221` builds successfully in
[CI run 38066380552](https://github.com/jamalnexellis/X1Box/actions/runs/38066380552).
Version `1.2.9-pgr2-perf17` (46) has APK SHA-256
`4a5188b14eebe40dececf06abea2852641c8b217adda4579daff033e94719d0f`.
The presentation-policy, submission-handshake, XISO and uniform fixtures pass
with ASan/UBSan on the CI host; Android native fixtures and texture shaders
compile. The downloaded artifact matches CI's checksum, retains perf16's
signer certificate identity and contains the presentation-reuse property,
startup log and profiler category. The version name is present in the APK
manifest. These checks do not verify the APK payload's signature
cryptographically or establish a device performance improvement.

The private analyzer passes eleven offline checks for the complete 15-, 31-
and 32-phase schemas, partial/mixed-schema rejection, frame continuity,
marker boundaries and event timestamps. The 32-category frame payload fits
the Android log limit with maximum-width counters. A fresh five-minute
capture is prepared: moving scenery with profiling enabled checks candidate
correctness and retained-presentation counts; separate same-APK stationary
captures with profiling disabled compare FPS if time permits. Two short
captures alone cannot establish repeatability or new-content gains. The
then-current app-owned state and all nine properties will be preserved and
restored. The previous phone window has ended. Fresh availability and device
correctness/performance validation remain pending; the candidate stays off
by default.

The five-minute device window completes all three planned captures. Seven
reviewed images show coherent road/buildings/HUD, neutral controls in both
stationary comparisons and movement into different scenery in the diagnostic
trace. All runtime mode, native-presentation, configuration, controller and
foreground checks pass. The instrumented movement segment contains 336
complete flips, median 62.394 ms, p95 76.220 ms and maximum 205.076 ms.
`display_reuse` counts zero in both initial and movement segments: the guarded
candidate does not retain an image in these scenes. The trace alone does not
identify which identity or exclusion check prevents reuse.

The longest interval contains a complete 105.291 ms shader-compilation
operation on PFIFO's thread, followed by an 18.342 ms pipeline creation. It
also contains a 29.945 ms surface-download finish overlapping a 29.036 ms GPU
fence wait. This captures a specific first-use compilation stall consistent
with the new-content symptom; it does not explain every reported hitch or
resolve the recurring readback caller. Across movement, surface-download
finishes still occur once per complete flip (336 calls, 8,006.065 ms total,
36.899 ms maximum). Texture decode peaks at 1.281 ms. Nested phases are not
additive GPU timings.

The separate same-APK, profiling-disabled stationary captures contain three
complete accepted five-second windows each. Reuse off records 224 flips in
15,133 ms (14.802 FPS); reuse on records 234 in 15,107 ms (15.490 FPS), an
observed difference of 4.644%. Each mode has only one run, intervals vary
substantially, disk shader caches can grow and clocks are uncontrolled. No
repeatability or candidate-caused improvement is established; the diagnostic
also found no reuse. The mode remains off and is not an accepted optimization.

The current pre-test archive is restored through the app UID. Archive, HDD
and EEPROM checksums match, EEPROM ownership and all nine originally empty
properties are restored, debugger forwarding is absent and the emulator is
stopped. Independent verification completes before the agreed deadline and
the phone is released. Raw captures and game data remain private.

## Isolating compiler stages and readback callers (perf18)

The next diagnostic splits shader compilation into preprocessing, parsing,
linking and SPIR-V generation. It distinguishes the five surface-readback
finish families (deferred completion, direct buffer download, pending
downloads, dirty downloads and incompatible-binding eviction), and times the
texture-range parent and CPU-access wait. Required submissions, fences,
downloads, compiler options and shader output stay on their existing paths.
All timing remains opt-in through `debug.xemu.vk.stall_profile=1` and is not
an FPS benchmark or an optimization.

The 43 phase categories are emitted in two bounded log parts per guest flip.
Each part carries the same frame number, end time and interval; both use one
locked counter snapshot. The private parser joins complete matching parts
and rejects missing, duplicate, truncated, mismatched or mixed-schema parts.
It retains support for the earlier complete 15-, 31- and 32-category logs.
Seventeen offline checks pass. Compiler/readback phases overlap their parent
regions and different threads; they must not be added as GPU time. No guest
addresses, instructions, shaders or texture content are added to these logs.

The Android glslang build already disables SPIRV-Tools optimization. Its
[pinned upstream translation path](https://github.com/KhronosGroup/glslang/blob/b5782e52ee2f7b3e40bb9c80d15b47016e008bc9/SPIRV/GlslangToSpv.cpp#L10816)
also limits that optimizer to HLSL legalization or size optimization. Merely
turning off an optimizer is not a substantiated fix for the captured GLSL
compilation stall. This build measures the active stages before choosing a
compiler change. Perf17's presentation-reuse mode remains off.

Source `db3c14d235eb56ba20d1acfe561a0a81d492d0aa` builds successfully in
[CI run 38072571578](https://github.com/jamalnexellis/X1Box/actions/runs/38072571578).
The downloaded version `1.2.9-pgr2-perf18` (47), package
`com.izzy2lost.x1box.dev`, has APK SHA-256
`36f180751e2600d200335a543d9c1635653e77ed829ad42ab8c4ef73f715ce8d`.
It matches CI's checksum and retains the previous signer certificate identity.
Its native library contains all eleven compiler/readback detail phase names
and the split-frame header. Host presentation-policy, submission-ordering,
XISO and uniform fixtures pass with ASan/UBSan; Android native fixtures and
texture shaders compile. These are build and artifact checks, not device
performance or full APK signature validation.

The next private three-minute capture preserves the then-current app-owned
state and all nine properties before installing. It temporarily stages one
known cached shader to try to reproduce the captured first-use compilation,
records its original hash, loads the same snapshot and drives into new
scenery with complete press/release input. Foreground, controller, runtime
mode and trace completeness guards apply. Restoration independently checks
the current archive, HDD, EEPROM, original shader and properties. Cache
extraction merges files; newly generated cache entries may remain. No entire
cache reset is planned. Fresh phone availability and device validation remain
pending; this APK is diagnostic and has no accepted FPS improvement.

The fresh three-minute perf18 window completes with all three images reviewed:
the initial car is stopped with neutral controls, and the final image shows
68 km/h and different road/buildings. Configuration, controller, native
presentation, foreground and split-frame completeness checks pass. The
analyzer joins 974 consecutive complete 43-phase records. No shader compiler
stage runs in this trace, including startup; staging the one cache entry does
not reproduce its first use on this route. The earlier 105.291 ms compilation
remains evidence of one hitch, but its internal stage attribution is pending.

The initial segment has 238 complete flips (median 63.308 ms, p95 72.805 ms,
maximum 77.706 ms). Movement has 431 (median 62.429 ms, p95 77.155 ms,
maximum 89.221 ms). Every measured surface-download finish belongs to deferred
completion: 431 calls and 9,198.886 ms host time in movement, averaging
21.343 ms per flip. CPU-access waits, pending/dirty downloads and incompatible
binding eviction count zero in the measured segments. Texture-range fallback
parents account for 4,326 calls and 9,401.175 ms, overlapping deferred finish
and GPU fences. Their byte counters are requested ranges, not transferred
bytes. Pipeline creation peaks at 30.297 ms. These instrumented scenes and
cache histories do not establish an FPS improvement over earlier APKs.

The current archive, HDD, EEPROM, staged shader's original bytes and all nine
originally empty properties are independently verified after restoration
through the app UID. The emulator is stopped, debugger forwarding is absent
and the phone is explicitly released before the deadline. New phone actions
require fresh availability. Game and cache contents remain private.

## Identifying the surface-to-texture fallback (perf19)

Perf18 places the recurrent synchronization in the texture-range fallback and
deferred surface completion. It does not identify which texture shape fails
the existing GPU surface-copy eligibility checks. Perf19 adds at most sixteen
shape-only detail records for slow fallback calls while stall profiling is
enabled. They record dimensions, formats, layout, pitch, mip/cube flags,
overlap/dirty counts and the selected exact or overlapping surface's shape.
Dirty counts are collected before the readback. Addresses, pixels and shader
contents are excluded. Profiling off performs no extra surface scan or log.
The existing GPU-copy eligibility, downloads and rendering remain unchanged.
This is a targeted diagnostic; a GPU-only conversion must match the observed
layout before it can be a correctness-preserving optimization. Build,
artifact checks and a fresh capture are pending.

The first perf19 CI attempt stops before native compilation because the NDK
download ends with `Premature EOF`. Retrying the identical source
`5ec8170c2255bc8cc08e58ef97dff9deb6e6c9de` succeeds in
[CI run 38073974631](https://github.com/jamalnexellis/X1Box/actions/runs/38073974631).
Downloaded version `1.2.9-pgr2-perf19` (48) has APK SHA-256
`83b9773743ceb41b149d1c5b0c35772ecbcd17a065a7306d4b159df3d0c60b99`.
Package/version parsing, the shape-detail string and compiler/readback phases
are verified in the artifact; its checksum matches CI and its certificate
identity matches perf18. Host ASan/UBSan fixtures pass and Android native
fixtures/texture shaders compile. No device install, shape capture or FPS
gain is established yet. A guarded current-state-preserving three-minute
capture is prepared without staging shader cache entries; fresh availability
is requested after these checks.

Further private perf18 event analysis finds 540 slow deferred-completion
regions completely contained in texture-range fallback parent regions. All
541 logged slow texture-range operations request the same byte length. The
4,096-event quota expires during the trace, so these events are a bounded
subset; complete per-flip counters remain available. Requested byte length
alone does not establish texture dimensions, format or cubemap layout.

The fresh three-minute perf19 capture installs successfully and joins complete
split-frame records. The three images show a stationary initial car with neutral
controls, then 99 km/h and new scenery after a complete trigger press/release.
All sixteen bounded slow-detail samples identify one shape: a swizzled RGB565
256-by-256 cubemap, one mip, no border, with six dirty overlapping surfaces. The
selected exact-base color surface is also swizzled RGB565, 256-by-256, pitch 512,
two bytes per pixel and no pending upload. These startup samples identify the
shape; they do not independently establish eligibility of every face throughout
gameplay.

The initial segment has 242 complete flips (median 62.965 ms, p95 70.036 ms).
Movement has 377 (median 65.036 ms, p95 82.055 ms, maximum 104.277 ms). Deferred
completion remains once per flip: 377 calls and 10,039.961 ms during movement,
averaging 26.631 ms. Texture-range parents overlap that wait; their requested
range counters are not transferred bytes. Shader compilation stays zero, texture
decode peaks at 1.488 ms and pipeline creation at 36.506 ms. This diagnostic
does not establish a performance improvement. Archive, HDD, EEPROM ownership
and all nine originally empty properties are independently verified after
restoration through the app UID; the emulator is stopped and the phone released.

## Opt-in GPU copy for the observed cubemap (perf20)

The candidate `debug.xemu.vk.cube_copy=1` copies six active color surface images
directly into the cubemap's six layers. It is off by default and applies only
to one-mip, borderless, swizzled native RGB565 cubes at scale one. Every face
must be initialized, have an exact base, matching dimensions, pitch and size,
an active CPU access callback and no pending upload/download. Missing, shelved,
depth, differently formatted or otherwise incompatible sources keep the CPU
fallback. The existing guest CPU access callbacks and required CPU readbacks
remain active; GPU copying does not pretend CPU VRAM has been refreshed.

Refresh identity covers all six images and their draw times/generations, plus
the active surface-list generation to detect retired/recreated sources. GPU
source changes are checked before draw fast paths can skip texture binding.
CPU dirty aliases invalidate the copy identity. Returning from a cube copy to
the CPU path forces a fresh hash/upload, even if old CPU VRAM appeared clean.
Queued source writes also force binding/recording before their generations
advance. Unchanged cubes preserve batching of unrelated queued draws.

Prior queued draws are recorded first. Shared production copy code transitions
all six sources and the destination with explicit same-queue barriers, copies
each face to its corresponding layer, restores source layouts and transitions
the destination for sampling. It updates the existing image without changing
descriptors or removing resource retirement. It adds no host fence wait to the
GPU copy. The 43-category profiler remains compatible; copy recording uses the
existing texture-record phase and bounded shape-only detail messages. A runtime
message confirms the path was actually used even with profiling off.

New fixtures reject incompatible or incomplete sources and changes to every
face identity. A Vulkan fixture executes the shared copy code with generated
RGB565 patterns at 16 and 256 pixels, checks all six layers, then replaces only
face five across separately queued command buffers without a host wait between
them. CI runs it on software Vulkan with synchronization validation and also
builds the Android executable for the actual driver. CI/artifact checks and
device graphics/FPS validation remain pending; no gain is accepted yet.

Source `cf22d4b38c854ad61cf093110591857aa47b9a22` passes
[CI run 38076325568](https://github.com/jamalnexellis/X1Box/actions/runs/38076325568).
The policy fixture passes ASan/UBSan and the production GPU-copy fixture passes
on llvmpipe with synchronization validation, including both sizes and a queued
replacement of face five. Android compilation also passes. The downloaded
package `com.izzy2lost.x1box.dev`, version `1.2.9-pgr2-perf20` (49), has SHA-256
`70057006bd657924f6030ac02d04540e1441b3a42a84f2eef05a20650d86ce3d`.
It matches CI's checksum and retains the earlier signer certificate identity;
native experiment, engagement and detail strings are present. These checks
do not establish full APK signature validation, Adreno pixels or FPS gains.

The APK artifact initially omits the two new, successfully compiled fixture
binaries from its upload list. The list is corrected and the unchanged test
sources are independently compiled for Android in
[fixture run 38077176901](https://github.com/jamalnexellis/X1Box/actions/runs/38077176901).
Both downloaded binaries are AArch64 ELF files with verified CI checksums.
A fresh seven-minute capture is prepared to execute these generated-pixel
fixtures on the device, capture the candidate while driving, then compare
four identical stationary states in off/on/on/off order with profiling off.
Current app-owned data and all ten properties, including the new experiment,
are preserved and independently verified after restoration. Fresh phone
availability and actual-device validation remain pending.

The fresh perf20 session loses its mDNS transport before any phone mutation.
Reconnecting the known paired device allows a shorter diagnostic and one off/on
comparison within the original seven-minute deadline. Both generated-data native
fixtures pass on Adreno 830, including exact six-layer pixels at both sizes and
queued replacement of one face. This device run has no validation layer enabled;
the synchronization-validation result remains the earlier software-driver test.
Android accepts the perf20 update and the runtime confirms the copy actually
engages. All seven captures are reviewed: the initial/stationary states have
neutral controls and coherent buildings, road, car and HUD; the movement capture
shows 91 km/h and new scenery. This checks resume and accelerator input, not
every control or the whole game.

The diagnostic joins 540 complete flips in its marked segments. Initial has
256 (median 60.680 ms, p95 70.829 ms, maximum 94.011 ms); movement has 284
(median 68.634 ms, p95 99.722 ms, maximum 130.760 ms). Surface-download,
texture-range fallback and CPU-access readback categories all count zero in
both segments. Movement's copy/upload recording averages 0.537 ms per flip;
shader compilation stays zero and pipeline creation peaks at 26.715 ms.
However, `finish_wait` still accounts for 7,556.449 ms, averaging 26.607 ms
per moving flip. Its scope includes recycling the next submission slot's GPU
fence, framebuffer/staging cleanup and vertex RAM propagation. Previous fence
instrumentation omits that slot fence, so the existing aggregate does not yet
distinguish these contributions or establish pure GPU work time.

With profiling off, three complete five-second windows per mode give
217 flips / 15,117 ms = 14.355 FPS off and 221 / 15,083 ms = 14.652 FPS on.
The observed difference is about 2.1%; a single pair, AI traffic, cache history
and timing variation do not establish repeatable improvement. Thermal status
reports zero. The originally empty ten properties, archive, HDD, EEPROM and
ownership are independently verified after restoring current app-owned state.
The emulator is stopped and the phone released before the deadline. The
experimental copy remains off by default; no large performance gain is claimed.

## Measuring the remaining slot-recycling wait (perf21)

Perf21 records the missing next-slot `vkWaitForFences` in the existing
`gpu_fence` phase. It remains nested inside `finish_wait`; overlapping phases
must not be added or called GPU execution time. A one-time opt-in message
identifies the revised coverage. The 43-phase schema, submission order,
fence waits, resource lifetimes and cubemap defaults are unchanged. Profiling
off takes no timestamp or extra log. This diagnostic will separate fence
waiting from the remaining CPU work before changing scheduling. CI, artifact
checks and a fresh guarded capture are pending.

Source `1f92fbdb17926c828f67020c8016f9322f45fa74` succeeds in
[CI run 38077923995](https://github.com/jamalnexellis/X1Box/actions/runs/38077923995).
The downloaded version `1.2.9-pgr2-perf21` (50) has APK SHA-256
`ce65c41c1995f71e977b620f0791b4b690a2033e5d3b50b05b666d0d2b6858b5`.
It matches CI's checksum, keeps the previous certificate identity and contains
the slot-fence coverage message and existing split-frame/cubemap diagnostics.
Android compilation and host fixtures pass. The guarded three-minute runner
requires a new absolute availability deadline, actual runtime coverage, current
state backup and independent restoration verification. Its private analysis
only attributes fully contained same-thread slow fence intervals; incomplete
coverage, short waits, the shared quota and overlapping thread totals cannot
be presented as pure CPU/GPU execution time. No perf21 install or capture has
occurred yet; fresh device availability is requested after artifact checks.

The fresh perf21 capture installs successfully. The first runner stops at its
remaining-time guard before any mutation; a shorter 20-second movement uses
the same original three-minute deadline, with restoration time reserved.
Runtime confirms two submission slots, the GPU cubemap path and the revised
fence coverage. All three images are reviewed: neutral stopped initial state,
then different scenery and 40 km/h after acceleration, with coherent car,
buildings, road and HUD. Resume and accelerator inputs respond; this is not
a complete control or game-compatibility test. Current archive, HDD, EEPROM,
all ten originally empty properties and ownership are independently verified
after app-UID restoration. The emulator is stopped and phone released before
the deadline.

The marked segments contain 239 initial complete flips (median 63.702 ms,
p95 76.432 ms, maximum 91.299 ms) and 301 moving flips (median 66.935 ms,
p95 91.198 ms, maximum 124.897 ms). All readback routes and shader compilation
remain zero. Pipeline creation peaks at 27.367 ms during movement.
`finish_wait` averages 25.380 ms per initial flip and 25.779 ms moving.
Fully contained same-thread slow fence intervals cover 99.258% of the time
in 220 logged slow preparation parents initially, and 99.179% in 281 such
parents moving (279 have a logged child). The slow-event subset is bounded
by the shared quota and two-millisecond threshold. This attributes almost all
of the examined long preparation waits to waiting for submitted GPU work;
it is not a GPU execution-time measurement or a diagnosis of all frame costs.
Other fence waits and threads are present, so aggregate fence totals must not
be subtracted from or added to `finish_wait`. No new FPS gain is established.

The existing renderer allocates three submission slots and accepts counts up
to three. Android's default selects two on this device. A private six-minute
comparison is prepared without another native change: fixed same-scene states,
profiling off, cube copy enabled throughout and slot counts 2/3/3/2. Exact
preference fixtures differ only in `submit_frames`, are checksum-guarded and
copied as the app UID. Native startup must confirm each count. A final opt-in
three-slot movement trace measures the fence wait if sufficient restoration
time remains. All benchmark/trace images require review, alongside AI traffic,
cache and thermal limits. Defaults remain unchanged and fresh availability is
required; increased queue depth is a hypothesis, not an accepted improvement.

The fresh six-minute comparison completes all four profile-off captures in
the same perf21 APK, ordered 2/3/3/2, with cube copy enabled throughout. Each
25-second capture supplies three full five-second intervals after warmup.
Two-slot runs give 14.584 and 14.809 FPS; three-slot runs give 13.474 and
13.364 FPS. Pooled flips/time are 444/30,211 ms = 14.697 FPS versus
405/30,180 ms = 13.419 FPS: three slots are 8.690% slower. Within-mode
run ranges are 1.530% and 0.819%, respectively. This is a rejection of the
three-slot change in this scene, not a claim about every game. Runtime slot
counts, actual cube-copy engagement, neutral stationary controls and matching
car/camera/road are checked. All eleven benchmark/trace images are reviewed;
movement reaches 68 km/h with coherent new scenery and released input.
Thermal status is zero throughout, but that does not establish fixed clocks;
AI traffic, cache history and short sample duration remain limits. Current
archive/HDD/EEPROM, ownership and all ten original properties are independently
verified after app-UID restoration, with emulator stopped and phone released
more than a minute before the original deadline. No default is changed.

The profile-on three-slot trace has 211 initial complete flips (median
72.894 ms, p95 83.260 ms, maximum 163.162 ms) and 280 moving flips (median
71.329 ms, p95 96.037 ms, maximum 133.746 ms). Slot preparation is almost
gone: `finish_wait` averages 0.005 and 0.075 ms per flip. Instead,
`tex_reuse_wait` averages 30.398 and 30.816 ms, containing `flush_wait`
at 30.356 and 30.785 ms. These are overlapping host scopes, not additive
GPU execution times. Readback routes and shader compilation stay zero;
movement pipeline creation peaks at 36.053 ms. Increasing queue depth moves
the blocking point into texture reuse and fails the measured FPS comparison.

## Ordering compatible 2D surface refreshes on the GPU (perf22)

The cached 2D surface-copy branch waits for all submissions before refreshing
an image, even though the allocation and descriptors remain stable. Perf22
adds an Android opt-in `debug.xemu.vk.surf_copy_queue=1` candidate that records
an in-place refresh with explicit same-queue prior-reader/write barriers,
restores the source layout and makes the destination shader-readable. Prior
queued draws are recorded first. This uses the synchronization scopes defined
by the [Vulkan synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html),
not submission order alone. The candidate accepts only active initialized
color surfaces, exact native RGB565/RGBA8/BGRA8 formats, identical dimensions,
one mip/layer, 2D depth-one textures, 1x scale, no border/cubemap, distinct
images and no pending upload/download. Depth/format/geometry mismatches,
direct descriptors, CPU uploads and retirement retain their existing paths.
Eligibility is rechecked after prior draw emission; losing eligibility restores
the ordinary wait before fallback. Default is off and submission depth remains
two. Bounded shape-only slow-wait logs distinguish remaining color/depth/CPU
routes without exposing memory addresses or texture contents.

The shared production barrier helper is covered by generated exact-pixel
fixtures for three queued versions of the same 2D destination, with prior
transfer reads and no host wait between submissions. RGB565, RGBA8 and BGRA8
are exercised at 16x16 and 640x480, alongside existing six-face cube tests.
A separate policy fixture rejects incompatible states. Software-driver
synchronization validation, Android compilation, artifact inspection and
actual Adreno/game/FPS checks are pending; transfer-read fixtures do not
constitute complete shader-reader or game compatibility validation.

Source `cf433b3241fb61730ee7b1fbf47b65f8612c7967` succeeds in
[APK CI run 38080413502](https://github.com/jamalnexellis/X1Box/actions/runs/38080413502)
and [standalone Android fixture run 38080413576](https://github.com/jamalnexellis/X1Box/actions/runs/38080413576).
Host policy/synchronization tests and Android native compilation pass.
The downloaded `1.2.9-pgr2-perf22` (51) has SHA-256
`3c876a35f5a8e4ecabd13e09af52ed0b4b2556af9129247a4c751991fbc5f76f`.
It matches CI and the delivered copy, preserves certificate identity and
contains the candidate, reuse-wait shape logs and previous 43-phase/fence
coverage. This inspection checks certificate identity, not the full APK
content signature; Android installation and actual Adreno/game checks remain
pending. A seven-minute guarded test is prepared with eleven-property current
state preservation, generated pixel fixtures on the Adreno, candidate movement
diagnosis and a profile-off comparison of two slots/copy off versus three
slots/copy on, ordered baseline/candidate/candidate/baseline. Cube copy stays
on for both modes. Any gain would belong to that combination relative to
this experimental baseline, not either change alone or the stock application.
All eleven images require review, three full FPS intervals per capture and
independent current-state restoration before the one absolute deadline.
Fresh availability is required; no perf22 device or FPS result is claimed yet.

The subsequent guarded device run installs perf22 successfully and passes
the policy and all six native-color 2D pixel fixtures on the Adreno 830,
alongside both cube fixtures. Device synchronization validation is not enabled.
The candidate movement trace uses three slots, profile on and cube copy on:
224 initial complete flips (median 69.497 ms, p95 82.681 ms, maximum 98.362 ms)
and 287 moving flips (median 70.011 ms, p95 97.839 ms, maximum 133.912 ms).
All three images are reviewed, with coherent road/buildings/HUD, neutral
stationary controls and movement to 62 km/h after releasing the throttle.
This is a limited scene/input check, not complete game or touch compatibility.
The queued 2D candidate never engages, so the runner skips the FPS comparison;
no FPS improvement is established. Strict phase joining and runtime checks
pass. `tex_reuse_wait` averages 30.402 ms per initial flip and 30.042 ms moving,
containing `flush_wait` at 30.367/30.015 ms. These overlapping host scopes must
not be added or interpreted as pure GPU execution time. Readback routes and
shader compilation remain zero. The bounded slow-wait metadata repeatedly
identifies a 1280x480, one-mip color texture, source BGRA8 UNORM (44) and
destination RGBA8 UNORM (37), excluded by the exact-format policy. All eleven
originally empty properties, current archive/HDD/EEPROM hashes and app
ownership are independently verified after restoration; the emulator is
stopped and the phone released well before the absolute deadline.

## Queued byte-preserving BGRA/RGBA refresh (perf23)

The existing surface-to-texture fallback already uses `vkCmdCopyImage` for
this BGRA/RGBA pair. Its bytes are reinterpreted, not converted or channel
swizzled. The [Vulkan copy specification](https://docs.vulkan.org/spec/latest/chapters/copies.html)
permits size-compatible color copies. Perf23 extends the opt-in queued-copy
policy only to RGBA8/BGRA8 UNORM pairs in either direction, retaining the
existing image/view formats, component mappings, stable resources, prior-user
barriers, fallback and strict geometry/lifetime constraints. RGB565 still
requires an exact format match; depth, integer, sRGB, other packed formats and
CPU uploads remain excluded. A separate engagement log identifies the format
pair and dimensions without guest addresses or texture contents. Default
remains off and submission depth remains two.

The generated fixture adds both cross-format directions at 16x16, 640x480 and
the observed 1280x480, each with three queued versions and prior transfer
reads. Byte-exact comparison detects unwanted red/blue swapping. The shared
production format predicate also rejects unsupported and mixed-size pairs.
Host synchronization validation, Android compilation and new artifact/device
checks are pending. Prior transfer reads do not cover every shader-reader
scenario. This is a targeted candidate, not a demonstrated FPS improvement.

Source `a316bbbd692ddc73820a0ad1a52ea5398fae2fa0` succeeds in
[APK CI run 38094774704](https://github.com/jamalnexellis/X1Box/actions/runs/38094774704)
and [standalone Android fixture run 38094774552](https://github.com/jamalnexellis/X1Box/actions/runs/38094774552).
The host policy/regression tests, byte-exact mixed-format fixture and software
driver synchronization validation pass. The downloaded APK (52,
`1.2.9-pgr2-perf23`) is 35,688,821 bytes with SHA-256
`683654d51b296f6306a5a643211bbab6bd6ddcbd33f6566e8d9af92618bf1fde`.
CI checksum, manifest/package, native engagement/format diagnostics,
43-phase/fence coverage and prior certificate identity match. Certificate
inspection does not verify the entire APK content signature. Standalone
Android AARCH64/PIE fixtures match their CI checksums. Neither perf23 Android
installation, actual Adreno mixed-format output nor game FPS is checked yet.

The next guarded seven-minute test requires fresh phone availability and
preserves current state and all eleven properties. It first tests the generated
mixed-format fixtures on the Adreno, then captures an opt-in two-slot movement
trace and checks actual BGRA8-to-RGBA8 1280x480 engagement and host-wait bypass.
The profile-off comparison isolates only the surface queue property, off/on/on/off,
with two slots and cube copy on throughout. This retains an experimental cube
baseline; it does not compare against stock. Three full five-second windows
per capture, all eleven reviewed images, runtime/input/mode checks and exact
current archive/HDD/EEPROM/property/app-UID restoration remain required.

The subsequent guarded perf23 device run installs successfully and passes the
policy, cube, six native-color and six mixed BGRA/RGBA generated pixel cases
on the Adreno 830, including both directions at 1280x480. Device synchronization
validation is not enabled. The profile-on two-slot trace contains 249 initial
complete flips (median 60.811 ms, p95 73.210 ms, maximum 88.568 ms) and 363
moving flips (median 56.022 ms, p95 77.773 ms, maximum 119.521 ms). All three
images are reviewed: stationary neutral controls and coherent road/buildings/HUD,
then movement to 49 km/h with released throttle. This checks a limited scene
and A/RT inputs, not every control or game scenario. The queued candidate does
not engage, so the isolated FPS comparison is skipped and no gain is accepted.
Readback routes and shader compilation remain zero. Unlike the earlier
three-slot trace, `tex_reuse_wait` is zero throughout; `finish_wait` averages
26.607 ms per initial flip and 18.153 ms moving. These host waits overlap fence
measurements and are not pure GPU execution time. Thermal status is zero,
which does not establish fixed clocks. Runtime mode/controller/43-phase
checks pass. Current archive/HDD/EEPROM, all eleven originally empty properties
and app ownership are independently verified after restoration, with emulator
stopped and phone released before the original deadline.

## Correcting linear-border eligibility and exposing rejection reasons (perf24)

Code inspection finds another incorrect restriction in the candidate: it uses
the raw NV2A border-source flag as an image-storage-border test. Existing
`texture_can_compute_upload`, texture layout and fragment-state code already
distinguish these: linear textures use their explicit rectangle/pitch, while
only non-linear textures receive the virtual storage border. Perf24 normalizes
this flag with a shared pure predicate for the queued-copy policy. Exact
source/allocated destination dimensions, one mip/layer, native or permitted
byte-compatible format pairs, stable resources and all pending/lifetime checks
still apply. Non-linear storage borders remain rejected. This corrects a code
restriction; perf23 logs do not establish it as the actual game rejection cause.

Inspection also finds that the policy compares the allocation against the
base format table instead of `texture_format_for_shape`. Compute upload can
select RGBA8 storage with an adjusted component mapping for a BGRA8 texture;
the observed BGRA8-source/RGBA8-destination pair is consistent with this path.
Perf24 checks the effective format selected for the cached shape/address.
It preserves the existing allocation/view/component mapping and byte-copy
semantics. A different effective format or allocation mismatch still rejects
the candidate. Runtime rejection/engagement logs remain necessary to establish
which conditions apply to the actual game texture.

Engagement diagnostics also retain up to eight distinct mixed-format shapes,
so an early boot/menu copy cannot hide the later 1280x480 target behind a
single first-use log. The cached-wait bypass marker is emitted only after
the copy function confirms successful queued recording following its state
recheck; a rejected recheck/fallback does not count as an actual bypass.

The policy now exposes geometry/border/source/pending/format-allocation/image
rejection bits, with a separate layout bit. Opt-in profile-on logs are bounded
to 24 changing shape/reason entries and print only shape/format/state flags,
without guest addresses, image handles or pixels. This makes remaining failed
eligibility observable instead of inferring it from non-engagement. Policy
fixtures cover linear border flags, retained non-linear rejection and combined
reason bits. GPU barriers and copy helper are unchanged. Defaults remain off
and two slots. Host checks and Android compilation pass; actual-device checks
remain pending.

The final perf24 APK is built from
`a4682b4ce2ffb912c6a31fd0a84cddb86f5cb2a6` by successful
[CI run 38096644506](https://github.com/jamalnexellis/X1Box/actions/runs/38096644506).
It is 35,688,833 bytes, with SHA-256
`00ee7fa0b5db01578efa649b7be1f5328e0f1acee20c6c4208f4f594927e6eba`.
Manifest inspection confirms development package version 53 /
`1.2.9-pgr2-perf24`; native inspection confirms the 43-phase/fence coverage,
copy/rejection/bypass diagnostics and matching development certificate
identity. Certificate inspection does not verify the APK payload signature;
Android update installation remains part of the device test. The standalone
Android fixture source is unchanged from its successful build at `ab84467`.

The next planned comparison should distinguish queue depth and copy rather
than assuming a two-slot texture-reuse wait: perf23 has no such wait. A guarded
nine-minute test is prepared to diagnose the three-slot queued candidate
and compare all four fixed-scene profile-off combinations, twice each, in
symmetric order: 2/off, 3/off, 2/on, 3/on, 3/on, 2/on, 3/off, 2/off.
Cube copy stays on. The on modes require actual target-format copy engagement;
the three-slot on mode additionally requires the cached host-wait bypass.
All images/runtime checks and exact current-state restoration remain required.
This is preparation, not a new result or accepted FPS improvement.

The subsequent perf24 Adreno 830 run installs successfully, passes the generated
policy/cube/native/mixed-format pixel fixtures and engages both the exact
BGRA8-to-RGBA8 1280x480 queued copy and the cached host-wait bypass. Device
synchronization validation is not enabled. Unsupported R8/depth formats keep
their ordinary fallback. The initial profile trace has 271 complete flips
(median 51.728 ms, p95 73.568 ms, maximum 87.988 ms); movement has 328
(59.544, 89.645 and 121.571 ms). Texture-reuse waits average 0.139/0.154 ms
per initial/moving flip, and finish waits 0.739/0.362 ms. Readback routes and
shader compilation remain zero. Moving texture decoding peaks at 1.294 ms;
pipeline creation still peaks at 27.641 ms. Host display-present scopes average
20.332/22.364 ms per flip; these overlapping scopes are not GPU execution time.

All eight profile-off captures complete in the planned symmetric order, each
with three full five-second post-warmup intervals. The pooled figures are:

| Slots / queued copy | Flips / milliseconds | FPS | Two run FPS | Run range |
| --- | --- | --- | --- | --- |
| 2 / off | 449 / 30,199 | 14.868 | 14.951 / 14.785 | 1.118% |
| 3 / off | 414 / 30,105 | 13.752 | 13.757 / 13.747 | 0.073% |
| 2 / on | 495 / 30,196 | 16.393 | 14.959 / 17.829 | 17.506% |
| 3 / on | 504 / 30,263 | 16.654 | 17.652 / 15.657 | 11.977% |

Copy-on versus off at three slots is +21.104%; both on runs exceed both off
runs. Compared with two/off, the combined change is +12.012%, similar to the
three/on run spread. At two slots, the +10.256% pooled difference is smaller
than its 17.506% spread. Three slots without copy remain slower (-7.507%).
Cube copy is constant and experimental, so these are not stock comparisons.
AI traffic, cache history, clocks and short real-time windows remain
uncontrolled. The high variation requires further investigation; no general
FPS claim or default promotion follows from these figures.

All nineteen images are reviewed with coherent buildings/road/HUD, stationary
neutral controls and limited A/RT movement to 14 km/h. All runtime modes and
controller checks pass, with thermal status zero throughout; this does not
verify every control, full-game compatibility or fixed clocks. The then-current
archive/HDD/EEPROM, all eleven originally empty properties and app ownership
are independently verified after restoration. The emulator is stopped and the
phone is released before the original nine-minute deadline.

At the user's request, the fork's Android APK, Android fixture and CI workflows
are manually disabled. Subsequent builds and checks must run locally, with
Judge used for independent review. Judge preflight finds only Codex; its
single-author-engine guard leaves native review pending a different authorized
model or an available Claude installation. This is not a Judge approval.

A local Docker Linux toolchain is subsequently prepared with Java 25,
Android platform 36/build-tools 36.1.0/NDK 30.0.15729638-beta2/CMake 3.22.1
and Rust 1.99.0. Without network access and with repository input mounted
read-only, local native checks pass: sanitizer-enabled media/uniform/presentation/
submission/copy-policy tests, production copy pixel fixtures with llvmpipe
synchronization validation, and texture-upload fixture compilation. This
checks the replacement test environment; a full local APK build has not yet
been run, and the installed development APK's signing key is not present
locally. The installed app and its data are retained.

## Perf25: bound Android native presentation requests

Offline analysis of the perf24 profile trace finds approximately 74--92 native
presentation calls per second, or 4.5--5.4 calls per complete guest flip, in
successive five-second windows. The stationary windows slow from 19.192 to
16.935 profiled flips/second with zero texture decodes and zero shader
compilations. That decline cannot be attributed to new texture decoding in
those windows. Pipeline creation occurs only once in the middle stationary
window, taking 0.343 ms in total. This does not identify the whole slowdown's
cause: host scopes overlap, profiling affects performance and presentation
source keys were not logged. The calls are not proven identical images.

The native Vulkan branch of `xemu_android_display_loop` requests scanout on
every SDL iteration, followed by a 1 ms delay; unlike the GL branch, it does
not apply the configured presentation interval. Each scanout request kicks
PFIFO, queues a render-thread display command and waits for completion. The
renderer can acquire, draw, submit and present again before the next guest
flip. Perf17's stricter identical-image reuse experiment did not engage in
its capture and does not resolve this request cadence.

Perf25 adds an opt-in `debug.xemu.vk.present_pace=1` experiment that limits
native scanout requests to the existing Android target presentation interval
(60 Hz by default). SDL input polling, menu/snapshot request servicing and
the loop's 1 ms delay remain outside the cadence condition. Late requests do
not trigger catch-up bursts; pause/hidden paths reset the request time. Guest
flip/vblank timing, GPU barriers and correctness waits are unchanged. The
property is read at display-loop startup and defaults off. Startup logging
identifies the actual mode and target rate. This is a candidate to measure,
not a proven performance gain or a default change.

Host ASan/UBSan timing checks pass for the 1 ms polling timeline, deadline
boundaries, a slow request without catch-up bursts, resume/backward clock and
a 120 Hz configured interval. A full local Android build is being prepared;
independent Judge review and device correctness/FPS checks remain pending.
The phone remains released, with no new install or setting changes.

Local debug builds can select `XEMU_LOCAL_TEST_APK=1` to use the separate
`com.izzy2lost.x1box.dev.local` application ID. The normal debug package
remains `.dev`. This permits a persistent local development signing key
without replacing/uninstalling the existing CI-signed app or clearing its
data. No local package is installed as part of compilation.

The local perf25 build subsequently succeeds in 7 minutes 16 seconds, producing
version 54 / `1.2.9-pgr2-perf25`, package `.dev.local`, 35,685,661 bytes and
SHA-256 `4e0a663bb67ea561dde7e8dacd6cf65f2a356c5255e97122c97bbc8189c92bfc`.
`apksigner verify` passes APK v2 payload verification; its local certificate
SHA-256 is `8af550a32fc9d6f2a7b96bff0556cc30d461a96387ebd686475582c339951f29`.
Manifest/native inspection confirms the new pacing property/startup log and
retained 43-phase/copy diagnostics. The ARM64 pacing test target also compiles;
it is not executed on the released phone. Source snapshot hashes for all five
changed build/native/test inputs match the host checkout. This is an
uncommitted review candidate; Judge review, device compatibility and sustained
FPS measurement remain pending. No CI/CD or phone operation is used.

The user subsequently cancels Judge and requests continued testing. The
independent-review/model-selection gate is withdrawn; device compatibility
and sustained FPS measurement are still required before accepting a gain.
CI/CD remains disabled and further builds/tests stay local.

### Perf25 device comparison

The locally signed APK is installed alongside the existing development app;
the original package and `/sdcard/Xbox` files are retained. The timing test
executes successfully on the phone's ARM64 CPU. Test-package files are staged
with archive-owner restoration disabled and their app UID is checked before
launch. Both measurement modes use the same APK, immutable save-state/cache
fixture, Vulkan renderer, three submission slots and enabled cube/queued-copy
experiments. Graphics/CPU profiling and identical-presentation reuse remain
off. Actual startup/copy/controller flags are checked per emulator PID.

The first OFF capture stays in the game's pause menu and is excluded from
the race comparison after image review. A longer A press before measurement
reliably resumes the subsequent OFF runs. Four valid stationary race captures
use ON/OFF/OFF/ON order, with 60 seconds for the first adjacent pair and 40
seconds for the reverse pair. The analyzer filters the recorded emulator PID,
requires both capture markers and complete five-second intervals starting at
least five seconds after capture begins, and rejects incomplete/mixed modes.

| Presentation pacing | First run FPS | Second run FPS | Pooled FPS |
| --- | ---: | ---: | ---: |
| OFF | 14.749 | 15.347 | 14.974 |
| ON | 19.302 | 18.260 | 18.910 |

This is a measured 26.29% improvement for the tested stationary Florence
scene. The two adjacent comparisons improve by 30.86% and 18.97%; restricting
every run to its first six accepted windows gives 24.93%. The range between
run means is 3.97% for OFF and 5.55% for ON. The last three windows average
19.058 versus 14.025 FPS in the minute-long pair and 16.670 versus 14.355 in
the shorter pair. A separate, earlier ON run is not pooled into this balanced
comparison. These measurements do not prove fixed clocks or a general gain:
the ON runs still vary, AI traffic and driver caches are not frozen, and the
test does not reproduce loading new scenery during continuous driving.

All fourteen captured images are reviewed. The valid race runs show the same
car position at zero speed, advancing game time, neutral controls and coherent
buildings, road and HUD. A and a separate four-second RT check respond; the
car reaches 33 km/h with the touch released. This is limited control/visual
coverage, not a complete controller or full-game compatibility test. Thermal
status is zero at every measured start/end, without asserting fixed clocks.

The then-current original HDD/EEPROM hashes, twelve properties and EEPROM
ownership are independently verified after restoration. Both emulator
processes are stopped and no guest-debugger forward remains. The phone is
released at 03:40:42 Madrid, before the original 03:41:37 deadline. Pacing
remains opt-in while sustained driving and newly appearing scenery are tested.
No CI/CD workflow, Judge review or private guest-data upload is used.

### Perf26: scaled GPU copies and moving-race diagnostics

An opt-in `debug.xemu.vk.scaled_copy=1` extends the existing cube/queued-color
copy experiments to matching 2x/3x/4x allocations (maximum copy dimension
4096). Surface bindings retain their actual image extents across migration;
the image pool also uses those extents when retiring an allocation. All six
cube faces must match the current scale. Scaled GPU cubes have separate
texture cache keys and allocations from native CPU fallback cubes. Guest
VRAM addresses, face strides and sizes remain native. Copy regions use actual
image dimensions, with the existing queue dependencies preserved. The new
property defaults off.

Host ASan/UBSan policy checks reject stale or mismatched allocations, invalid
scales and overflowing dimensions. Generated pixel fixtures pass local Vulkan
synchronization validation and run successfully on the phone's Adreno 830:
512/768-square RGB565 cubes with a queued face replacement, 1920x1440 color
copies, and 2560x960/3840x1440 RGBA/BGRA byte-preserving copies with prior reads
and three queued versions. These fixtures do not establish game compatibility.

The local build produces version 55 / `1.2.9-pgr2-perf26`, package `.dev.local`,
APK SHA-256 `7ef29eb79e1eed09e55d078c53a9578a3da9663a0474e27d962716eb04b0de66`.
APK v2 verification passes with the same local certificate as perf25. Actual
game startup confirms a 1920x1440 scanout source, 3840x1440 queued color copies
and 768x768 six-face copies at 3x. The output screen remains 2340x1080; internal
resolution and display stretching are separate measurements.

A private race-start state is created with three AI rivals visible. A bounded
shell touch helper accelerates the car with independent trigger/stick pointers,
checks the test activity's foreground state, and releases both inputs at the
end. Short 12-second driving captures reach the first corner. The enabled 3x
capture has two complete five-second flip windows around 21-22 FPS. These are
functional samples, not a sustained or balanced performance comparison. The
disabled route shows conspicuous stretched geometry/reflections, and artifacts
also appear just after restoring the new state in the enabled route. A later
enabled image is coherent, but this does not establish a complete graphics fix.
Neither original game speed nor full-circuit compatibility is achieved.

A separate profiled 1x driving capture contains 302 complete frames: median
41.775 ms, p95 59.162 ms and maximum 148.121 ms. Texture reuse waits total
2.832 seconds (605 calls), whereas decoding totals 28.675 ms. Depth D16-to-R16
copies and R8 color copies are among the logged fallback routes. Vertex-related
finish time totals 334.522 ms. These host wall times overlap/nest and include
profiling overhead; they are diagnostic evidence, not additive GPU timings or
an FPS gain. The next candidate should address the remaining texture reuse
dependencies rather than assume CPU decoding is dominant.

Both installations' current states are preserved before testing. The test
package uses app-owned directory backups and a read-only link to its existing
disc; the original package is not replaced. Both app states and all thirteen
diagnostic properties are restored and hash-checked; EEPROM ownership is
retained. The phone is released at 16:57:16 Madrid within the ten-minute window.
CI/CD remains disabled. No private game content is included in the fork.

### Perf27: queued D16 copies and fresh race restarts

The next experiment targets a measured reuse dependency: D16 depth surfaces
copied into R16 textures. `debug.xemu.vk.depth_copy_queue=1` defaults off and
only bypasses the host reuse wait for matching, initialized, active 2D images
with one level/layer, no pending upload/download, and sufficient GPU scratch
capacity. Native sizes and actual allocated extents must agree; scaled copies
also require the perf26 opt-in. Depth/stencil conversions keep their ordinary
fallback. Deferred draws are flushed before eligibility is checked again.

The shared production primitive copies depth image -> scratch buffer -> color
image. It preserves bits, orders prior destination/scratch users, and restores
the source layout and destination shader layout. It does not issue an invalid
depth-to-color image copy or omit queue dependencies. Native R8 color copies
also join the existing queued-color whitelist under that separate opt-in.

Host ASan/UBSan eligibility checks and Vulkan synchronization validation pass.
The generated fixtures pass on Adreno 830 as well: D16/R16 copies at 16-square,
1024-square and 3072-square, plus R8 copies at 256-square and 768-square. Each
uses three queued versions with prior destination reads and compares output
pixels exactly. Existing cube and color-copy fixtures also pass. These are
correctness checks, not performance measurements.

The local build produces version 56 / `1.2.9-pgr2-perf27`, package `.dev.local`,
APK SHA-256 `2e748e5df396bc899a9c62ae188f4a2988e4d4792910d3b1e137c1be53117b86`.
APK v2 verification passes with the same local signing certificate. The phone
logs confirm the new route at 1024x1024 (1x) and 3072x3072 (3x), and the 3x
scanout source is 1920x1440. This retains the game's 4:3 internal aspect; the
2340x1080 Android output is a separate measurement.

Tests restore the immutable older Slot 2 and restart the race through the
game menu, avoiding the stretched geometry seen when loading the newer perf26
race-start snapshot. The first private script attempt remains paused because
it failed to execute the restart; that attempt is discarded. After correcting
the script, both 1x modes and the 3x enabled mode show fresh countdowns,
advancing lap clocks, the player accelerating and three moving AI rivals.
Buildings, road and HUD are coherent in the reviewed fresh-race captures.
This narrows the earlier visual problem to a reproduction involving the new
snapshot; it does not diagnose its cause or prove full-game graphics correctness.

Profiling is off during the short driving samples. Five complete five-second
flip intervals inside the input markers average 27.495 FPS for depth-copy ON
at 1x and 25.992 for OFF. Four complete intervals average 26.647 FPS for ON at
3x. These are descriptive functional samples, not an accepted sustained gain:
there is only one ON/OFF pair, automated steering hits barriers, and the 3x
steering plan differs. The early straight shows moving rivals, but the later
scene becomes unsuitable for a matched clean-race comparison. Thermal status
is zero at the captured ends, without implying fixed clocks. An entire clean
lap, calibrated game-speed measurement and sustained 3x performance remain
unverified; the experiment stays opt-in.

Both installations' current states and all fourteen diagnostic properties are
restored. HDD/EEPROM hashes, local app data hashes and original EEPROM ownership
are verified. The emulators are stopped with no diagnostic forward remaining.
The phone is released at 17:22:01 Madrid, before the ten-minute window ends.
All work is local; CI/CD stays disabled and no Judge workflow is used.

### Perf28: retained depth views and queued integer D24/S8 packing

The ordinary depth/stencil texture-copy path previously destroyed its sampled
depth image view before submitting commands that reference it. A surface now
owns one lazily created depth-only view, transfers ownership when its image
migrates, and retires the view with the image after the existing GPU drain.
This fixes resource lifetime and removes repeated view creation; it is not a
confirmed explanation for the artifacts in the newer race-start snapshot.

The opt-in `debug.xemu.vk.zeta_copy_queue=1` adds a queued D24/S8-to-RGBA/BGRA
path. It copies both aspects into aligned GPU scratch storage, packs the raw
24-bit depth and eight-bit stencil with the existing integer compute shader,
and copies the packed bytes into the destination image. Prior image/buffer
users, dispatch and transfer access, subsequent scratch reuse, and final image
layouts retain explicit dependencies on the same queue. Descriptor exhaustion
still drains pending commands before descriptor reuse. It does not alter
guest instruction timing, omit rendering, or match a game ID.

Eligibility requires active initialized images, no pending CPU upload or
download, exact native and allocated dimensions, one level/layer, compatible
formats, distinct images/buffers, and sufficient aligned scratch capacity.
The stencil word reads require a pixel count divisible by four; incompatible
cases use the existing path. D32 depth conversion also retains the existing
path with the corrected view lifetime. Compute workgroup selection now obeys
both the per-axis size and total invocation limit of the actual GPU, and the
new queued policy also checks the dispatch group count. The experiment
defaults off.

Host ASan/UBSan policy checks and Vulkan synchronization validation pass.
Generated pixel tests also pass on the Adreno 830: three queued D24 versions
at 16x16, 1280x480, 3840x1440 and 3072x3072, with prior destination reads and
exact packed-byte comparisons. Separate D24/D32 view tests exercise 64 cache
lookups, ownership migration, three queued depth samples, and retirement after
completion. Existing cube/color/D16 tests pass. These checks cover the shared
production primitives and shader; they do not establish full-game graphics
correctness or a performance gain.

The local build produces version 57 / `1.2.9-pgr2-perf28`, package `.dev.local`,
APK SHA-256 `8d41022ceea71b9b01eca7df582173e739914794aa8e8f0ab9207c5cfbf7633c`.
APK v2 signature and manifest verification pass with the existing local
certificate. Actual startup confirms the D24 queued path at 1280x480 (1x)
and 3840x1440 (3x), with a 1920x1440 scanout source at 3x and 2340x1080 Android
output. Internal allocation resolution and Android output remain distinct.

Four 24-second driving captures use ON/OFF/OFF/ON order at 3x with the other
copy/pacing settings constant and profiling off. Each restores the immutable
older Slot 2 and restarts the race through the game menu. Four complete
five-second flip windows wholly inside each input marker give:

| D24 queued copy | First descriptive FPS | Second descriptive FPS | Pooled FPS |
| --- | ---: | ---: | ---: |
| ON | 25.654 | 25.349 | 25.501 |
| OFF | 26.261 | 26.093 | 26.177 |

This is not an accepted gain. The player and three rivals move in the first
straight; later trajectories differ, with the first adjacent pair ending near
the right barrier and the reverse pair on the second straight. Buildings,
road and HUD are coherent in the reviewed mid/turn/end images. These short
samples do not prove sustained speed or full-game graphics correctness. The
D24 experiment remains off by default.

Screenshot acquisition now has before/after device timestamps around the
capture itself, excluding host transfer time from the interval. From the mid
to end images, ON lap-clock delta divided by acquisition-elapsed bounds is
0.966-1.057 and 0.951-1.066 in the two runs. This does not establish 100%:
the intervals are short, acquisition takes roughly a second, displayed-frame
latency and guest timer behavior remain additional limits, and no clean full
lap is measured. Longer capture attempts are rejected by the private deadline
guard before starting so that restoration remains inside the agreed window.

Separate profiled 1x ON/OFF driving traces contain 660/651 complete frames.
Neither records a texture reuse wait in its accepted movement interval. Raw
vertex-related finish totals are 734.7/695.9 ms; presentation totals are
1061.1/1077.0 ms. Texture decoding is tens of milliseconds, and pipeline
creation remains visible. These nested, overlapping host wall times include
profiling overhead and differ in the sampled trajectory. They identify
remaining synchronization candidates, not a causal or additive frame budget.
There is no measured benefit attributable to the new D24 path here.

Both installations' current app states and all fifteen diagnostic properties
are restored and hash-verified, with original EEPROM ownership retained.
The emulator processes are stopped and no guest-debugger forward remains.
The phone is released at 18:08:02 Madrid before the ten-minute deadline.
No 100% speed or full-lap graphics claim is made. CI/CD remains disabled;
builds and checks are local, without a Judge workflow or private game upload.

### Perf28 extended functional run and perf29 vertex-upload experiment

A fresh ten-minute device window completes an extended 3x functional run with
D24 queued copy off and profiling off. The scanout source remains 1920x1440.
The 363.230-second marker interval contains 72 complete five-second flip
windows averaging 26.594 FPS. The player advances through several sections,
but the short steering phases hit barriers; during capture/analysis gaps the
player coasts and eventually stops while the rivals continue. This mixed run
is not a sustained moving-race benchmark, matched comparison or complete lap.
Buildings, road and HUD are coherent in the reviewed captures, and no fatal
exception or device loss appears in the captured process log. End thermal
status is 1, unlike the status-zero ends of the earlier short samples.

Displayed lap time changes from 20.820 to 379.587 seconds between captures
whose device acquisition bounds imply a clock/wall ratio of 1.064-1.073.
The game's displayed clock is not an independent simulation-speed reference:
this result must not be reported as achieving original speed or exceeding it.
Both installations' states and all fifteen properties are restored and
verified, with the final verification completed at 18:32:12 Madrid within the
user's ten-minute window. There is no clean full-lap or 100% speed result.

Perf29 adds opt-in `debug.xemu.vk.vertex_same_bytes=1`. When an upload overlaps
vertex pages protected for earlier commands, the renderer still downloads
dirty overlapping surfaces first. It compares at most 64 KiB with the current
frame's mapped RAM buffer, and only if every affected page is already uploaded.
Exact equality returns without a host write or vertex-related finish, while
retaining the existing flush/propagation spans and command-buffer page tracking.
Changed, unuploaded or larger ranges keep the existing finish-and-upload path;
the mapped buffer is obtained again after that finish can advance the frame.
No hash match, game ID, shader omission or weakened GPU dependency is used.
The experiment defaults off. The opt-in stall profiler reports equal-data
skips and comparison fallbacks alongside the existing vertex finish timings.
The byte fields are requested upload sizes, not measured comparison bandwidth.
The initial APK check caught that the old performance-log counters were
compiled out; the final diagnostic uses the independently enabled stall profiler.

Host ASan/UBSan checks pass, including changing each byte of a 64 KiB generated
range, unaligned/end-of-allocation spans, overflow/oversize rejection, inaccessible
guard pages and read-only mappings. Vulkan synchronization validation on
llvmpipe passes with three buffer versions and comparisons before/after queue
submission; prior readbacks retain their original bytes. Existing generated
copy/depth-view fixtures also pass. These checks validate the shared comparison
primitive and buffer ordering, not full emulator compatibility or a FPS gain.

The local APK is version 58 / `1.2.9-pgr2-perf29`, package `.dev.local`, SHA-256
`6217be14876abed57533c898f2faa73ae319c73ff5885a319f42c465ef4d553c`.
Compilation, v2 signature and manifest checks pass; the native generated-data
fixtures also cross-compile for arm64. Builds/checks remain local, CI/CD
disabled and Judge unused.

### Perf29 moving device comparison: waits avoided, no FPS gain

The guarded CPU comparison fixture and generated Vulkan buffer/copy fixtures
pass on the phone's Adreno 830. A fresh ten-minute window then measures four
24-second driving starts at 3x in OFF/ON/ON/OFF order, with profiling off,
all other settings fixed and D24 queued copy off. Each launch restores the
immutable older Slot 2 and uses the game's Restart Race command. Four complete
five-second flip windows wholly inside each driving marker give:

| Exact vertex comparison | First descriptive FPS | Second descriptive FPS | Pooled FPS |
| --- | ---: | ---: | ---: |
| OFF | 27.121 | 26.401 | 26.761 |
| ON | 26.648 | 26.867 | 26.758 |

The pooled difference is -0.013%; there is no accepted FPS improvement, and
the option remains off by default. Early captures show the player and three
AI rivals moving. Reviewed mid/end images have coherent buildings, road and
HUD. Later trajectories differ and include barrier contact; these short runs
do not establish a clean full lap, sustained original speed or complete game
compatibility. Internal scanout is 1920x1440 at 3x, presented to a 2340x1080
output; this is not a measured native 1920x1080 game mode. End thermal status
is zero in all four cases. No fatal exception or device loss appears in their
captured process logs.

Separate 1x ON/OFF/ON diagnostic runs contain 690/684/690 complete frames:

| Vertex comparison | Exact-data skips | Comparison fallbacks | Vertex finishes | Total vertex finish wall time |
| --- | ---: | ---: | ---: | ---: |
| ON, first | 139 | 18 | 18 | 62.461 ms |
| OFF | 0 | 0 | 150 | 549.270 ms |
| ON, repeat | 153 | 19 | 19 | 64.053 ms |

Skipped-byte counters are 1,404,928 and 1,540,096 requested bytes, not measured
comparison bandwidth. The experiment does avoid vertex-related finishes in
this workload, but the unprofiled 3x result does not show more FPS. Diagnostic
presentation counts are 1426/1434/1445, with host wall totals of
1200.890/1205.102/1172.667 ms. First-ON pipeline creation is 535.109 ms across
40 calls; texture decoding is 32.701 ms across 269 calls. None of the three
movement traces records a texture reuse wait. These nested/overlapping host
times are not a GPU utilization measurement or an additive/causal frame budget;
profiling overhead and trajectory differences also apply. Presentation reuse
is off in all these runs, so zero reuse hits would not test that feature.

The first profiled ON mid image has an unusual dark/angular rear-car shape
whose cause is unresolved; it is not used to claim full graphics correctness.
The repeated ON captures do not reproduce that appearance. A local per-thread
CPU snapshot was taken after the driving marker ended and is excluded from
the moving benchmark. The game's displayed lap clock is also not used as an
independent simulation-speed reference.

Both installations' current states and all sixteen diagnostic properties are
restored and hash-verified. Original EEPROM ownership is retained; emulator
processes are stopped and no debugger forward remains. Final verification
completes at 20:07:35 Madrid on 2026-10-11, before the ten-minute window ends.
No 100% speed or full-game graphics claim is made.

### Perf30 per-surface native presentation identity

The opt-in native presentation cache previously included the global surface
list generation. Inserting, shelving or invalidating an unrelated offscreen
binding could therefore reject reuse even when the selected scanout was
unchanged. Perf30 replaces that part of the key with a unique 64-bit active
binding serial assigned in `surface_put`, including recycled images and
reactivated shelved bindings. Image/view identity, source address, content
generation, scanout geometry, viewport and swapchain identity still match
exactly. Pending CPU uploads, PVIDEO, captures and the 250 ms refresh deadline
still force presentation. Only a successful present commits a cache key.
The serial counter persists through surface flushes; exhaustion returns zero
and disables reuse rather than allowing an old identity to reappear.

With both `debug.xemu.vk.present_reuse=1` and the stall profiler enabled,
`XEMU_PRESENT_REUSE` reports five-second decision totals and overlapping
rejection reasons: cold cache, invalid serial, upload, video, capture, clock,
source identity, content and geometry. The classifier used for these counters
is also the production decision. Profiling-off FPS comparisons do not collect
these counters. This does not change emulated flips or omit changed images.

Host ASan/UBSan policy checks pass for unchanged scanout during 1024 unrelated
binding allocations, recycled handles with otherwise identical keys, serial
exhaustion, clock extremes and all existing redraw guards. Generated Vulkan
copy/depth fixtures also pass with synchronization validation on llvmpipe.
The policy fixture does not exercise native swapchain presentation or establish
an FPS gain. The new cache policy still defaults off. A fresh moving device
comparison and graphics review are required before accepting it.

The local perf30 APK is version 59 / `1.2.9-pgr2-perf30`, package `.dev.local`,
SHA-256 `4c4ed1a28cbd5a8ea1ff80c85504acd5755e00f0c340eceb6e13f25c5863b879`.
Local compilation, v2 signature, certificate, manifest and actual arm64
diagnostic-string checks pass. The policy fixture also cross-compiles for
arm64. At this build stage the previous phone window had ended and no perf30
device run or FPS gain was claimed. The subsequent device results follow.
CI/CD remains disabled and private game data stays local.

### Perf30/31 device diagnosis: accept completed suboptimal presentations

A fresh ten-minute window installs perf30 in the separate test package and
runs its native policy fixture successfully on the phone. A profiled 3x
moving start reveals that all 1120 decisions in four complete five-second
intervals wholly within movement reject reuse for a cold cache; none records
an invalid binding serial, CPU upload, video or capture. The run contains 664
complete emulated-frame diagnostic records and 1411 native presentations.
An additional profiling-off OFF run is captured, but a complete paired FPS
comparison is not obtained in this window.

The cache only accepted `VK_SUCCESS` from `vkQueuePresentKHR`. Khronos also
classifies `VK_SUBOPTIMAL_KHR` as a successful presentation, with the same
meaning as for acquisition; the swapchain remains usable. See the primary
[Vulkan reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html).
Perf31 uses a shared explicit predicate accepting those two results. It
continues to discard all errors and other positive statuses, retaining the
existing out-of-date recreation path and bounded 250 ms refresh. It does not
treat every nonnegative result as success.

Host ASan/UBSan and the arm64 policy fixture on the phone pass both successful
results and the tested not-ready, timeout, event, incomplete, memory, device,
surface, out-of-date and fullscreen-loss results. The result predicate is the
one called by the presenter before committing its cache. The generated Vulkan
copy paths are unchanged from the fixtures already checked above.

After local build/signature/manifest/native checks, perf31 is installed in
the same separate test package while preserving the current session backup.
Its first eight profiled native presentation results are actually
`VK_SUBOPTIMAL_KHR` (1000001003), accepted with a nonzero candidate serial.
Four complete five-second decision intervals inside a new 3x moving start
then contain 1094 checks: 605 unchanged-image reuses (55.3%), no cold-cache
rejections, and 489 changed-content redraws. No source, geometry, upload,
video, capture, clock or invalid-serial rejection occurs in those four windows.
This establishes that the corrected cache is exercised, not an FPS gain.

Across the separately selected complete emulated-frame records, perf31 has
623 frames, 767 reuse events and 623 native presentations. Presentation host
wall total changes from 1622.056 ms in perf30's diagnostic run to 192.395 ms
in perf31's. GPU-fence wall totals are 784.910/1708.723 ms; median frame
intervals are 35.453/36.737 ms. These profiled runs have different trajectories
and workloads, nested/cross-thread timings and profiling overhead. Reduced
presentation work does not establish faster simulation or higher sustained
FPS. The 20-second decision windows and approximately 25-second frame-record
intervals are distinct selections; their counters must not be conflated.

Reviewed perf31 mid/end images show coherent car, road, buildings and HUD.
The player and all three AI rivals are visible moving in the mid capture;
the player reaches the next straight and the rivals advance on the minimap.
End thermal status is zero and no captured fatal exception or device loss
is found. This is a short functional check with a steering/contact limitation,
not a full-lap compatibility result. Internal scanout remains 1920x1440 and
the output 2340x1080. Presentation reuse remains off by default.

A further profiling-off perf31 run is rejected by the private remaining-time
guard before changing any property or starting input. Both app states and
all sixteen properties are restored and hash-verified at 20:30:51 Madrid
on 2026-10-11, before the fresh ten-minute deadline. Original EEPROM ownership
is retained, both emulators are stopped and the debugger forward is absent.
The phone is released; no 100% speed or accepted FPS improvement is claimed.

Perf31 is version 60 / `1.2.9-pgr2-perf31`, package `.dev.local`, local APK
SHA-256 `2432ac4ba8f5335c9b2a5b994563b28bb98a9762cb573db793f6f7656f825ea2`.
Local compilation, v2 signature/certificate, manifest and actual arm64
diagnostic-string verification pass. Builds and tests remain local, without
Judge or CI/CD dispatch, and no private game data is uploaded.

### Perf32: preserve vertex mirror history and avoid clean-page copies

Source inspection after perf31 finds a generic three-slot mirror hand-off
defect. The old rotation copies only the current slot's new bounding range
and clears that range. An initialized third slot can therefore miss a change
made two turns earlier, while receiving an uploaded bitmap that assumes the
data is current. A generated isolated history reproduces that stale byte;
this does not establish that it caused a particular PGR2 artifact or stall.

Perf32 accumulates pending changes separately for every allocated destination
mirror, marking both host-upload entry points. Rotation still waits for the
destination's GPU readers before copying from the current mirror. First use
copies the whole allocation, one-slot rotations avoid self-copy, and flushing
GPU work retains pending mirror history. Equal-data upload skips do not mark
changes. Allocation-failure cleanup and renderer teardown free the tracking.

Both copy modes use the corrected history. The default copies the pending
destination's bounding range; the opt-in `debug.xemu.vk.vertex_pages=1` copies
only coalesced pending page runs. The existing fence waits, mapped-memory
flush and host-to-vertex barrier stay in place. Tracking uses one byte per
guest page, or 16 KiB per 64 MiB mirror. With profiling enabled, independent
five-second totals report copy bytes, equivalent bounding bytes, runs and
host copy time; ordinary profiling-off runs collect no such totals.

Local ASan/UBSan checks pass 12000 rotations against a fully updated byte
oracle for one, two and three slots, including idle turns, overlaps, page
boundaries, partial tails and rejected out-of-bounds requests. A synthetic
64 MiB allocation with only its first and last pages changed copies 8192 bytes
instead of a 67108864-byte bounding range. This is a generated workload,
not a measured reduction in PGR2.

The extended Vulkan fixture passes 24 queued readback versions per slot count
and copy mode on host llvmpipe with validation enabled. Earlier snapshots
retain their exact bytes across fenced mirror recycling. Existing generated
color, cube and depth pixel/synchronization fixtures also pass. Transfer
readbacks do not test full vertex fetch, device graphics or game speed.

At this stage no perf32 phone measurement has run. The previous phone session
remains restored and released. A fresh moving-race comparison at 3x with
profiling off is required before accepting any FPS gain; sustained original
speed and full-game graphics correctness remain unverified.

Perf32 builds locally as version 61 / `1.2.9-pgr2-perf32` in the separate
`.dev.local` package. The v2 signature, local certificate, APK manifest and
actual arm64 experiment/diagnostic strings pass verification. APK SHA-256:
`6d15cb32eadc3966da694f8a32d7924d0bbc286054ead748ebe787fbc0c7e1a6`.
The policy and extended Vulkan fixtures also cross-compile for arm64. No
Judge or CI/CD pipeline is used, and no private game data is uploaded.

### Perf32 device checks and bounded moving-race comparison

A fresh ten-minute window on 2026-10-11 installs perf32 only in `.dev.local`.
The CPU propagation fixture passes all 12000 rotations on the phone. The
extended Vulkan fixture passes all six 24-version mirror histories on the
Adreno 830, as well as its generated cube, native color, byte-preserving
color and D16 copy/readback checks. Device transfer readbacks check bytes and
ordering; they do not exercise full vertex fetch or an actual game scene.

Four fresh-process 3x moving starts use the same older snapshot followed by
the game's Restart Race and the same bounded throttle/steering plan. All
other experimental settings stay fixed: presentation reuse and equal-data
vertex comparison are off; the previously checked cube/color/D16/scaled-copy
and presentation-pacing settings are on. CPU and graphics profiling are off
for these four FPS runs. Each result selects four complete roughly five-second
flip-count windows wholly inside that process's movement markers:

| Pending-page copy | First run | Repeat |
| --- | ---: | ---: |
| OFF (corrected bounding range) | 26.624 FPS | 23.946 FPS |
| ON (coalesced pending pages) | 26.949 FPS | 26.687 FPS |

OFF/ON/ON/OFF pooled descriptive results are 25.285/26.818 FPS, a 6.062%
difference. The OFF repeat varies substantially, later trajectories/contact
differ, and only a short start/first-curve interval is covered. This is not
accepted as a reliable 6% speed gain or proof of sustained original speed.

Separate profiled OFF/ON runs each provide four complete five-second copy
diagnostic intervals inside movement. OFF reports 1103 rotations, 210153472
copied/bounding bytes, 210 copy runs and 27207 microseconds of host copy time
over 20014266 microseconds. ON reports 1097 rotations, 4743168 copied bytes
versus its own 261296128-byte bounding alternative, 532 copy runs and 1423
microseconds of host copy time over 20050722 microseconds. Thus the sparse
route avoids about 98.2% of its equivalent bounding-copy bytes in this actual
capture. The two runs have different histories; their totals are not matched
per-event measurements. Even the OFF copy time is only about 0.14% of its
selected wall interval, so this reduction alone does not explain the remaining
frame-rate shortfall.

Complete emulated-frame diagnostic records cover a separate approximately
25-second selection: OFF/ON have 635/650 frames, median intervals 37.024/36.657
ms, p95 intervals 60.991/59.602 ms and maxima 107.256/95.383 ms. Host presentation
totals are 1426.595/1544.152 ms, GPU-fence totals 1353.341/1000.066 ms and pipeline
creation totals 335.050/283.759 ms. These timings can overlap or nest across
threads and are not an additive CPU/GPU utilization budget. They do not locate
the entire bottleneck or establish causality for the FPS difference.

All six reviewed mid/end image pairs show coherent player car, road, buildings
and HUD. Each mid image shows the player and three AI cars moving; later rivals
advance on the minimap. Touch steering/throttle take effect and inputs release
at the end of each plan. All six end thermal statuses are zero and captured
process logs contain no fatal exception, assertion or device loss. This remains
a limited graphics check, not full-game compatibility. Internal scanout is
1920x1440 with 2340x1080 output; no sustained 100% speed claim is made.

Both installations' HDD/preferences/EEPROM state and all seventeen diagnostic
properties are restored and verified at 21:24:50 Madrid, before the agreed
21:26:22 end. Original EEPROM ownership is retained, emulators are stopped
and the debugger forward is absent. The phone is released. Sparse copying
remains opt-in; per-destination history correctness applies in both modes.

### Perf33 render-worker drain correctness

Review after the reported instability found that
`pgraph_vk_render_thread_wait_idle` checked only the pending queue. The worker
removes a command before executing it, so an empty queue can still have an
active submission, display operation or flush. Callers use this drain before
submitting auxiliary work, recycling resources and destroying draw state.
Returning while the last command is executing defeats that synchronization.
Vulkan requires external host synchronization of queue access for this
renderer's ordinary queue; see the primary
[vkQueueSubmit reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html).
This source-level race is not yet established as the cause of PGR2's hitches.

Perf33 tracks an active command under the queue mutex from removal through
completion, including the explicit shutdown-command path. Drain waits on a
condition while either queued or active work remains. Completion broadcasts
under the same mutex, so two waiters and spurious wakeups cannot clear the
predicate. Only the actual render worker bypasses the wait, using
`qemu_thread_is_self`; a shared context flag describing another thread no
longer permits the caller to bypass synchronization. GPU completion fences
remain required at their existing callers; draining the host worker alone is
not proof that a deferred submission has completed on the GPU. This is a
generic correctness fix, with no game-name or device-specific branch.

The shared production wait passes 200 rounds with two waiting threads,
spurious broadcasts, additional queued work, the last command becoming active
and completion before a waiter starts. The old queue-only check would accept
the empty-but-active case. Separate host ASan/UBSan and ThreadSanitizer runs
pass. The arm64 native fixture also passes all 200 rounds on the connected
phone. These checks exercise host synchronization without Vulkan submissions;
they do not establish game stability, graphics correctness or a speed gain.
The local Android build, signature, certificate, package and native-library
checks pass. The APK is version 62 / `1.2.9-pgr2-perf33`, separate `.dev.local`
package, SHA-256
`3f321c7731f73f4413a8f9453a1719af4426220488a5d8161e46b3146d7901d9`.

A scheduler-trace driving test was prepared, but the phone was locked before
inputs could be sent. No new PGR2 race, frame-time comparison or FPS result was
obtained in this session. The previously reported perf32 measurements must
not be attributed to perf33. The active installation's HDD, preferences and
EEPROM hashes and all seventeen diagnostic properties were restored at
22:14:09 Madrid on 2026-10-11. The older installation was absent in Android
user 0 and present in another profile; that other profile was left untouched.
Perf33 is prepared as an APK, pending the unlocked moving-race validation.
No 100% sustained-speed claim is made; CI/CD remains disabled.

### Perf33 unlocked race and scheduler diagnosis

The next unlocked session on 2026-10-11 validates perf33 with the player and
three AI cars moving at 3x, restarting the same older state from the game.
Four unprofiled perf33/perf32/perf32/perf33 runs select four complete five-second
flip windows per process: 26.227, 25.172, 24.616 and 23.931 FPS respectively.
The pooled perf33/perf32 difference is only 0.743%, below the repeat variation
and confounded by later trajectories/contact. No reliable FPS gain is accepted.
The render-worker synchronization correction is retained for correctness.
Reviewed car, buildings, road and HUD remain coherent in these short starts;
all end thermal statuses are zero, with no captured fatal error/device loss.

A separate profiled perf33 start has 677 complete movement-frame records,
median 34.965 ms, p95 53.692 ms, p99 71.351 ms and maximum 146.891 ms. Its
longest frame overlaps a 95.940 ms shader compile, including 94.334 ms of
preprocessing. The scheduler trace records 94.182 ms of actual PFIFO-thread
execution and 0.152 ms runnable delay during that preprocessing interval;
the render worker sleeps through it. This specific hitch is compiler CPU
work, not a GPU-fence wait or scheduler starvation. Other hitches need their
own evidence. Three shader compilations occur in the selected movement span.

Over the exact 24.261-second driving interval, the vCPU thread executes for
22.079 seconds, about 91% of one CPU core; PFIFO executes for 15.148 seconds
and the render worker for 0.591 seconds. Those threads can overlap and this
is not whole-phone CPU/GPU utilization. The remaining sustained-speed
shortfall cannot be explained solely by the isolated shader initialization.
Trace parsing reports no nonzero data-loss/error stats. Raw traces, process
logs, shader caches, game data and images stay private.

The active test installation's HDD, preferences and EEPROM hashes and all
seventeen diagnostic properties are restored at 22:32:33 Madrid. The older
installation remains absent in user 0 and present in another profile, which
is untouched. The phone is released with its emulator stopped.

### Perf34 GLSL compiler startup preparation

The pinned glslang source lazily creates process-wide GLSL built-in symbol
tables in `SetupBuiltinSymbolTable` on first preprocessing; calling
`glslang_initialize_process` alone does not prepare them. A process whose
startup shaders all hit the SPIR-V cache can pay this cost at its first new
shader during gameplay. Perf34 preprocesses a minimal GLSL 450 vertex shader
after target selection and before guest commands. The real compile and the
preparation share the same Vulkan/SPIR-V target and resource inputs. No module,
pipeline or shader-cache entry is created; normal generation/validation and
compiler options are preserved. A failed preparation leaves normal compilation
available. Android `debug.xemu.vk.compiler_prewarm=0` disables it for diagnosis.
This is a generic startup change with no game or device branch.

Fresh cold/warm fixture processes check complete SPIR-V output preservation
for vertex, fragment and geometry shaders at all three supported target pairs.
All nine sizes/hashes agree on the host and arm64 phone. The first phone
preprocess measures 78.576 ms cold versus 0.195 ms after a 74.518 ms startup
preparation. Host values are 133.987/0.636 ms after 122.562 ms preparation.
This moves lazy initialization to startup; it does not remove the cost of
new shader parsing, linking, SPIR-V generation or driver pipeline creation.

Perf34 builds locally as version 63 / `1.2.9-pgr2-perf34` in `.dev.local`.
The v2 signature, existing certificate, package/version and actual arm64
preparation switch/log strings pass verification. APK SHA-256:
`7561109620f72182eb22294b2ea474c02f8a43965edd139c493cf8d67ca22487`.

### Perf34 controlled first-miss and unprofiled device checks

The unlocked 22:41-22:49 Madrid session tests only the protected `.dev.local`
copy. The user installation in the other Android profile stays untouched.
An initial OFF/ON pair uses the older seed SPIR-V cache, where preprocessing
already occurs during startup. That pair therefore cannot demonstrate removal
of lazy initialization during gameplay. A second controlled pair copies the
known private warm SPIR-V cache and temporarily omits only the module matching
the earlier 13,908-byte shader. Each run restores identical seeded module-key
and driver pipeline caches. The omitted module is not in that seed; it first
compiles while the player and rivals move. All cache manipulation is limited
to the test copy; no game file or user cache is discarded.

With preparation OFF/ON, that actual shader compile takes 103.654/5.433 ms;
its preprocessing is 101.455/1.399 ms. The compiled SPIR-V SHA-256 matches
exactly between both runs. The frame overlapping the compile is 154.365/62.512
ms. This controlled test supports removing the compiler's lazy-initialization
stall from that first gameplay miss. The work is moved to startup, where the
ON preparation takes 55.650 ms. Other long frames remain: whole selected
movement medians are 35.755/36.931 ms, p95 57.926/62.728 ms and maxima
154.365/132.728 ms across 660/637 records. The ON maximum overlaps 60.793 ms
of host `display_present`. These are nested host timings with diagnostic
overhead, different trajectories and driver behavior; they do not establish
an overall frame-time or FPS gain.

A separate unprofiled OFF/ON first-miss pair selects four complete five-second
flip windows per process, yielding 24.592/24.890 FPS. A single pair and its
1.214% difference are insufficient to accept a sustained speed gain. Six
reviewed mid/end pairs show coherent road, buildings, car and HUD; the player
and three AI cars move, then advance along the course/minimap. Throttle and
steering respond and release. All six end thermal statuses are zero, and
captured process logs have no fatal exception, assertion or device loss.
This is a short-start graphics check, not a full lap or full-game validation.
Internal scanout remains 1920x1440 at 3x, with 2340x1080 native presentation.
Sustained original speed is not verified.

The active installation's HDD/preferences/EEPROM hashes and all eighteen
diagnostic properties are restored and verified at 22:49:38 Madrid, before
the protected test deadline of 22:51:16. The emulator is stopped and the phone
is released with perf34 installed. Native fixtures, APK compilation and
verification run locally; Judge and CI/CD are not used. Private captures,
traces, game files and runtime caches are not uploaded.

### Perf35 presentation and timer diagnosis

A follow-up scheduler trace resolves the previously selected 60.793 ms
`vkQueuePresentKHR` stall. Over that interval the render worker sleeps for
60.577 ms and executes for 0.180 ms; PFIFO waits throughout while the vCPU
executes for 48.519 ms. This particular stall is native presentation blocking
the graphics command consumer, rather than render-worker CPU starvation.
It does not establish the cause of every slow frame.

Four unprofiled moving starts compare presentation reuse OFF/ON/ON/OFF at
3x, with compiler preparation and sparse vertex propagation enabled. Complete
five-second windows pool to 24.949/24.988 FPS, a 0.157% difference below repeat
variation. The reviewed starts show coherent road, buildings and HUD with
three moving rivals. Presentation reuse stays disabled by default; this
comparison provides no accepted sustained FPS gain.

Perf35 adds opt-in `XEMU_VBLANK` timer-delivery diagnostics and an optional
phase-preserving regular timer grid. Android `debug.xemu.vblank.adaptive=0`
selects it; the default retains adaptive behavior and the existing period
selection. Missed periods are skipped without a burst. PCRTC interrupt
assertion/acknowledgement, flip waits and render fences are preserved. A
30,000-callback native fixture checks reference-clock agreement, lateness,
large gaps and saturation on the sanitized host and arm64 phone. This is
an experiment, not a verified correction to Xbox hardware timing.

In separate diagnostic starts, adaptive/regular timer delivery is
55.003/59.539 Hz, with 323/0 deferred callbacks over four selected windows.
Timer delivery does not prove guest interrupt handling or simulation speed.
An unprofiled adaptive/regular/regular/adaptive comparison measures
26.904/25.093/27.550/24.954 FPS. Pooled adaptive/regular values are
25.928/26.320 FPS, a 1.513% difference below the 7-10% repeat variation.
No FPS gain is accepted and the adaptive default remains enabled. All six
reviewed diagnostic/unprofiled starts show player and three rival cars moving,
coherent road/buildings/HUD and responding throttle/steering. They cover short
starts with differing contact and trajectories, not a full lap. End thermal
statuses are zero; captured processes have no fatal exception, assertion or
Vulkan device loss. Internal scanout is 1920x1440 with native 2340x1080 output.

The protected timer-comparison session restores the active lab HDD,
preferences, EEPROM and nineteen diagnostic properties at 23:24:11 Madrid.
The subsequent CPU-profile session restores them again at 23:26:43. The
older installation remains untouched in its other Android profile.

### Perf36 generic primitive assembly

A fresh moving-race CPU-cycle sample identifies primitive index rewriting
as 10.468% of PFIFO's weighted sampled cycles. PFIFO and vCPU account for
30.646% and 47.002% of sampled process cycles respectively. These are event
shares from overlapping threads, not wall time or whole-phone utilization.
The vCPU sample also contains native dispatch work and roughly 13.6% in
a translated RAM polling loop. Skipping guest polling without proving its
synchronization semantics would be incorrect. Samples, translated game code,
maps and instruction addresses stay private. Kernel symbol access is not
needed and no device security setting is relaxed for this diagnosis.

Perf36 batches the two line or three triangle index stores and publishes
the output count once per primitive. The index allocation is separate from
the result descriptor; this removes repeated alias-dependent descriptor
loads/stores in the generated host code. Allocation, primitive topology,
winding, incomplete-tail behavior and provoking-vertex selection are
unchanged. Both OpenGL and Vulkan use this generic assembler, with no game,
device or shader-specific branch. A compile-time scalar reference is used
only by the differential fixture.

The sanitized host fixture and the Android fixture with the real QEMU/GLib
allocator compare 48,960 complete index sequences, counts and passthrough
results. Every supported primitive/polygon mode and flat/provoking setting
is covered with indexed input and draw ranges. All checks pass. Six
alternating arm64 fixture timings measure 15.623/5.393/5.202/14.261/14.186/
4.735 ms for scalar/batched/batched/scalar/scalar/batched assembly of 300
long strips. Means are 14.690/5.110 ms, about 65.2% less time in this isolated
routine. This is not a game FPS or sustained simulation-speed gain.

### Perf36 moving-race device comparison and delivery

The five-minute protected window completes three unprofiled 3x starts in
the order perf35/perf36/perf36. The final perf35 repeat is omitted to reserve
restoration time; this is not a completed ABBA comparison. Every installation
is checked against its local APK SHA-256. Compiler preparation, adaptive
VBlank, sparse vertex propagation, the cache seed and the 24-second touch
input plan are held constant; presentation reuse and diagnostics are off.
Each case loads the protected race seed and restarts the race from its game
menu, then advances the player and three AI cars.

Four complete five-second flip windows per process measure 26.473 FPS for
perf35 and 27.610/26.212 FPS for perf36. Pooled perf36 is 26.911 FPS, 1.656%
above the single baseline. This difference is below repeat variation and
the incomplete comparison cannot establish an accepted sustained FPS gain.
The reduced primitive-assembly cost is verified independently by the fixture;
its contribution to sustained gameplay remains uncertain. Original simulation
speed at Full HD or higher is still not verified.

All three mid/end capture pairs show coherent road, buildings, player car
and HUD. Three rivals move in the mid captures and advance along the course
and minimap. Throttle and steering respond and release. Game clocks advance
from 1.454/2.204/2.687 to 22.937/23.937/23.254 seconds, but differing contact,
capture timing and trajectories limit comparisons. These are short starts,
not full laps or full-game correctness checks. Captured process logs contain
no fatal exception, assertion or device loss; all end thermal statuses are
zero. Internal scanout is 1920x1440 at 3x and native output is 2340x1080.

The lab's original HDD/preferences/EEPROM hashes and all nineteen diagnostic
properties are restored and verified at 23:47:45 Madrid, before the original
23:48:49 test deadline. The emulator is stopped and perf36 remains installed.
The user installation in the other Android profile is untouched. APK/native
builds and checks run locally; Judge and CI/CD are not used. Raw captures,
profiles, translated code, game files and caches remain private.

Perf36 uses version 65 / `1.2.9-pgr2-perf36` in `.dev.local`. The v2 signature,
existing signing certificate, package/version and actual arm64 diagnostic
strings pass verification. The delivery APK matches the tested artifact;
52 selected build inputs are checked against the committed sources. APK
SHA-256: `2590b9dd79a90116703a40f56ab18ead63154f82ed7054b4dcd146c3f64ed840`.

### Perf36 follow-up with native touch timing, 12 October

The first follow-up finishes four short starts after an initial phone lock.
Review finds that a mid capture in one run still shows the race countdown:
the capture was scheduled from Windows job creation, while the asynchronous
touch helper started later. Outer shell begin/end markers also include helper
startup and teardown. Those bounds can admit extra windows outside actual
touch driving, so this repeat set is retained privately as diagnostic data
and is not used to accept an FPS gain. Its protected state and nineteen
properties are restored at 00:04:40 Madrid.

The standalone Android test helper now logs native touch begin/end markers
with `SystemClock.uptimeMillis`, after the first injection and after gesture
release. Captures wait for the native start rather than host job creation.
The analysis checks marker/uptime agreement, the installed APK hash, emulator
PID and actual input duration, then selects only complete flip windows between
those markers. Longer qualifying windows are retained to avoid hiding stalls.
The helper and this measurement correction leave the production APK at perf36.

The corrected session completes perf35/perf36/perf36 before the final perf35
reinstallation waits on a Google Play Protect dialog. Four complete windows
per process measure 26.170 FPS for perf35 and 27.137/26.491 FPS for perf36.
The pooled perf36 result is 26.814 FPS. Its eight five-second windows range
from 24.871 to 28.144 FPS; every window is below 29.7 FPS. These are window
averages, not instantaneous minima or frame-time percentiles. The incomplete
comparison and trajectory variation still prevent accepting a sustained
FPS gain. Original simulation speed and stable 30 FPS at 3x remain unverified.

All three corrected mid/end pairs show coherent road, buildings, car and HUD,
with three moving rivals in the mid captures and later course/minimap progress.
Throttle and steering respond and release. The reviewed game clocks advance
from 3.404/4.337/3.804 to 22.970/24.254/23.287 seconds; capture latency,
different contact and trajectories remain limits. All three end thermal
statuses are zero. Captured emulator processes show no fatal exception,
assertion or device loss. Internal scanout is 1920x1440 at 3x, with native
2340x1080 presentation. This covers short moving starts, not complete laps.

The final gameplay comparison is stopped while installation waits on Play
Protect. Verification settings are unchanged. The three measured runs remain
separate from that incomplete gameplay attempt. Lab HDD/preferences/
EEPROM hashes and all nineteen diagnostic properties are restored and verified
at 00:17:19 Madrid. The installer delay exceeds the planned 00:16:25 session
deadline; a future harness must bound installation waits as well as gameplay.
Closing the ADB client does not cancel the committed Android install: the
final package check at 00:27:32 finds perf35 installed after the pending
session disappears. Restoring the previously verified perf36 APK succeeds.
At 00:28:52, version 65 and its exact APK hash are verified again, along
with all nineteen original properties and HDD/preferences/EEPROM hashes.
Perf36 is installed and its emulator is stopped. Future tests must track
committed installation completion independently of the host client process.
The other Android profile remains untouched. No raw captures, traces, game
data or caches are uploaded; local checks continue with Judge and CI/CD off.

### Perf36 resolution and stall comparison, 12 October

An installed-only session avoids package replacement during gameplay. Four
unprofiled starts run at 1x/3x/3x/1x with identical settings, private shader
and pipeline-cache seeds, and the same 24-second throttle/steering plan.
Native touch markers bound the measured emulator-PID flip windows. The
two 1x cases measure 27.644/28.021 FPS; the two 3x cases measure
26.767/26.727 FPS. Seven complete windows per scale pool to 27.806 FPS
at 1x and 26.750 FPS at 3x, a 3.80% decrease for nine times the internal
pixels. All fourteen unprofiled windows remain below 29.7 FPS. Resolution
cost alone does not explain the remaining shortfall in these starts.

Separate profiled starts contain 45/40 graphics-pipeline creations at
1x/3x, with maximum host durations of 36.754/38.562 ms. At 3x, native
presentation waits reach 27.951 ms and GPU-fence waits reach 25.603 ms.
These are nested, possibly cross-thread host durations; they cannot be
added as GPU execution time. The diagnostic runs have overhead and do not
establish an FPS gain. Pipeline creation can account for individual hitches
when new material appears, alongside CPU work and synchronization waits.

All six reviewed mid/end pairs show coherent road, buildings, car and HUD.
Three rivals are visible moving in each mid capture and advance on the
course/minimap. The player accelerates and the gesture releases. Reviewed
game clocks progress from 3.087--3.887 to 23.170--23.970 seconds; contact
and trajectories differ, with two end captures at the right barrier. This
is short moving-start coverage, not full-lap or full-game validation.
All end thermal statuses are zero and captured emulator processes show
no fatal exception, assertion or device loss. The 3x internal scanout is
1920x1440; native presentation is 2340x1080.

Lab HDD/preferences/EEPROM hashes and all nineteen diagnostic properties
are restored and verified at 00:41:36 Madrid, before the 00:44:44 deadline.
Perf36 stays installed, its emulator is stopped, and the other Android
profile is untouched. No APK installation runs inside this protected session.

### Perf37 compact graphics-pipeline lookup

Each graphics-pipeline lookup formerly cleared, copied and hashed a full
shader state as part of its key. The new key reuses the immutable shader
binding's existing LRU hash. Pipeline bindings still own a full shader-state
copy, and both the active-binding and LRU candidate comparisons check its
complete bytes after comparing the compact key. Hash collisions and reuse
of shader-cache memory therefore cannot alias different saved states.
Clear lookups have no shader pointer and retain their separate key behavior.

Render-pass load semantics, keyed registers and vertex descriptions remain
part of the equality decision. This is a generic renderer change without
game-specific instruction substitutions, skipped drawing or approximate
shader equality. The independent shader-state cache's normalized comparison
does not replace the pipeline's previous full-state comparison.

The host ASan/UBSan fixture and actual arm64 fixture each pass 132,000
differential cases against the former complete-key predicate, including
forced collisions, cache-memory reuse and clear/draw mismatches. On arm64
the complete shader state is 2640 bytes and the lookup key changes from
3136 to 504 bytes. This reduces repeated key construction/hashing work,
not shader compilation itself or retained shader-state storage. Byte
counts and equivalence checks alone do not establish a game FPS gain.

Perf37 builds locally as version 66 / `1.2.9-pgr2-perf37` in `.dev.local`.
The APK's v2 signature, existing certificate, package/version and actual
arm64 counters pass verification. A single version-increasing installation
completes and preserves lab HDD/preferences/EEPROM hashes and all nineteen
diagnostic properties before gameplay starts. Its SHA-256 is
`b2138e0265db9e121fdf8ad2a73f0cf26212f517b0b001aa58395438b8929c08`.

### Perf37 moving-race validation and outcome

Three unprofiled 3x starts use the same protected seed, native touch markers,
24-second driving plan and settings as the perf36 scale comparison.
Each installed APK is checked against the signed local artifact. The
three cases measure 26.212/27.318/26.543 FPS over 3/4/4 complete flip
windows. Pooled FPS is 26.736; eleven window averages range from
22.342 to 28.821 FPS, all below 29.7. The earlier perf36 3x pool is
26.750 FPS. These separate cohorts have differing contact/trajectories
and are not an interleaved ABBA test. No sustained FPS gain is accepted,
and stable original speed at Full HD or higher remains unverified.

A separate profiled start includes 644 complete frame intervals within
native touch bounds: median 34.587 ms, p95 53.299 ms and maximum
85.111 ms. Thirty graphics-pipeline creation calls reach 46.931 ms;
one shader compile takes 6.335 ms and texture decoding reaches 0.682 ms.
Native presentation and GPU-fence waits reach 23.871/31.444 ms. These
nested host timings show unresolved creation/synchronization hitches;
they do not measure additive GPU time or isolate an instruction-translation
speed limit. The compact lookup does not remove pipeline creation.

All four mid/end pairs show coherent buildings, road, player car and HUD.
Three moving rivals are visible in each mid capture and later advance
along the course/minimap. Throttle and steering respond and release.
Reviewed game clocks advance from 4.021--4.387 to 24.021--24.970 seconds.
End captures are beside the right barrier; this covers short starts,
not complete turns/laps or intermittent artifacts between captures.
All end thermal statuses are zero. Captured emulator processes show no
fatal exception, assertion or device loss.

The lab's HDD/preferences/EEPROM hashes and nineteen diagnostic properties
are restored and verified at 01:16:02 Madrid, before the 01:20:22 deadline.
Perf37 remains installed and its emulator is stopped. The other Android
profile is untouched. Fifty-four selected build inputs are checked against
the committed sources; the delivery APK matches the installed artifact.
Builds and tests remain local with Judge and CI/CD disabled. Raw game files,
captures, profiles and caches remain private.

### Perf37 compiler and scheduler diagnosis, 12 October

A new installed-only 3x moving race segment combines stall records, scheduler
tracing and CPU-cycle sampling. Native touch timestamps bound the selected
operations. The trace's MONOTONIC clock snapshots agree to 52 ns; scheduler
analysis uses their offset, rather than `/proc/uptime`. No positive
error/data-loss statistics are reported. The unstripped symbol table's ELF
build ID matches the actual installed APK's native library.

The three longest selected graphics-pipeline creations take
50.679/42.303/39.243 ms. The calling PFIFO thread runs on CPU for
50.298/42.244/39.229 ms during those calls. Approximately
81.0/79.4/79.6% of that thread's sampled CPU-cycle weight in the respective
intervals is in Qualcomm's `libllvm-qgl.so`. This identifies backend compiler
CPU work for these hitches, rather than a sleeping PFIFO thread awaiting
GPU completion. The vCPU runs for 46.778/40.426/38.513 ms in the same
intervals. Sampling without JIT maps cannot identify the guest instructions
responsible, so continued vCPU execution is not proof of useful simulation
progress or a specific guest spin loop.

For a separate 21.240 ms presentation call, the presentation worker sleeps
for 20.796 ms and PFIFO sleeps for the full interval. The longest selected
GPU-fence wait is 25.480 ms; its worker sleeps for 25.449 ms. Compilation
and graphics completion therefore require distinct measurements and fixes.
Whole-start CPU-cycle estimates assign 50.6% of sampled process cycles to
vCPU and 27.5% to PFIFO, not percentages of total phone utilization. The
sampler's kernel-symbol warning does not require changing kernel protection;
its completed samples are recovered and verified. This combined diagnostic
has overhead and does not establish an FPS improvement.

Visual review shows the player moving through a different course segment,
with three rivals progressing on the minimap. The mid/end game clocks read
1:05.489 and 1:26.323, rather than a restarted grid. Thus this is not a
repeat of the prior standing starts and is not used for an FPS comparison.
Road, buildings and HUD remain coherent in the two reviewed captures.

An independent capability query on the current Adreno 830 driver finds
`VK_EXT_vertex_input_dynamic_state` and its feature available, while graphics
pipeline libraries and shader objects are unavailable. A private generated
pixel experiment exercises sixteen vertex layouts, including changing
stride, offsets and float/normalized color formats. Static layouts use
sixteen pipelines; dynamic layouts use one. Four alternating static/dynamic/
dynamic/static rounds each verify 65,536 exact RGBA pixels. Creation totals
are 11.217/2.921/2.790/4.898 ms; record/submit/fence totals are
3.563/3.136/4.218/3.841 ms. Each static round reports fifteen application
pipeline-cache hits. These flat-shader synthetic results establish a usable
driver route, not redundant PGR2 variants or a gameplay FPS gain. Integration
requires measuring actual game variants and preserving the vertex layout
of each queued/reordered draw. See the
[Khronos dynamic vertex-input sample](https://docs.vulkan.org/samples/latest/samples/extensions/vertex_dynamic_state/README.html).

The protected session restores and verifies its own initial HDD/preferences/
EEPROM hashes and nineteen diagnostic properties at 01:25:17 Madrid, before
the 01:28:09 deadline. Preferences had changed before this session; older
session preferences are not substituted for the new baseline. Perf37 stays
installed and its emulator is stopped; the other Android profile is untouched.
There is no new APK or accepted sustained FPS gain in this diagnosis. Native
experiments and analysis run locally, with all captures/profiles/game data
private and Judge/CI/CD disabled.

### Perf38 dynamic vertex-input integration

The generic renderer can now use `VK_EXT_vertex_input_dynamic_state` when
explicitly requested and supported. Device creation checks the extension,
queried feature and loaded entry point, with static-input fallback. Ordinary
graphics-pipeline keys retain a distinct mode bit, all other keyed state and
the full shader-state collision check. In dynamic mode vertex descriptions
leave the pipeline key and are emitted for each draw. Reordered entries own
their descriptions, so later state mutations or sorting cannot change a
queued draw's input. Immediate, merged and empty-input paths also set state;
clear/display pipelines remain static. This targets layout-only pipeline
variants, as described in the [Khronos sample](https://docs.vulkan.org/samples/latest/samples/extensions/vertex_dynamic_state/README.html).

Render-pass state initialization now defines all bytes, including the three
load operations. Previously, a format-only caller's dirty comparison could
read uninitialized load-operation fields. Pipeline creation keeps its LOAD
semantics and render-pass begin still selects the appropriate load operations.
This is a cache/dirty-check correctness fix; no independent FPS gain is claimed.

Host ASan/UBSan and native arm64 checks pass 1,088 owned-input cases.
Pipeline-key checks pass 132,000 legacy-equivalence cases on both platforms,
plus separation of otherwise identical static/dynamic keys. On Adreno 830,
the native generated-pixel fixture passes 270,336 exact RGBA comparisons:
four sixteen-layout rounds and two empty-input draws. It exercises the same
production helper, overwritten live arrays, static-pipeline transitions,
strides, offsets and float/normalized formats. Synthetic warmed creation
totals are 1.517/0.128/0.148/1.498 ms; record/submit/fence totals are
3.645/3.533/3.747/3.775 ms. A preparatory static pipeline is excluded from
these timings; they do not establish cold-game or sustained-FPS improvements.

Perf38 builds locally as version 67 / `1.2.9-pgr2-perf38` in `.dev.local`.
APK v2 signature, existing certificate, package/version and actual arm64
feature strings pass verification. One version-increasing installation
completes before gameplay, preserving original lab data and properties.
APK SHA-256:
`d373ba8e3aea7e5cffcc8cec3a64d4427808897dc6daf75b45f469ccdc361467`.

### Perf38 moving comparisons and decision

Four unprofiled OFF/ON/ON/OFF starts use the same signed APK, settings,
warm shader seed and twenty-four-second native driving plan at 3x. Startup
checks show no cache invalidation and the same 185/193 module warm-up in
every case. The guest race is restarted through its retry confirmation;
grid captures show the countdown. Player and three rivals move. Only
complete flip windows between native touch timestamps enter the FPS pool.

| Dynamic input | Case | Complete windows | Descriptive FPS |
| --- | --- | ---: | ---: |
| OFF | A | 4 | 28.144 |
| ON | A | 4 | 26.060 |
| ON | B | 4 | 27.226 |
| OFF | B | 4 | 26.501 |

OFF pools to 27.321 FPS and ON to 26.643 FPS, a descriptive 2.48% decrease.
Window averages range from 24.157--29.312 OFF and 23.966--28.121 ON;
all sixteen windows are below 29.7 FPS. Countdown timing, trajectories and
barrier contact differ, so this does not establish a causal regression.
It establishes no accepted benefit. Dynamic input stays OFF by default.
Sustained original 30 FPS at Full HD or higher remains unverified. Internal
scanout at 3x is 1920x1440 and native presentation is 2340x1080.

A separate OFF diagnostic contains 644 complete moving frame intervals:
median 35.303 ms, p95 50.245 ms, maximum 113.535 ms. Thirty pipeline
creations total 290.364 ms with a 53.821 ms maximum. Creation metadata
groups into twenty-eight shader-state hashes, with at most two creations
per hash. These hashes describe observed metadata, not proven shader or
pipeline equality. This segment offers little evidence of many repeated
layout-only variants. One shader compile takes 4.652 ms; 201 texture decodes
total 33.317 ms with a 1.164 ms maximum. Native presentation and GPU-fence
waits reach 19.128/24.883 ms. Host phases overlap across threads and nest;
their totals are not an additive GPU budget or proof that compilation alone
explains the sustained speed shortfall.

The final ON diagnostic starts in a fresh protected files directory lacking
`gpu_driver_id.bin`. Production correctly invalidates the imported seed.
It records 636 moving intervals, p95 52.056 ms and maximum 187.591 ms;
35 pipeline creations reach 45.350 ms. There are 148 shader compiles before
touch input and 24 during movement, unlike the warm OFF diagnostic. It is
excluded from a matched-cache timing comparison. It does not invalidate the
four warm unprofiled FPS cases. The private harness now preserves the exact
current driver identity with the warm seed and rejects startup invalidation
or changed warm-up counts. This harness correction has syntax/identity
checks; no additional matched ON diagnostic is claimed in this session.

All six reviewed grid/mid/end sets show coherent road, buildings, car and
HUD. Three moving rivals are visible in each mid capture and progress on
the minimap. Throttle/steering respond and release. Mid game clocks span
3.871--4.454 seconds; ends span 22.954--25.420. End trajectories/contact
differ, including one stopped car facing a building. This covers short
starts, not complete turns/laps or artifacts between still captures. End
thermal statuses are zero; captured emulator processes have no fatal
exception, assertion or device loss.

Both protected records restore their own HDD/preferences/EEPROM hashes and
all twenty diagnostic properties. Final restoration is verified at 02:02:11
Madrid, before the original 02:02:59 deadline. EEPROM ownership/mode and
the other Android profile remain intact. Perf38 stays installed, dynamic
input defaults OFF, and the emulator is stopped. Sixty-one selected build
inputs are checked against committed sources and the delivered APK matches
the installed artifact. Builds/tests remain local; Judge and CI/CD stay
disabled. Game data, caches, captures and profiles remain private.

### Perf39 driver compilation policy and feedback

The opt-in compilation experiment applies Vulkan's
`VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT` to ordinary synchronous and
async graphics-pipeline creation. Default remains normal optimization.
Pipeline keys carry a separate policy bit and keep the complete shader-state
collision check. Clear/display/compute pipelines keep their previous policy.
Guest instructions, generated shader operations and geometry are unchanged.
The existing async draw-skipping setting remains disabled in gameplay tests.
Vulkan describes this flag as a possible creation-time reduction; execution
cost still needs measurement. See [Khronos pipeline flags](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCreateFlagBits.html).

Profiled runs also capture driver-reported creation feedback and hashes of
the actual vertex/geometry/fragment SPIR-V modules. Feedback requires Vulkan
1.3 or its optional extension; duration/cache-hit values are read only when
VALID is set. Unprofiled FPS runs attach no feedback structure and do not
hash modules. Module tuples help investigate repeated code, without relaxing
production state equality. See [Khronos feedback semantics](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCreationFeedback.html).

Perf39 builds locally as version 68 / `1.2.9-pgr2-perf39` in `.dev.local`.
V2 signature, existing certificate, package/version and actual arm64 feature
strings pass verification. Host ASan/UBSan and native arm64 pass 132,000
pipeline-key differential cases plus both policy/mode separation checks.
The production flag helper is also used by the native generated-pixel
fixture. Normal and unoptimized creation each pass 270,336 exact RGBA
comparisons across static/dynamic layouts and empty-input draws. These flat
shaders use warmed driver state; their timings establish no cold-game gain.
The APK is installed once with an increased version and data/property hashes
verified before gameplay. SHA-256:
`045f7cb8034b5132b611c18d102a4c570fa58ef6d166d5009eb663781072944c`.

### Perf39 moving results and decision

Four unprofiled normal/unoptimized/unoptimized/normal starts use the same
signed APK, 3x settings, warm seed and twenty-four-second native input plan.
Each case includes four complete flip windows inside native touch markers.
All six starts, including the diagnostics, preserve the exact current driver
identity and report 185/193 prewarmed modules with no invalidation. No
diagnostic has shader compilation before touch input.

| Creation policy | Case | Descriptive FPS |
| --- | --- | ---: |
| Normal | A | 26.979 |
| Unoptimized | A | 26.625 |
| Unoptimized | B | 26.460 |
| Normal | B | 27.599 |

Normal pools to 27.289 FPS; unoptimized pools to 26.543 FPS, a descriptive
2.74% decrease. Eight normal window averages range from 25.324--28.155;
eight unoptimized averages range from 22.396--28.800. All are below 29.7 FPS.
Trajectories/contact differ and this short cohort does not isolate a causal
regression. It establishes no accepted FPS benefit. The new policy remains
OFF by default, as does dynamic vertex input. Original 30 FPS sustained at
Full HD or higher remains unverified; 3x scanout is 1920x1440 and native
presentation is 2340x1080.

The matched-cache profiled starts contain 649/615 complete moving frame
intervals for normal/unoptimized creation. Median intervals are
35.144/37.027 ms, p95 51.291/57.894 ms, and maxima 91.901/115.790 ms.
There are 35/60 intervals above 50 ms. No improvement in frame stability is
accepted from these descriptive diagnostics.

Driver feedback is valid for all 41/33 ordinary draw-pipeline creations
ending inside native input bounds. Application-cache hits number 28/22 and
take 0.469--1.944/0.616--1.603 ms. The 13/11 misses take
17.145--46.587/22.103--43.556 ms. Thus cache hits are inexpensive in these
captures and fresh backend compilation remains the expensive case; requesting
unoptimized compilation does not remove it. Complete-frame host phase
aggregates record 41/33 pipeline creations totaling 342.699/341.638 ms and
maxima 46.601/43.575 ms. Frame-phase and metadata selections use different
boundary rules and include different clear/draw calls; their totals are not
interchangeable. Nested/cross-thread host timings are not an additive GPU
budget or proof that compilation alone explains the sustained FPS deficit.

During movement, module-tuple hashes group those creations into 36/29 groups,
with at most two creations per group. Full captured startups/movement contain
313/304 creations grouped into 120/114 module tuples. Equal hashes alone do
not prove shader or pipeline equality: fixed state and layout can differ.
Normal/unoptimized shader-compilation counts during complete moving frames
are 1/2, with maxima 5.088/12.142 ms. Texture decoding maxima are
0.881/0.971 ms; presentation waits reach 23.906/24.477 ms and GPU-fence
waits 30.522/23.168 ms. These measurements support investigating complete
pipeline preparation/persistence separately from SPIR-V warm-up and ongoing
CPU/synchronization cost.

All six reviewed grid/mid/end sets show coherent buildings, road, player car
and HUD. Three moving rivals are visible in each mid capture and progress on
the minimap. Throttle/steering respond and release. Mid game clocks span
4.087--4.404 seconds and ends 22.737--24.704. Barrier contact/trajectories
differ, including the first-turn approach in the final diagnostic. This is
short-start coverage, not a complete lap, calibrated simulation-speed proof
or assurance about artifacts between still captures. All end thermal
statuses are zero and captured emulator processes have no fatal exception,
assertion or device loss.

The protected record restores its own HDD/preferences/EEPROM hashes and all
twenty-one diagnostic properties at 02:47:46 Madrid, before the 02:49:28
deadline. Final verification confirms the installed APK, stopped emulator,
EEPROM ownership/mode and untouched original Android profile. Perf39 remains
installed with both new experiments default OFF. Sixty-three selected build
inputs match committed sources and the delivered APK matches the installed
artifact. Judge/CI/CD remain disabled; all builds/tests are local and raw
game data/caches/captures/profiles remain private.

### Perf40 complete pipeline recording and startup replay

The generic opt-in route records pointer-free descriptions of successful
synchronous draw pipelines and complete SHA-256-addressed SPIR-V copies.
On a compatible later startup it reconstructs those pipelines into the
existing opaque Vulkan cache, then destroys the replay objects. Ordinary
guest pipeline lookup and the complete shader-state collision check remain
unchanged. Replay never assigns a pipeline to guest state. Current descriptor
layouts and compatible format-based render passes are used. Vulkan supports
cache reuse between pipeline creations and runs; see [Khronos pipeline cache](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCache.html).

Android `debug.xemu.vk.pipeline_prewarm` / desktop
`XEMU_VK_PIPELINE_PREWARM` selects 0 disabled/default, 1 replay plus record,
or 2 record only. Shader caching must be enabled and async compilation
disabled. The header checks recipe ABI/size, actual driver identity,
enabled physical features and renderer capability/policy bits. Complete
record and shader digests are checked, reads are bounded and interrupted
appends retain only complete checked records. Missing/incompatible data
falls back to ordinary creation. No title-specific behavior, guest
instruction removal or shader simplification is introduced.

The host ASan/UBSan fixture and native arm64 fixture each pass 1,088
capture/rebuild round trips, 2,160 corrupt/truncated byte cases, unaligned
records, overflow indices and SPIR-V instruction/stage bounds checks.
The generated-pixel fixture now creates pipelines from production-owned
recipe reconstruction after overwriting caller arrays and state. Both
normal and unoptimized compilation pass 270,336 exact RGBA comparisons.
These tests cover generated inputs and cache handling, not every guest
format or a complete game's correctness.

Perf40 builds locally as version 69 / `1.2.9-pgr2-perf40`, in the separate
`.dev.local` package with the existing signing certificate. APK signature,
package/version and actual arm64 feature strings pass. The installed,
delivered and tested APK SHA-256 is
`3b4ababb6921e6812e20799c1497b2cfcb7e64b2dfee193f39374f869dd80026`.

### Perf40 matched-cache moving results

A record-only moving pass produces 144 complete recipes: 137,788 archive
bytes and 154 shader files totaling 2,517,520 bytes. All record digests and
shader filename/content digests match. This training pass is excluded from
FPS comparisons. Every subsequent case restores the same initial opaque
Vulkan cache, warm shader seed, current driver identity and recipe/shader
seed. All seven starts report 185/193 prewarmed shader modules and no cache
invalidation. The three replay starts load and warm all 144 recipes with
zero failures, taking 415.823, 418.321 and 412.876 ms respectively.

Four unprofiled off/on/on/off starts use identical 3x settings and the
twenty-four-second native input plan. Each has four complete flip windows
within native touch markers:

| Recipe replay | Case | Descriptive FPS |
| --- | --- | ---: |
| Off | A | 27.781 |
| On | A | 26.685 |
| On | B | 27.520 |
| Off | B | 25.900 |

Off pools to 26.839 FPS and on to 27.103 FPS, a descriptive 0.98% increase.
Between-case/countdown/trajectory/contact variation exceeds that difference;
no causal FPS improvement is accepted. Off windows range 24.886--28.332,
on windows 24.075--28.326, with all sixteen below 29.7 FPS. Replay stays
OFF by default. Original sustained 30 FPS remains unverified. Scanout is
1920x1440 at 3x, with native presentation 2340x1080.

The separate matched-cache profiled starts demonstrate a substantial
reduction in backend compilation work in the observed scene. Ordinary
draw-pipeline creation feedback is valid for all 30 off / 28 on calls ending
inside input bounds. Cache hits rise from 18/30 to 27/28. Twelve off misses
take 21.577--37.830 ms; the single on miss takes 29.443 ms. Cache hits take
0.619--1.743 / 0.703--1.707 ms. Total driver-reported creation time falls
from 350.856 to 57.799 ms, an 83.5% descriptive reduction. The one remaining
miss shows that a previously unseen pipeline still requires compilation.
Replay cannot eliminate the cost of genuinely new rendering state.

Each diagnostic has 619 complete moving frame intervals. Off/on medians
are 36.643/36.340 ms, p95 58.130/56.045 ms, p99 73.802/84.562 ms and
maxima 93.031/110.240 ms. Intervals above 50 ms number 61/50. The worst
tail is worse with replay; no overall frame-stability improvement is
accepted. Lower compilation cost alone does not remove the sustained
CPU/rendering/synchronization deficit.

Complete-frame host phase aggregates report 30/28 pipeline creations,
341.668/45.144 ms total and 37.843/29.463 ms maxima. These selections
include clear/draw calls and use different boundaries from driver feedback;
their totals must not be treated as interchangeable. Shader compilation
occurs once in each moving diagnostic, reaching 5.548/5.812 ms. Texture
decoding maxima are 0.911/1.221 ms. Presentation waits reach 23.987/38.079 ms
and GPU fences 24.631/41.436 ms. Nested/cross-thread host wall durations are
not an additive GPU budget, but show that important stalls remain after
preparing the known pipelines.

All seven reviewed grid/mid/end sets show coherent road, buildings, player
car and HUD, with three moving rivals in each mid capture and minimap
progress. Throttle/steering respond and release. Countdown differs (2, 3,
or no visible number); mid HUD clocks span 3.904--4.370 seconds, end clocks
22.904--24.938. Barrier contact and trajectories differ. Coverage is short
starts/first-turn approach, not complete laps, calibrated simulation speed
or artifacts between still captures. All end thermal statuses are zero;
captured emulator processes contain no fatal exception, assertion or
device loss.

The protected record restores its own HDD/preferences/EEPROM hashes and
all twenty-two diagnostic properties at 03:46:32 Madrid, before the
03:49:25 deadline. Final verification confirms the installed APK, stopped
emulator, EEPROM ownership/mode and untouched original Android profile.
Sixty-nine selected build inputs match committed sources and the delivered
APK matches the installed/tested artifact. Judge and CI/CD remain disabled;
all builds/tests are local and raw game data/caches/captures/profiles stay
private. This route prepares known pipeline work; sustained throughput
and long-tail synchronization remain open performance problems.

### Perf40 CPU, translated-code and scheduler diagnosis

A separate moving diagnostic with 144 recipes prewarmed correlates simpleperf
CPU-cycle samples, timestamped JIT load records and Perfetto scheduling with
the same native input markers. The sampled native library matches the tested
APK build ID. All 56,609 JIT loads have distinct start addresses in this
capture; attribution checks load timestamps rather than assuming an address
always represents the same translated block. Raw code, guest addresses,
profiles and game disassembly remain private.

During 24.268 seconds of native input, the vCPU executes for 21.943 seconds
(90.4% of one core), PFIFO for 14.446 seconds and the render worker for
0.669 seconds. These threads overlap; their times are not an additive budget.
The vCPU accounts for 53.91% of sampled process cycles and PFIFO for 26.44%.
Within vCPU samples, 49.72% map to translated guest code, 47.18% to native
code and 3.09% remain unmapped. Two short guest polling loops account for
approximately 25.38% of vCPU sampled cycles. This shows substantial polling,
not useful guest progress or proof that removing the loops would improve
the critical path. The producer of the second loop has not been established;
no title-specific loop removal or guessed wait replacement is introduced.

Native `cpu_exec_loop`, `cpu_tb_exec` and `lookup_tb_ptr_common` account for
12.12%, 4.29% and 2.86% of vCPU sampled cycles. PFIFO copy/move samples are
15.10% of that thread's cycles. Call-chain attribution places 4.36 percentage
points in uniform packing and 1.87 in the bulk vertex-uniform getter, while
5.17 are inside driver descriptor binding. These are profiled cycle shares,
not wall-time savings or unprofiled FPS gains. They motivate a generic exact
uniform-copy experiment without changing translated guest semantics.

The clocks correlate with two MONOTONIC snapshots differing by 52 ns in their
offsets, with no positive trace error/data-loss counters. A 19.864 ms fresh
pipeline creation has PFIFO running for 19.812 ms and vCPU running for
19.501 ms. A 19.921 ms presentation interval has the render worker sleeping
for 19.647 ms, PFIFO sleeping throughout and vCPU running for 19.389 ms.
GPU-fence waits reach 33.032 ms. Thus substantial CPU execution can coexist
with graphics waits. The sampled vCPU utilization alone cannot establish
which work limits sustained speed.

### Perf40 moving dispatcher comparison

The existing guarded `can_do_io` elision experiment is compared with recipe
replay enabled and profiling disabled at 3x. Four complete twenty-four-second
moving starts give off/on/on/off descriptive FPS of 26.894, 26.695, 27.269
and 27.797. The first attempted final-off start loses foreground and is
discarded; the fourth valid start is collected later in a fresh protected
session. The opaque cache, warm shaders, driver identity and 144-recipe seed
match, but the separated time window and trajectories limit inference.
Off pools to 27.345 FPS and on to 26.982 FPS. No improvement is accepted;
the experiment remains off by default. All sixteen window means are below
29.7 FPS. Three moving rivals, coherent road/buildings/HUD and responding,
released controls are visible in the reviewed captures. All end thermal
statuses are zero. Each session restores its own current data and properties;
later ordinary user changes are preserved by a fresh backup rather than
reapplying an old record.

### Perf41 exact uniform packing experiment

The opt-in `debug.xemu.vk.uniform_exact` / `XEMU_VK_UNIFORM_EXACT` route
compares reflected value bytes against the current shader-module allocation,
copies only changed values, and reads bulk vertex/light arrays directly from
their existing sources. Computed scalars still use the existing getter.
Uniform equality is exact, including NaN payloads and signed zero; inter-element
padding remains untouched. Existing force-upload flags on binding/frame/reset
remain set. Shader interfaces, pipeline recipes, cache identity, full shader
state comparisons and emulated guest instructions stay unchanged. The default
remains off while performance and game coverage are evaluated.

The ASan/UBSan host fixture and native arm64 fixture each pass 36,864
differential updates against the old packing implementation: 10,809 changed
and 26,055 unchanged cases, including contiguous/strided, partial/empty,
repeated and cold-allocation cases. The existing native uniform-copy fixture
also passes. These are generated buffer comparisons, not an exhaustive
queued-draw or game compatibility test.

Perf41 builds locally as version 70 / `1.2.9-pgr2-perf41` in the separate
`.dev.local` package with the existing signing certificate. Signature,
package/version, actual arm64 feature strings and installed APK identity
pass. The delivered/installed APK SHA-256 is
`1a74ea0b62d8704931253dfeb5e1ee3566b67565773e7911e858f061873a084d`.

Two initial moving attempts stop at the foreground-window guard before a
complete measurement. Neither is counted as an FPS or graphics result.
Their independent fresh records restore their own HDD/preferences/EEPROM
hashes and all twenty-three properties, with the emulator stopped and the
original Android user-10 installation untouched. The private harness now
records the rejected window and startup log before restoration, to distinguish
menu-transition problems from a real foreground loss. At that point no perf41
FPS benefit or sustained original-speed result had been established. Judge/CI/CD
remain disabled and raw game/profile/cache data stay private.

### Perf41 resumed moving comparison and CPU evidence

The next protected attempt identifies `NotificationShade` above the correctly
started emulator. It restores without contributing a measurement. After closing
the panel, three complete unprofiled cases run; the first attempted final-off
case loses foreground at its grid capture and is discarded. A fresh protected
session completes that final reference within the original eight-minute limit.
The private harness closes a notification panel only when the focused app is
the test emulator and keyguard is unlocked, then verifies emulator focus again.
It still rejects other apps and lock screens. None of the six accepted cases
records a panel-close event during its measurement.

All four unprofiled cases use identical 3x settings, the same cold opaque
Vulkan cache, actual driver identity, 185/193 warm shader modules and the
144-recipe seed. All recipes warm with zero failures and no shader-cache
invalidation. CPU/JIT and graphics profiling are disabled for the FPS cases.
Each has four complete five-second flip windows inside the native
twenty-four-second throttle/steering plan:

| Exact uniform updates | Case | Descriptive FPS |
| --- | --- | ---: |
| Off | A | 26.208 |
| On | A | 28.512 |
| On | B | 27.139 |
| Off | B, resumed | 25.233 |

Off pools to 25.721 FPS and on to 27.825 FPS, an observed 8.18% difference.
This is a favorable short-start result supported by the CPU evidence below;
countdown, trajectory/contact variation and the resumed final reference limit
causal inference and generalization. Off window means range 20.367--28.611 FPS
and on means 25.135--29.130 FPS. All sixteen are below 29.7 FPS. Sustained
original 30 FPS remains unverified. Scanout is 1920x1440 at 3x and native
presentation is 2340x1080. The route remains opt-in pending longer gameplay
and broader guest coverage.

A separate protected off/on pair enables CPU-cycle sampling, private timestamped
JIT mapping and graphics/scheduler diagnostics. It is excluded from the FPS
comparison because of the instrumentation overhead. Actual APK/unstripped
native build IDs match (`482ba2803feda677ee1cf425a5cac7387fc3915f`). Live map
and dump reads can differ while translation continues, so final attribution
uses the preserved stopped directory after restoration, with before/after
source hashes matching the downloaded bytes. The parser checks load timestamps
and rejects overlapping distinct JIT bases. Off/on have 59,443/59,291 loads,
zero reused start addresses and 290,824/280,943 selected samples. Sampling
records zero lost samples off and 31 lost kernel samples on, with zero lost
userspace samples. Kernel-symbol access remains restricted; no security or
kernel setting is changed.

PFIFO copy/move samples fall from 15.39% to 10.29% of that thread's cycles.
Copies attributed to `apply_uniform_updates` fall from 4.27% to 0.40%; bulk
copies in `pgraph_glsl_set_vsh_uniform_values` go from 1.70% to no attributed
samples. These are sampled shares, not an additive wall-time saving. During
24.315/24.267 seconds of input, PFIFO runs for 14.723/14.115 seconds and vCPU
for 20.760/21.284 seconds (85.38%/87.71% of one core). MONOTONIC offsets have
105/51 ns spreads and neither scheduler trace has positive error/data-loss
counters. Threads overlap and a busy vCPU can still be executing guest polls;
these numbers do not establish useful guest work or a complete critical path.

The instrumented starts contain 597/654 complete moving frame intervals.
Off/on median intervals are 37.267/34.301 ms, p95 67.680/48.669 ms,
p99 86.017/73.130 ms and maxima 140.217/100.331 ms. Intervals over 50 ms
number 76/30. The observed distribution is favorable, but one diagnostic per
mode, differing movement and instrumented timing cannot establish sustained
unprofiled stability. Long pauses remain.

Ordinary draw-pipeline feedback is valid for 28/29 creations inside input
bounds, with 27/28 cache hits and one miss each. Driver-reported creation totals
are 60.836/52.158 ms and miss maxima 31.023/25.276 ms. Complete-frame host
aggregates use different boundaries and total 47.516/40.102 ms; these totals
are not interchangeable. Shader compilation occurs once in each diagnostic,
reaching 10.148/5.085 ms. Texture-decoding maxima are 2.242/1.605 ms,
presentation waits 41.869/26.111 ms and GPU-fence waits 43.505/36.710 ms.
Nested and cross-thread wall durations are not an additive GPU budget.

The six reviewed grid/mid/end sets show no obvious road, building or HUD
texture corruption. The player and three rivals progress along the road and
minimap; throttle/steering respond and the native helper records release.
Mid game clocks span 4.171--4.571 seconds and ends 21.904--25.538.
Intro-camera/translucency and countdown differ, as do barrier contact and
trajectories. Coverage is short starts/first-turn approach, not full laps,
calibrated simulation speed, exact game-pixel equivalence or artifacts between
stills. All six end thermal statuses are zero and captured emulator processes
contain no fatal exception, assertion or device loss.

The resumed unprofiled session restores its own current data and twenty-three
properties at 17:28:50 Madrid, before its 17:30:29 deadline. The independent
CPU session restores at 17:33:22, before 17:34:41. Verification checks each
record's HDD/preferences/EEPROM hashes, APK identity, EEPROM ownership/mode,
stopped emulator and untouched original Android user-10 installation. Seventy-three
selected build inputs match committed sources and the delivered APK matches
the installed/tested artifact. All builds and tests are local; Judge/CI/CD
remain disabled and raw game/cache/profile/capture data stay private.

Driver descriptor-binding copy stacks remain about 5.76%/6.06% of PFIFO sampled
cycles. This motivates auditing redundant descriptor binds next. Dynamic
uniform offsets, changed texture sets, pipeline layouts, command-buffer
recording resets and queued/reordered draws must all remain correct; their
necessary binds cannot simply be removed based on the sampled cost.

### Perf42 exact descriptor command reuse

Perf42 adds opt-in `debug.xemu.vk.desc_memo` / `XEMU_VK_DESC_MEMO` reuse of
the last recorded single-set graphics descriptor command. Command buffer,
pipeline-layout handle, descriptor-set handle, first-set index and copied
dynamic-offset values must all match. New recordings, both graphics pipeline
bind sites and push-descriptor writes invalidate the memo. Queued guest state
does not restore this recorded-command state. Push-descriptor and bindless
routes retain ordinary calls. This generic experiment leaves descriptor
contents, layouts, shader interfaces, guest timing and fences unchanged; its
default remains off.

The sanitized host and native arm64 CPU fixtures each pass 57,344 checks,
including changed offsets/sets/layouts, caller-array reuse, foreign writes and
command-buffer handle reuse. Host llvmpipe and Adreno 830 each pass 131,072
generated RGBA pixel comparisons in normal/memo/memo/normal order. Each memo
round emits 18 and skips 110 of 128 requests; ordinary rounds emit all 128.
The GPU fixture includes two dynamic UBOs, different descriptor sets,
incompatible pipeline layouts, an intervening foreign bind and reset/reuse of
the command buffer. These generated checks do not establish whole-game
compatibility or the game's command-skip ratio.

The separate local package builds as version 71 / `1.2.9-pgr2-perf42`.
Signature, package/version, arm64 feature strings and installed identity pass.
The delivered and installed APK SHA-256 is
`6777b1204b4b18e1d4a35fbd8e01a6166be088b91ef759fb2914b0a8504503a9`.

Four unprofiled moving starts compare memo off/on/on/off at 3x, with exact
uniform updates enabled in all cases. They share a cold opaque Vulkan cache,
actual driver identity, 185/193 warm shader modules and the 144-recipe seed.
All recipes warm without failure or cache invalidation. Each case supplies
four complete five-second flip windows within the same native twenty-four-second
throttle/steering plan. Descriptive FPS is 25.845, 26.398, 25.953 and 25.704.
Off/on pool to 25.774/26.176 FPS, an observed 1.56% difference; no causal FPS
gain is accepted. All sixteen window means are below 29.7 FPS, with off ranges
21.982--28.351 and on ranges 21.947--28.377. Sustained original 30 FPS remains
unverified. Internal scanout is 1920x1440 and native presentation 2340x1080.

A separate graphics-profiled pair, excluded from FPS inference, records 635
complete moving frames per mode. Off emits 1,434,431 descriptor commands; on
emits 1,267,900 and skips 169,227, or 11.78% of its own requests. Host durations
for emitted calls are 1,259.030/1,209.860 ms and skipped calls total 12.682 ms
on. These nested, instrumented durations are not an additive frame saving.
Median intervals are 35.183/35.876 ms, p95 52.681/53.796 ms,
p99 84.628/73.283 ms and maxima 97.373/83.050 ms. Intervals over 50 ms number
42/43. The final on diagnostic ends at Android thermal status 1; the other
five cases end at 0. This mismatch further limits stability comparisons.

Each diagnostic creates 31 ordinary draw pipelines, with 30 valid driver-cache
hits and one miss. Driver feedback totals 55.965/57.577 ms, with miss maxima
25.821/25.314 ms. Complete-frame host creation durations use different
boundaries and total 42.561/43.339 ms. Shader compilation occurs once, at
5.113/11.242 ms. Texture decode maxima are 1.760/3.380 ms, presentation
21.840/32.234 ms and GPU-fence waits 43.392/37.060 ms. Thus texture appearance
does not by itself identify texture decoding as the principal stall.

All six reviewed grid/mid/end sets show coherent road, buildings and HUD with
the player and three rivals moving. Throttle/steering respond and the native
helper records release. Countdown, contact and trajectories vary; coverage is
short starts and the first-turn approach, not full laps or calibrated
simulation speed. Captured emulator processes contain no fatal exception,
assertion or device loss. No accepted case records a notification-panel-close
event during measurement.

The protected session restores its own fresh HDD/preferences/EEPROM and all
twenty-four diagnostic properties at 18:19:29 Madrid, before its 18:21:58
deadline. Verification checks record hashes, EEPROM ownership/mode, APK
identity, the stopped emulator and the untouched original Android user-10
installation. Raw game/cache/profile/capture data remain private. Builds and
tests remain local with Judge/CI/CD disabled.

### Perf43 small uniform value comparison experiment

Further analysis of the preserved perf41 CPU call-chain samples attributes
6.156% of PFIFO sampled cycles in the exact-packing diagnostic to comparisons
called from `apply_uniform_updates`. This is a sampled share, not a measured
wall-time saving. The exact route reduced copying but introduced this comparison
cost, motivating a separate small-value experiment rather than treating its
earlier favorable short FPS result as a complete solution.

Scheduler correlation of the six longest complete moving frames in each
preserved perf41 trace further separates execution from waiting. In an
exact-packing 100.331 ms frame, PFIFO executes for 47.391 ms and sleeps for
51.200 ms; vCPU executes for 89.111 ms. In the ordinary-packing 93.546 ms
frame, PFIFO executes for 41.347 ms and sleeps for 48.606 ms, with a
43.505 ms graphics-fence event. These intervals overlap across threads and
must not be added. They show that substantial command-preparation CPU work
coexists with graphics waits, without proving useful vCPU work or GPU execution
time. Clock-offset spreads are the previously validated 105/51 ns, and neither
trace reports positive error/data-loss counters.

Perf43 adds opt-in `debug.xemu.vk.uniform_small` / `XEMU_VK_UNIFORM_SMALL`,
used only with exact uniform packing. It compares 4/8/12/16-byte values through
fixed-size integer temporaries and keeps libc comparison for other lengths.
The loads use fixed-size `memcpy`, preserving alignment and aliasing rules;
12-byte values read exactly 8+4 bytes. All raw bits remain significant.
Padding, dirty-state handling, forced uploads, descriptors, shader interfaces,
cache identity and emulated instructions are unchanged. The experiment defaults
off and has no title/device-specific condition.

ASan/UBSan host and native arm64 fixtures each pass 73,728 differential packing
updates across both comparison modes, with 21,622 changed and 52,106 unchanged
cases. A separate libc-oracle fixture passes 4,362,702 comparisons on each,
including every single-bit change for small spans, independent unaligned
pointers, read-only sources and allocation tails followed by inaccessible
pages. These checks establish generated byte equivalence, not game FPS or
whole-game graphics compatibility.

The local separate-package APK builds as version 72 / `1.2.9-pgr2-perf43`.
Signature, package/version and actual arm64 feature strings pass. The delivered
APK SHA-256 is
`f914abaddd270403bce76b6423a2e131e8d9b3627182cc4efd5c40d0d089c853`.
The standalone native fixtures leave current HDD/preferences/EEPROM hashes
unchanged and the emulator stopped; they do not install the APK.

A proposed same-perf42-APK fast-fence comparison stops before measurement
because the scripted menu tap leaves the Android notification panel above the
emulator. Its fresh backup is restored and verified at 18:52:07 Madrid, before
18:59:45. The private harness now opens the app's supported Back-key menu path.
A subsequent attempt observes keyguard before creating a backup. Neither
attempt contributes FPS or graphics evidence. At this stage perf43 installation
and its moving off/on comparison were pending a usable foreground device; no
new FPS gain, sustained original speed or full-game graphics claim was made.

### Perf43 moving comparison and native CPU evidence

Perf43 is installed once with its APK hash, version, lab HDD/preferences/EEPROM
and twenty-five properties verified. The first protected start fails before
measurement: Android opens the Back-key menu on one input edge and dismisses
it on the other. The private current-process log confirms dialog creation and
immediate removal. That fresh record restores at 19:23:05 Madrid, before its
19:29:59 deadline. The private harness uses the app's upward-swipe menu path
for the subsequent accepted cases. This is an automation correction, not a
measured emulation optimization.

Three complete unprofiled starts run in the next protected session; the final
reference is deferred to reserve restoration time. A fresh session completes
that reference and a separate CPU-profiled pair. The four unprofiled cases
hold exact uniform packing, descriptor memoization, 3x scanout, three submission
slots, adaptive VBlank and all other settings fixed; fast fences remain off.
Each uses the same cold opaque Vulkan cache, actual driver identity,
185/193 warm shader modules and 144 complete pipeline recipes. All recipes
warm without failure or cache invalidation. Four complete five-second flip
windows fall within each native twenty-four-second throttle/steering plan:

| Small uniform comparisons | Case | Descriptive FPS |
| --- | --- | ---: |
| Off | A | 27.289 |
| On | A | 26.968 |
| On | B | 27.012 |
| Off | B, resumed | 26.427 |

Off/on pool to 26.858/26.990 FPS, an observed 0.49% difference below repeat
variation. The separated final reference and differing trajectories/contact
further limit inference. No FPS gain is accepted and the experiment remains
off by default. Off windows range 23.279--28.856 FPS and on windows
24.990--28.913; all sixteen are below 29.7 FPS. Internal scanout remains
1920x1440 with native 2340x1080 presentation. Original sustained 30 FPS is
not established.

The independent profiled pair enables graphics timing, scheduler tracing and
native CPU-cycle sampling; JIT instrumentation remains off and no guest-code
dump is collected. It is excluded from the FPS comparison. APK and unstripped
ELF build IDs match (`9c389d6dbe98f9f78e3967f7ba34f04f1bd23a07`). Attribution
selects the PFIFO thread from a pipeline event within the native driving bounds
and checks that sampling spans those entire bounds. Off/on select 55,887/53,139
PFIFO samples. Sampling records 327,604/343,661 process samples, with 64/0
lost kernel samples and zero lost userspace samples. Kernel-symbol access
remains restricted; no device security setting is changed.

Libc comparisons attributed to `apply_uniform_updates` fall from 6.219% to
3.430% of PFIFO sampled cycles. Its own inlined instructions rise from 4.350%
to 5.925%, as comparison work moves into the function; the libc decrease must
not be described as eliminating that entire cost. Total libc-comparison shares
are 9.887%/7.236%. During 24.213/24.242 seconds of native input, PFIFO executes
for 14.415/13.799 seconds and sleeps for 9.224/9.856 seconds. These are separate,
instrumented starts with differing motion, not causal wall-time savings.
MONOTONIC offset spreads are 1/52 ns and neither trace has positive scheduler
error/data-loss counters.

The profiled starts contain 636/569 complete frame intervals. Off/on medians
are 34.998/38.662 ms, p95 54.339/68.442 ms, p99 79.393/107.089 ms and maxima
97.837/130.487 ms. Intervals over 50 ms number 43/95. This unfavorable second
diagnostic rules out a stability claim from the CPU-share change; it does not
establish an unprofiled regression from one instrumented start per mode.
Each has 28 valid ordinary pipeline feedback records, with 27 driver-cache
hits and one miss. Feedback totals are 51.792/66.984 ms and miss maxima
25.663/33.733 ms. Texture-decode maxima are 2.510/2.364 ms, shader compilation
5.690/9.696 ms, presentation 31.441/24.139 ms and GPU fences 40.018/35.433 ms.
Nested and cross-thread durations are not an additive GPU budget.

All six reviewed grid/mid/end sets show coherent road, buildings and HUD with
the player and three rivals moving. Throttle/steering respond and release is
recorded. Mid captures show 112--115 km/h. Countdown, contact and trajectories
differ; coverage remains short starts/first-turn approaches rather than full
laps, calibrated simulation speed or exact game-pixel equivalence. All six
end thermal statuses are zero and their current-process logs contain no fatal
exception, assertion or Vulkan device loss. No accepted case records a
notification-panel-close event.

The first valid session restores its fresh current data at 19:28:28 Madrid,
before 19:29:59. The resumed reference/CPU session restores at 19:34:38,
before 19:36:09. Verification checks each record's hashes, twenty-five
properties, APK identity, EEPROM ownership/mode, stopped emulator and untouched
original Android user-10 installation. Seventy-nine selected build inputs
match committed source; delivered/installed APK identities match. All raw
game/cache/profile/capture data remain private and all builds/tests are local.

A subsequent same-APK fast-fence comparison uses small comparisons off and
the same cache/settings controls. Its first launch loses emulator foreground
before a complete measurement and restores its own fresh record. It contributes
no FPS or graphics result; fast-fence effects remain unmeasured in this session.

### Perf43 resumed fast-fence comparison

The next protected session tests the existing `fast_fences` preference in the
same perf43 APK. Small uniform comparisons stay off; exact packing, descriptor
memoization, 3x scanout, three submission slots, adaptive VBlank, the cold opaque
cache, actual driver identity, warm shader seed and 144 pipeline recipes are
fixed. Native startup logs verify the actual fence mode. This option defers
vertex/buffer-space submissions through the existing fenced frame slots; no
guest-specific wait shortcut, shader omission or changed barrier is added.

The first off/on starts finish; an attempted second-on start ends before its
native driving marker. Its grid image already shows a blurred Android
transition and is excluded from game-graphics evidence; no complete FPS
measurement is accepted. The native helper checks emulator focus before
starting and throughout driving. The incomplete job's exact exception was not
preserved, so it is not used to attribute an emulator failure. No helper process
remains after restoration. The session restores at 19:49:17 Madrid, before
19:54:57. After
the user makes the phone available again, a fresh four-minute session completes
the second-on and final-off references:

| Fast fences | Case | Complete five-second windows | Descriptive FPS |
| --- | --- | ---: | ---: |
| Off | A | 4 | 25.872 |
| On | A | 3 | 24.773 |
| On | B, resumed | 4 | 27.400 |
| Off | B, resumed | 3 | 25.888 |

Off/on pool to 25.879/26.272 FPS, an observed 1.52% difference. On repeats
span 24.773--27.400 FPS, much more than the pooled difference; the separated
session and different trajectories/contact further limit inference. No FPS
or stability improvement is accepted, and fast fences remain off. All fourteen
window means are below 29.7 FPS. Internal scanout remains 1920x1440 with native
2340x1080 output. Graphics/scheduler diagnostics are not completed in this
comparison, so it does not establish which wait moved or shortened.

The four accepted grid/mid/end sets show coherent road, buildings and HUD,
the player and three moving rivals, and responding/released controls. All have
visible countdown 2 at the grid; mid captures show 114--119 km/h. End positions
and barrier contact vary substantially. Coverage remains short moving starts,
not clean full laps, calibrated simulation speed or full-game compatibility.
Each accepted current-process log has no fatal exception, assertion or Vulkan
device loss; end thermal status is zero in all four. No accepted case records
a notification-panel-close event.

The resumed session restores its own current HDD/preferences/EEPROM and all
twenty-five properties at 19:54:22 Madrid, before 19:56:26. Independent checks
verify record hashes, actual perf43 APK identity, EEPROM ownership/mode,
stopped emulator and untouched original Android user-10 installation. Raw
game/cache/profile/capture data remain private; Judge/CI/CD remain disabled.

### Perf44 bounded dependency-diagnostic preparation

The user requested detailed logging to attribute the remaining lag before
another optimization experiment. Perf44 adds an Android-only, default-off
dependency recorder; it makes no game-specific instruction or rendering
shortcut. See [LAG-DIAGNOSTICS.md](LAG-DIAGNOSTICS.md) for the schema,
interpretation limits and capture protocol.

Unique batch/worker/submission identities connect preparation, queue residence,
submission acknowledgement, fence/slot/completion waits and presentation.
Eighteen named CPU stages and the existing texture/shader/readback phases
retain detailed intervals at or above 50 us. All renderer fence-wait routes
preserve their original arguments and results. PFIFO condition snapshots,
VBlank observations, repetitive NV2A register reads/value transitions and
PGRAPH fence publication supplement scheduler and optional CPU/JIT captures.
This does not intercept all guest RAM polling or identify semantic game
objects such as a particular building.

Main and auxiliary command-buffer timestamp intervals retain their submission
identities. Queries are read after existing completion points, without an
extra per-frame wait. The selected queue's valid bits and timestamp period
are recorded, wrap uses unsigned modular arithmetic, and optional EXT
calibration retains DEVICE/CLOCK_MONOTONIC correspondence and maximum
deviation. Missing calibration gives relative durations only. GPU intervals
include dependencies; nested and cross-thread durations remain non-additive.

Captures are armed after race loading, limited to 30 seconds and 262,144
records (20 MiB), and exported from a detached thread after recording stops.
Pages are touched before the arming acknowledgement. Overflow is counted,
never silently overwritten. The analyzer rejects incomplete exports, lost or
miscounted records, invalid bounds and inconsistent timestamp metadata, and
withholds absolute GPU alignment when it contradicts host observations.
It outputs per-frame suspects, wait/submission links and a Perfetto-compatible
timeline rather than automatically declaring a root cause.

The host ASan/UBSan buffer/timestamp fixture and its arm64 version pass. The
timestamp fixture checks 118,784 wrap cases across all 36--64-bit widths plus
invalid-width and bounded-storage cases. Six generated Python tests cover
dependency links, calibrated/uncalibrated timestamps, contradictory clocks,
driving bounds and incomplete/lost records. On the phone, a fixture links the
actual recorder implementation: four concurrent producers export 16,001
records with 4,000 distinct command identities and a subsequent independent
capture exports two records. Both have zero dropped records and pass the
same analyzer; temporary trace properties are restored. This fixture opens
no game and does not change app data or controls.

The private moving harness is prepared for 3x references, warm 1x/3x captures,
cold-resource 3x and a repeated warm case, with optional CPU sampling/JIT maps.
Screenshots are outside timed driving, native input markers delimit analysis,
and scheduling/frequency collection is separate from unprofiled FPS results.
Fresh protected sessions preserve and restore all 27 properties and current
data/settings, retain exact helper failures and reserve restoration time.
Offline scheduler analysis runs after the phone is released. These race/GPU
captures have not yet run; this preparation establishes no FPS gain, sustained
30 FPS, full-game graphics compatibility or complete critical-path attribution.

The locally built and signed separate-package APK is version 73,
`1.2.9-pgr2-perf44`, SHA-256
`10d74450d9fd2f863a68ce0b8381ec8646de83ee31fd84d90cc4c9d797294ebe`.
Its packaged and unstripped native ELF build IDs agree:
`3cada00d6b37dd269dcccad011a0a89492d499f7`. Eighty-nine selected build inputs
are checked against the saved build snapshot and committed source. Judge and
CI/CD remain disabled; raw phone/game/driver/capture data stay private.

### Perf44 moving dependency captures and attribution

The installed perf44 APK identity matches the locally verified artifact above.
One unprofiled 3x moving start and four bounded diagnostics now ran on the phone:
warm 3x, warm 1x, warm 3x with CPU/JIT sampling, and cold shader/recipe resources
at 3x. Each restarts the race, drives with the same 24-second throttle/steering
plan and releases all inputs. Native input markers delimit analysis; images
are outside that interval. Warm cases establish 185/193 shader modules and
144/144 successful recipe warmups. Cold resources omit those imported seeds
and disable recipe warming. Trajectories and barrier contact differ, so these
are diagnostic interventions, not exact-scene FPS comparisons.

The first scheduler attempt was rejected because its configuration path was
not readable by Perfetto. A later warm 3x trace retained only the final part of
driving in its 32 MiB ring buffer; none of its slowest twenty frames had retained
scheduling. Its recorder/GPU data are complete, but it is excluded from whole-
driving scheduler attribution. Another initial query incorrectly excluded
workers whose Perfetto process mapping was absent. Private correlation now
uses independently captured process task ownership, checks clock agreement,
requires the whole driving interval inside trace bounds, and rejects positive
trace error/loss counters. Three later 128 MiB captures pass those checks; their
MONOTONIC snapshot offsets differ by only 52 ns within each trace.

All four recorder exports have valid sequence/count/footer and zero drops;
their calibrated GPU intervals pass the host submission/readback plausibility
checks. The 2,495 complete instrumented flip intervals are distributed below.
Percentiles refer to these recorded intervals, not unprofiled performance.

| Diagnostic | Complete flip intervals | Intervals over 50 ms | Median / p95 / maximum interval, ms |
| --- | ---: | ---: | --- |
| Warm 3x | 608 | 61 | 37.031 / 60.520 / 96.625 |
| Warm 1x | 664 | 24 | 34.098 / 43.734 / 94.609 |
| Warm 3x with CPU/JIT samples | 639 | 36 | 36.039 / 50.996 / 86.159 |
| Cold resources 3x | 584 | 82 | 38.150 / 69.577 / 113.344 |

There are now specific observed stall chains:

- Pipeline creation runs synchronously on PFIFO. Its maximum is 30.198 ms in
  warm 3x, 24.247 ms in warm 1x, 22.239 ms with CPU sampling, and 45.078 ms with
  cold resources. During the 45.078 ms interval, scheduling records PFIFO
  running for 44.799 ms. During the sampled 22.239 ms interval, PFIFO runs for
  21.874 ms and user callchains reach Adreno `vkCreateGraphicsPipelines`, driver
  compiler code and compression. Concurrent vCPU samples put 65.33% of its
  cycle weight in one ordinary-RAM polling loop; this overlap does not establish
  which operation publishes that RAM flag.
- Completion waits identify vertex-buffer updates and buffer capacity pressure.
  Warm 3x contains 90 vertex-dirty and ten capacity waits; the largest is
  24.798 ms. Submission itself takes 0.126 ms, then the first timestamp of that
  batch arrives 17.213 ms after submit return. Earlier batch GPU intervals
  overlap 17.196 ms of that gap; the current auxiliary/main interval union is
  7.021 ms. Another vertex-dirty wait lasts 20.913 ms although its own GPU
  interval union is 1.742 ms; earlier work overlaps 18.510 ms of its pre-start
  gap. These are queued dependency observations, not pure GPU busy time.
- The sampled 3x case includes a 36.306 ms vertex completion wait. PFIFO sleeps
  for 36.285 ms and the render worker for 36.113 ms while the vCPU runs for
  35.608 ms. At 1x there is still a 31.884 ms vertex completion wait. Lowering
  resolution changes the workload but does not eliminate synchronization.
- Register read-lock waits are a separate contributor. The sampled maximum
  is 64.762 ms, with the vCPU sleeping for 62.506 ms; only a nested 21.181 ms
  slot/fence wait has a complete producer link. The remaining lock interval
  is not attributed entirely to GPU execution. Lock ownership/wake paths and
  observer interference remain open questions.

Texture decode spans are smaller in these captures: maximum 1.014 ms in warm
3x, 0.932 ms at 1x, 1.062 ms with CPU sampling and 1.602 ms with cold resources.
This does not exclude cumulative texture preparation or other scenes. A warm
1x texture-image allocation also reaches 23.168 ms. Appearing objects can
introduce new pipelines and resource synchronization in addition to decoding
their textures; the symptom alone cannot distinguish those costs.

Full-interval scheduling shows the vCPU running for 92.09% at 1x, 89.53% in the
sampled 3x case, and 86.31% with cold resources. Its runnable scheduling delay
is 0.69--0.85%; this is one emulated CPU thread, not whole-phone utilization.
PFIFO runs for 61.5--61.8% and sleeps for roughly 36%. The separate CPU profile
contains 283,427 in-driving samples. Native/guest symbol attribution uses the
matching perf44 ELF and a fresh JIT map with no overlapping address ranges.
Two polling loops account for 26.51% of vCPU sampled cycle weight; selected
native dispatch/lookup functions account for another 22.37%, with prologue
execution separate. These cycle weights cannot be added to GPU wall time or
treated as achievable FPS gains. Simpleperf reports 22 lost kernel samples,
zero lost user samples, and restricted kernel symbols; no restriction is changed.

The unprofiled reference accepts four complete five-second windows inside
native driving markers: 27.900, 26.113, 24.147 and 27.589 FPS, pooled to
26.436 FPS. This is a short descriptive reference, not sustained original
speed or a measured perf44 optimization gain. Internal 3x scanout remains
1920x1440 and Android output 2340x1080. Repeated warm/unprofiled references
remain pending, and all diagnostic runs include observer cost.

Reviewed end images show coherent road/buildings/HUD, released controls and
advanced rival markers on the minimap. Player motion is established by the
grid/end positions and native input; barrier contact eventually stops several
cases. This covers short moving starts, not clean full laps or full-game
graphics correctness. Current-process logs have no fatal exception, assertion
or Vulkan device loss; end thermal status is zero for the four diagnostics.

The next generic optimization targets are pipeline misses on the PFIFO path
and buffer lifetime/dependency serialization. Shared destination-buffer writes
currently require explicit ordering against earlier readers; simply deleting
those waits/barriers or skipping guest polling would not establish correctness.
Detailed per-slot resource lifetime experiments need matching pixel checks and
unprofiled moving repeats. The long register mutex tail needs more attribution
before changing its synchronization.

The final protected session restores its own HDD/preferences/EEPROM and all
27 properties at 21:37:42 Madrid. Independent checks confirm the installed
perf44 APK, restored hashes, EEPROM ownership/mode, stopped emulator and
untouched original user-10 installation. All raw game/driver/JIT/scheduler
data remain private. Judge and CI/CD remain disabled; no clocks, governors,
kernel or security settings are changed.

## 2026-10-12: fenced draw uploads and PGRAPH snapshots (perf45--47)

Two generic experiments now target the captured dependency chains. Neither
recognizes a game or skips guest instructions, draws or register write side
effects. Both remain disabled by default.

`debug.xemu.vk.direct_upload` selects shared GPU destinations (0), direct reads
from each fenced slot's host upload buffers (1), or separate GPU destinations
for each slot (2). Index/indirect, inline vertex and uniform bindings follow
the slot; descriptor keys include its uniform buffer. Slots are recycled after
their fences. Direct uploads flush host writes and publish them to index/
indirect, vertex and both uniform shader stages. Mode 2 retains GPU copies.
The existing vertex-RAM allocation is already per-slot.

Main draws retain the previous submission chain. An auxiliary buffer may upload
before the previous main submission completes only if it was newly opened for
isolated slot uploads, with no existing auxiliary work or deferred downloads.
Shared-image work retains the original ordering. Buffer-capacity and vertex-
dirty finishes may defer in isolated modes, with resource/slot fences retained.

The generated-data Vulkan fixture passes 196,608 exact RGBA comparisons on
Adreno 830: 1/2/3 slots, staged/direct paths, indexed/indirect draws, dynamic
uniform offsets in both shader stages, fence reuse and auxiliary/main semaphore
routing through the production helper. Host llvmpipe passes with synchronization
validation enabled and zero errors. The phone has no validation layer enabled.
These are fixture checks, not whole-game graphics or FPS evidence.

Perf45 does not establish a direct-host speedup. Its unprofiled 3x reference
averages 26.193 FPS across four complete five-second windows. Two direct-host
cases average 25.123 and 24.835 FPS, pooled to 24.979 FPS. A later reference
averaging 22.175 FPS ends at thermal status 1 and is excluded from the matched
thermal pool. Trajectory/contact differences limit inference. The direct-host
route stays disabled; separate GPU destinations still need moving-race data.

`debug.xemu.pgraph.status_read=1` reads single published status words without
waiting on the renderer mutex. Live IRQ producers, acknowledgement and mask
accesses use matching atomic operations; FIFO/INCREMENT publication uses the
same acquire/release helper. Only side-effect-free 32-bit reads of INTR, INTR_EN,
FIFO and INCREMENT bypass the lock. RDI_DATA still advances its indirect index
under the original lock. INCREMENT writes still advance READ_3D and trigger the
existing flip/kick behavior. Quiescent save/load keeps its serialized format.

Offset 0x71c (1820) is INCREMENT; FIFO is 0x720. Perf46 initially covered IRQ/
FIFO reads but left INCREMENT locked. Perf47 adds that missing read. The atomic
fixture passes 65,536 IRQ-mask cases and 100,000 ordered publications preserving
another concurrent IRQ, on host and arm64. Host ASan/UBSan checks pass.

Two perf46 moving diagnostics have whole-input scheduler coverage, zero dropped
recorder events and valid calibrated GPU alignment:

| Observation | Original reads | IRQ/FIFO snapshots |
| --- | ---: | ---: |
| Recorder events | 121,429 | 123,544 |
| Complete recorded flip intervals | 622 | 628 |
| Recorded intervals over 50 ms | 48 | 39 |
| Logged INTR mutex waits >=50 us | 49 | 0 |
| Maximum INTR mutex wait | 31.507 ms | none |
| Logged INCREMENT mutex waits >=50 us | 32 | 36 |
| Maximum INCREMENT mutex wait | 27.729 ms | 24.672 ms |

Eliminating the observed INTR read mutex waits is a confirmed mechanism change.
The frame counts are descriptive diagnostics with observer cost, not an
unprofiled FPS gain. Both end at thermal status 0. The unprofiled perf46 reference
ends at thermal status 1 and is excluded from matched comparison. Pipeline
creation and vertex/capacity waits remain present.

Perf47 is locally compiled, signed and installed in the lab package as version
code 76 / `1.2.9-pgr2-perf47`. APK and download copy match SHA-256
`4fd5a89d263b7883fd2b1d1015f76148949a31eda8cd9217d10ebd761d098286`;
97 selected inputs from the actual build snapshot match the source. An initial
diagnostic loses foreground to ChatGPT and Android rejects the input helper;
it lacks complete driving markers and is excluded, not an emulator crash.

A later bounded perf47 session completes an unprofiled reference with both
experiments off: four five-second windows average 27.244 FPS (26.563--28.366).
Thermal status is 0 at start and case end, then changes before the next case
after passive cooldown. The harness stops and restores at 22:42:24 Madrid,
before its 22:45:52 deadline. This establishes no paired perf47 gain or sustained
30 FPS. Internal 3x remains 1920x1440; Android output is 2340x1080.

Reviewed end images show coherent road/buildings/HUD, released controls, player
advancement and advanced rival markers. The latest reference ends at 62 km/h
beside a barrier. This covers short starts, not full laps or full-game correctness.
Independent restoration checks confirm HDD/preferences/EEPROM, all 29 diagnostic
properties, stopped emulator and untouched original user-10 installation.
Raw captures and game data remain private. Judge and CI/CD remain disabled.

The separately authorized three-minute perf47 diagnostic completes and restores
at 22:49:46 Madrid, before its 22:51:20 deadline. It records 121,998 events with
zero drops, 628 complete flip intervals and 232 valid GPU clock calibrations.
Native input lasts 24.170 seconds; scheduler coverage contains the whole input
with verified task ownership and no positive trace error/data-loss counters.
With status snapshots enabled and shared draw destinations retained, both INTR
and INCREMENT have zero logged read mutex waits. The remaining PGRAPH read wait
is 66 us at another register. The recorder still observes INTR and INCREMENT
reads, so their disappearance from the lock list is not missing MMIO telemetry.
Perf46's original-read diagnostic had 81 target waits, totaling 482.131 ms
across its input interval. Those logged totals are not a frame budget or an
achievable FPS gain; the two captures differ in trajectory and thermal state.

This confirms the perf47 mechanism for both target reads, while the remaining
37 intervals over 50 ms show unresolved stalls. Median/p95/maximum recorded
intervals are 35.996/53.068/106.647 ms. Pipeline creation still reaches 23.139 ms;
110 vertex-dirty completion waits reach 37.665 ms and 11 capacity waits reach
18.438 ms. In the largest completion wait, submit takes 0.043 ms, the first GPU
timestamp follows submit return by 27.609 ms, and earlier GPU intervals overlap
27.590 ms of that gap. Its own GPU interval union is 9.710 ms. These continue
to support queued dependencies, not a claim of pure GPU occupancy. Maximum
recorded texture decode is 1.275 ms. The vCPU runs for 90.61% of the input
interval and PFIFO for 63.51%; these percentages concern individual threads.

The diagnostic ends at thermal status 1. Its four five-second flip windows
average 25.689 FPS, explicitly excluded from a paired, unprofiled performance
claim. The earlier 27.244 FPS reference has snapshots off and cannot be called
a perf47 optimization gain. The latest end image has coherent road/buildings/
HUD and advanced rival markers, but the player has contacted the barrier and
is moving at only 2 km/h. No sustained original speed or clean full lap is
verified. Current-process logs contain no emulator fatal exception, assertion
or Vulkan device loss. Independent verification again confirms restoration
of the lab data and all 29 properties, with the original profile untouched.

## 2026-10-13: Barcelona slow-race reproduction on the user's Slot 1

The user replaced Slot 1 with the slow Barcelona instant-action race and
explicitly prohibited overwriting it. The earlier Slot 3 cone-challenge
capture is excluded from this case: the user identified it as a different
race/city. The new Slot 1 preview and loaded image both show the Barcelona
introduction. The current user-selected disc and HDD are used directly, with
no stationary benchmark seed, HDD replacement or snapshot-save operation.

Perf47 is unchanged. Vulkan runs at the user's current 1x setting, with the
previously enabled status-snapshot, uniform/descriptor/vertex-page, pipeline
prewarm and queued surface/depth-copy experiments retained. Shared draw
destinations remain selected. This is a diagnostic of the reported case,
not a paired comparison of a new optimization or a FullHD result.

The bounded capture contains 84,981 recorder events with zero drops, 366
complete flip intervals, 267 intervals over 50 ms and 219 GPU clock
calibrations. The scheduler covers the full 24.314-second native input
interval, with contemporaneous task ownership and no positive trace
error/data-loss counters. End-image review confirms a moving player at
105 km/h, position 8/8, lap 1/3, visible advanced rivals and rain. The FPS
overlay shows 11; complete final-ten-second intervals yield 12.773 FPS.
These rates include diagnostic overhead and are not an unprofiled baseline.

| Final ten-second diagnostic observation | Value |
| --- | ---: |
| Complete flip intervals | 127 |
| Median flip interval | 73.115 ms |
| Recorded texture-decode intervals | 80 |
| Texture-decode interval union / maximum | 26.186 / 3.581 ms |
| Recorded pipeline-create intervals | 8 |
| Pipeline-create interval union / maximum | 4.680 / 0.791 ms |
| Recorded shader-compile intervals | 0 |
| Recorded texture-image creation intervals | 0 |
| Aligned GPU interval union over this window | 4,578.732 ms |
| Median aligned GPU interval union within a flip interval | 36.599 ms |

GPU timestamp intervals include device dependencies, not just shader work;
host intervals overlap and must not be added into a frame budget. The small
recorded decode/compile totals in the final slow section do not explain the
sustained frame times by themselves. Earlier in the capture, individual
pipeline creation and texture-image creation reach 57.567 and 26.874 ms,
respectively, so loading/transitions still have separate transient costs.

Whole-input scheduling shows the vCPU running 80.05% of the interval and
PFIFO running 49.56%; the render worker runs 2.48%. These are individual
thread states, not total phone CPU usage or an additive cost decomposition.
Application cycle sampling records 348,697 samples with two kernel-space
samples lost and zero user-space loss. The actual APK native ELF and its
unstripped build share a verified build ID. Native symbol attribution is
restricted to that matching ELF; unknown execution addresses are not
assigned to guest instructions or translation blocks without a JIT map.

PFIFO call-chain attribution gives a specific generic target. In the final
ten seconds, the nearest resolved native caller is
`copy_surface_to_texture` for 22.00% of sampled PFIFO cycles. Its descendant
`__ioctl` accounts for 21.25%, and recovered Vulkan calls identify
`vkCmdCopyImage2` for 21.75% of PFIFO cycles, including the small cubemap
contribution. These percentages overlap and are not FPS gains. Across all
recovered PFIFO call chains, memory-allocation descendants of `__ioctl`
account for 21.88% in that section. Another 8.00% is associated with
`vkBeginCommandBuffer` through `pgraph_vk_begin_command_buffer`.

Matching-ELF source lookup places the dominant color-copy call in the
production `vk_surface_copy_record` helper used by the eligible queued
surface-to-texture route. Startup logs show a 1280x480 BGRA8-to-RGBA8
byte-reinterpretation copy at 1x. A replacement must preserve byte
interpretation, alpha, formats, scaled extents and submission ordering;
an ordinary format-converting blit is not a substitute for this path.
Testing a reusable GPU-buffer transfer route against the current image-copy
route, with exact generated-pixel checks, is now a concrete next experiment.
It remains a hypothesis until the real Barcelona race is compared without
diagnostic overhead. The vCPU also samples block lookup and floating-point
helpers, so removing this graphics cost alone does not establish 30 FPS.

Thermal status is 1 at both capture endpoints. This is a valid diagnostic
of the observed slow session, but not a thermally matched performance claim.
At 00:32:11 Madrid, the harness has reloaded the user's Slot 1, restored the
two diagnostic properties to their original zero values and released the
phone. Thumbnail/title hashes for all existing slots match before and after;
no snapshot is saved or overwritten. Raw game images, traces, samples,
addresses and harnesses remain private. No APK update, generic renderer
change or speedup is delivered by this diagnostic. Judge and CI/CD remain
disabled.

## 2026-10-13: persistent GPU-buffer color copies (perf48)

Perf48 adds an opt-in replacement for eligible queued RGBA8/BGRA8 color
surface copies. `debug.xemu.vk.surf_copy_buffer=1` selects image-to-buffer
and buffer-to-image transfers through dedicated persistent GPU scratch.
`XEMU_VK_SURFACE_BUFFER_COPY=1` provides the desktop override. Both are off
by default. Existing surface eligibility, texture versioning, destination
reuse rules and main submission ordering are retained; no game is identified
and no guest work is skipped.

Scratch is allocated once, separately from compute and download buffers,
using the existing bounded compute-buffer capacity (64 MiB on Android).
Allocation failure disables the experiment. Unsupported formats, zero or
overflowing extents and copies exceeding capacity retain the original image
copy. Transfers preserve stored bytes, including BGRA/RGBA reinterpretation,
without a CPU readback or a format-converting blit. Barriers retire previous
scratch reads, order its write before its read, and restore source/destination
image layouts. Cleanup covers normal shutdown and initialization failure.

The production helper passes 71,887,872 exact generated-pixel comparisons
across 16 color cases: RGBA/RGBA, BGRA/BGRA, RGBA/BGRA and BGRA/RGBA; three
queued versions with prior destination reads; extents through 3840x1440.
Host llvmpipe passes synchronization validation with zero errors. The same
fixture passes on Adreno 830 without a phone validation layer. Capacity,
unsupported-format, zero-size and overflow guards also pass. These checks
establish helper correctness, not whole-game correctness or FPS gain.

The final locally built/signed APK uses code 77 / `1.2.9-pgr2-perf48`, the
existing lab package and signing certificate. SHA-256 is
`b57b82d2f5423b93c0d6a3aa626c0658bb74a4cf9d962df849ff2f939685d7ec`.
The download copy matches; 98 selected actual build inputs match committed
source. One initial comparison stops before driving because its startup
selection log was conditional on general debug logging. The final APK
reports selection unconditionally at startup. A subsequent script uses an
invalid input-helper label and is rejected before touch injection. Both
attempts restore the scene and contribute no performance measurements.

Four completed unprofiled Barcelona starts then compare the original and
buffer routes in ABBA order. Each loads the user's current Slot 1, preserves
their current 1x settings, uses the same 24-second native touch plan and
checks the entire input interval for foreground and released controls.
Thermal status is 1 at both endpoints of every case.

| Route/order | Complete five-second-window mean |
| --- | ---: |
| Original, first | 12.845 FPS |
| Buffer, first | 14.455 FPS |
| Buffer, repeat | 14.243 FPS |
| Original, repeat | 11.567 FPS |

Full means mix presentation/countdown/racing progress and are not accepted
as a speedup. Pooling only windows starting at least ten seconds after
native input begins yields 11.840 FPS original and 12.555 FPS buffer;
individual later windows are inconsistent, with different progression and
only coarse thermal matching. End images show coherent wet roads, buildings,
HUD and moving cars at 76--84 km/h with advanced rival markers. This covers
short straight sections, not clean full laps or all textures.

A separately authorized five-minute window completes an ABA repeat after
waiting for the gameplay HUD and a fixed countdown delay. Whole-input means
are 11.614 / 11.905 / 11.480 FPS, original / buffer / original. Later complete
windows are 11.549 / 12.772 / 11.325 FPS. End images show the player moving at
69--73 km/h, coherent graphics and advanced rival markers. Review of the
ready images exposes a remaining phase-selection limit: HUD visibility and
a fixed delay do not prove the race timer is active; placeholder timer/lap
values are still visible. Early intervals must therefore be treated as
transitional, and even the later-window comparison is short and lacks
matched race positions. No sustained FPS improvement is accepted.

The last repeat restores Barcelona at 01:13:10 Madrid, before its 01:13:57
deadline. No snapshot-save operation occurs. All existing snapshot sidecar
hashes, all original preference values and the original diagnostic properties
are independently verified unchanged. A phone-local verified original HDD
backup is retained; neither a benchmark seed nor a replacement HDD is used
for these comparisons. The lab remains on the verified final Perf48 APK
with the original copy route selected. FullHD race performance and sustained
30 FPS remain unverified. Raw captures and harnesses stay private, and
Judge/CI/CD remain disabled.
### Perf49: retire uniform storage by submission slot

Barcelona's active-race diagnosis identifies the capacity finish in the
uniform allocator as a concrete source of long host waits. Perf49 gives each
active submission slot a separate device-local uniform destination. Uniform
capacity now has its own finish reason (9): FIFO can submit that bounded batch
and prepare the next slot, instead of waiting for the whole batch immediately.
The existing fence is still waited before reusing that slot's staging,
destination and command buffers.

This route is enabled by default with multiple active slots. The comparison
override is debug.xemu.vk.uniform_arenas=0 on Android or
XEMU_VK_UNIFORM_ARENAS=0 on desktop. Existing full-slot direct/staged draw
experiments retain their own selection. The uniform byte capacity and mobile
submission bound stay unchanged; index/vertex destinations retain their
shared-buffer ordering. Optional destination-allocation failure releases the
new destinations and falls back to the original shared uniform allocation
before any command or descriptor can reference them.

Descriptor renewal now includes uniform-only slot changes. Transfer visibility
covers both vertex and fragment uniform reads; the latter stage was missing
from the old transfer barrier. The production barrier helper is used directly
by the pixel fixture. Guest instructions, draw order, textures, guest fences,
controller code, preferences and saved-state code are unchanged.

On host llvmpipe, synchronization validation reports zero errors and all
393,216 exact RGBA pixel comparisons pass. The twelve cases cover one/two/three
slots with full-slot staged uploads, host-visible direct uploads, uniform-only
GPU storage with shared index/vertex destinations, and the fully shared
reference. Queued versions, fenced slot recycling, indexed/indirect draws and
uniforms consumed by both shader stages are checked. These are generated
correctness tests, not measured game FPS or full-game compatibility.

The arm64 fixture cross-compiles. The locally built APK is code 78,
1.2.9-pgr2-perf49, in the separate test package with the same local signing
certificate. Signature, manifest and actual arm64 correction strings are
verified. APK SHA-256:
3e69044916727ebd6308d342724b230e75f952af94385ada11efd6b49ae485cb.

The same arm64 fixture passes all 393,216 exact pixel comparisons on the
phone's Adreno 830. The phone has no synchronization validation layer, so
the zero-error validation-layer result applies only to host llvmpipe.
The signed APK is installed in the separate test package and reports three
fenced uniform destinations of 8 MiB each with the new route enabled.

The completed load-only Barcelona ON/OFF pair uses the signed Perf49 APK,
resolution 1x and the phone's existing settings. Capacity, copy routes,
instruction handling and other runtime options are held constant; no CPU/JIT
sampler or GPU recorder runs during the FPS measurements. Both runs start
after a numeric race timer is visible, then use the same 24-second native
touch sequence. Four complete five-second flip windows fall wholly inside
each input interval. ON gives 13.154 FPS and OFF 13.393 FPS; later windows
give 11.776 and 11.573 FPS. Thermal status remains 1 at both ends of both runs.
This pair demonstrates no appreciable FPS improvement. It does not establish
a regression either: end positions and speeds differ, and this is one short
pair rather than repeated matched trajectories.

Reviewed race-ready and end images show coherent wet roads, buildings and
HUD. The numeric timer advances from about 0:03.7 to 0:17.7, the player moves
and steers, and rival markers advance. End speeds are 85 km/h ON and 36 km/h
OFF; neither run covers a clean full lap or proves all-game compatibility.
No fatal exception, device loss, fatal signal or assertion appears in either
captured app log. Earlier harness checks incorrectly assumed an old runtime
profile and failed to allow timestamp padding; those checks are corrected.

The phone is restored at 02:41:30 Madrid, before the 02:42:44 deadline.
All existing snapshot sidecar hashes, original preference values and affected
runtime properties are verified unchanged. Slot 1 is only loaded; there is
no snapshot-save operation, replacement HDD or benchmark seed. The lab keeps
the verified Perf49 APK with its default uniform route; the original package
is untouched. Judge and CI/CD remain disabled, and private game/device assets
and phone harnesses are not committed.

Compilation, pixel tests and these short race measurements do not establish
sustained original speed or Full HD performance. No new mechanism trace yet
proves how much of the former capacity wait becomes slot-recycling wait or
other GPU dependency time. A Perf49 capture with matching native symbols is
prepared for that attribution; it has not been executed or accepted as an
FPS improvement.
