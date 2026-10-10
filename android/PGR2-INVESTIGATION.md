# PGR 2 startup investigation

Base: official X1 BOX tag `1.2.9` (`dadced00cea8bcbada5a5650541cc5bac3cc93f4`).
Development application ID: `com.izzy2lost.x1box.dev`.
Latest tested startup and performance build: `1.2.9-pgr2-perf7`.

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
Initial `perf3` captures contain controller input during the comparison and are
excluded from quantitative FPS claims. The repeat explicitly refreshes trigger
and stick release states after restoring the VM snapshot, then measures without
controller input in the timed window. The `perf4` comparison still shows most
dynamic upload bytes on the transfer path and no clear FPS improvement. A menu
was opened before the end of one capture, so paused time cannot be counted as
gameplay. The uninterrupted `perf5` pair at a stationary Florence scene gives
12.571 FPS with transfers and 11.960 FPS with compute uploads. These are four
complete five-second windows per run, excluding snapshot restore and scripted
controller reset. Both runs finish at 0 km/h with coherent road, buildings and
HUD. Neither reports Android thermal throttling. The transfer path still
records 338.2 MB / 919.85 ms; compute leaves 241.6 MB / 699.11 ms in transfers
and records 48.6 MB / 4.98 ms of compute uploads. The repeat therefore does
not establish an FPS gain. Repeated last-upload samples identify RGB565
256x256; their byte totals suggest cubemaps, which perf6 explicitly covers
and includes in the diagnostic metadata. Perf6 confirms those repeated uploads
are RGB565 cubemaps with one mip. All sixteen size/mip/face fixtures, three
writes per image, all 65,536 RGB565 words, BGRA/forced-alpha sampling and raw
image-copy comparisons pass on the Adreno 830. The uninterrupted perf6 pair
reports 12.180 FPS with transfers versus 12.051 FPS with compute. It processes
the same 338.2 MB: transfer command recording takes 888.43 ms, versus 16.22 ms
for 338.1 MB of compute and 0.13 ms for the remaining 0.1 MB of transfers.
This removes the measured upload recording hotspot but still does not improve
the measured frame rate. Both end captures show the same stationary car with
coherent buildings, road and HUD, and thermal status remains 0.

A subsequent 15-second sampling profile is separate from the FPS captures.
The guest CPU thread accounts for 64.69% of sampled CPU cycles; the graphics
FIFO thread accounts for 21.19%. CPU-cycle proportions alone do not establish
the frame's critical path. The graphics profile now places 11.65% of its
cycles in surface-to-texture image copies, mostly driver allocation in
vkCmdCopyImage. Forcing the existing direct-surface diagnostic path does not
establish a gain either (11.908 FPS in the short stationary repeat), and the
property is restored to its original empty value. Its mobile-driver workaround
remains enabled. Perf7 enables the existing native ThinLTO build option for the
development APK and disables NV2A_PERF_LOG, removing phase timers, optimization
counters and per-render-pass GPU timestamp queries. The existing flip-based
FPS measurement remains available. Floating-point precision and runtime
settings are unchanged. All eighteen GPU comparisons, uniform packing,
1,000 coroutine migrations and the real PGR 2 image test pass with the perf7
executables on the same device. The real-image test still verifies that the
original disc byte is unchanged. The signing key is restored from CI's explicit
cache; perf6/perf7 updates and the diagnostic downgrade preserve app data and
the existing SAF grant.

The consecutive perf7/perf6 comparison uses the same snapshot and input reset,
compute uploads enabled, no sampling profiler and no user input. Four complete
five-second windows report 12.506 FPS for perf7 and 12.251 FPS for perf6.
The nominal 2.1% difference in this short pair does not establish a repeatable
performance improvement or resolved cold-rendering stalls. Both end captures
show the same stationary scene with coherent buildings, road, car and HUD;
Android thermal status remains 0. This comparison excludes the first five
seconds following snapshot load and input reset, so it does not quantify the
initial restore hitch. Perf7 intentionally omits the upload profiling counters;
the separate perf6 transfer/compute comparison above measures their cost.

The final phone installation is perf7. A fresh backup taken before this pair
is restored afterward, including private HDD/snapshots, shader cache, preferences
and the app-owned EEPROM. Debug logging is disabled again. Temporary compute
and direct-surface properties are cleared to their original empty values, and
USB stay-awake returns to its original value of 0. The slowdown remains an open
issue; no full-speed or maximum-FPS claim is made.

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

## Latest delivered binary

`perf7` source commit: `804d9072afc1cc86966f6267d29c4a14b6fe6ef3`.
[Successful build and native test artifacts](https://github.com/jamalnexellis/X1Box/actions/runs/38008983718).
APK version: `1.2.9-pgr2-perf7`, version code 36.
APK SHA-256: `50924571fe99695bbcef12ab8c3c0f7d8912e323b969e70fd8b04f8ae877332e`.
The artifact checksum and the delivered APK copy match. This binary includes
the original startup adaptation, uniform-copy optimization, pixel-tested GPU
texture uploads, native ThinLTO and disabled rendering profiling overhead.
Private phone backups, firmware, game images, logs and screenshots are not
included in the repository or CI artifacts.
