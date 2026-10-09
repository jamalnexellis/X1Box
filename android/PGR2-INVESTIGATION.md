# PGR 2 startup investigation

Base: official X1 BOX tag `1.2.9` (`dadced00cea8bcbada5a5650541cc5bac3cc93f4`).
Development application ID: `com.izzy2lost.x1box.dev`.
Startup and performance build: `1.2.9-pgr2-perf2`.

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

## Delivered binary

`perf2` source commit: `1ba08e1a1983fe9bb05cd0574baf1668ec42080f`.
[Successful build and native test artifacts](https://github.com/jamalnexellis/X1Box/actions/runs/37998447457).
APK SHA-256: `fe1cf9da9491781e50e887c74832b2da62e9fd35fd2309f8a7b054e15959d316`.
The standalone uniform packing test passes on the SM-S938B. The coroutine and
real-image media tests also passed on that device during this investigation.
The installed app has its SAF game folder configured, debug logging disabled
after measurement, and the tested Vulkan settings retained. Temporary device
profiling properties and USB stay-awake changes are restored. Firmware, EEPROM
and HDD tests use isolated copies; the user's original files remain untouched.
