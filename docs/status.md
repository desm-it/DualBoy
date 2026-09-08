# Implementation status

Last updated: 2026-09-08.

DualBoy now has a Nintendo DS engine implementation in addition to its existing
GB/GBC/GBA engines. The post-integration Linux x86-64 release suite and generated
content tests are recorded below. This document separates that automated
evidence from still-unverified real-RetroArch, retail/local-wireless, physical
controller, performance, suspend/resume, and compatibility boundaries.

## Verified upstream facts

- SameBoy, the libretro/mgba PR #318 snapshot, canonical mGBA, melonDS, the
  Libretro API header, and their licenses were inspected at the exact revisions
  recorded in [`THIRD_PARTY.md`](../THIRD_PARTY.md).
- GitHub reported
  [libretro/mgba PR #318](https://github.com/libretro/mgba/pull/318) open,
  unmerged, non-draft, and conflicting on 2026-09-08. Its head remained
  `fa743c965939f091350df094f57e639933bc17e3`, the pinned submodule commit.
- melonDS `906e9ebb27da8c6a715cd7abab4abfe8a8d29427` and tree
  `60c6724f8695bdbe5e21c4367b18293f7f811fc2` were pinned as an untouched
  submodule. On 2026-09-08 that commit matched upstream `HEAD` and `master`.
  The root license and selected bundled-source licenses were audited; the static
  combined binary is governed by GPLv3-compatible distribution terms.
- `JesseTG/melonds-ds` commit
  `bc4e4b67d2d470d7c682810a1e892cafd6f9082b` was inspected as a pattern
  reference only and is not a production dependency.
- The pinned Linux build environment ran as `linux/amd64` on the ARM64
  development host, and earlier artifacts were identified as x86-64 ELF shared
  objects built by GCC 12.2.0.

## Current post-NDS build and test results

On the final 2026-09-08 source, the exact primary release gate completed in the
digest-pinned `linux/amd64` container on an ARM64 Docker host:

```text
make test-linux-x86_64
100% tests passed, 0 tests failed out of 10
Total Test time (real) = 61.64 sec
sameboy_adapter_unit: 8.65 seconds
mgba_adapter_unit: 4.89 seconds
melonds_adapter_unit: 13.99 seconds
libretro_abi_smoke: 16.54 seconds
libretro_nds_pointer_integration: 16.78 seconds
```

The ten passing tests were `frontend_unit`, `session_unit`,
`state_unit`, `persistence_unit`, `save_manager_unit`,
`sameboy_adapter_unit`, `mgba_adapter_unit`, `melonds_adapter_unit`,
`libretro_abi_smoke`, and `libretro_nds_pointer_integration`. The preceding
`linux-x86_64` prerequisite release build also completed successfully with
`DUALBOY_WARNINGS_AS_ERRORS=ON`.

A native Linux ARM64 RelWithDebInfo diagnostic configured with warnings as
errors, rebuilt the current source, and passed the same 10/10 tests in 6.40
seconds. `git submodule update --init --recursive` succeeded and reported the
four exact revisions in `THIRD_PARTY.md`; `third_party/melonDS` remained clean.

The handoff's three direct host commands were attempted and each exited 127
because this macOS host has neither `cmake` nor `ctest` installed:

```text
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
zsh: command not found: cmake

cmake --build build
zsh: command not found: cmake

ctest --test-dir build --output-on-failure
zsh: command not found: ctest
```

The aggregate host target was also attempted and failed at its configure step:

```text
make test
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
make: cmake: No such file or directory
make: *** [build/build.ninja] Error 1
```

The container builds above execute the same configure/build/CTest phases; the
missing host tool is an environment limitation, not recorded as a host-build
pass.

## NDS implementation now present

Source inspection of the current working tree shows:

- the exact untouched melonDS gitlink built internally with its regular OpenGL
  renderer enabled and its desktop frontend, JIT, GDB stub, release LTO, and
  embedded build metadata disabled;
- two `melonDS::NDS` objects with distinct userdata, video, cartridge SaveRAM,
  128 KiB generated firmware, locally administered MAC addresses, and persistent
  frame workers;
- one pair-owned upstream `LocalMP`, with its 16-instance capacity unchanged but
  DualBoy registering only IDs 0 and 1 and allowing link enable/disable without
  content reload;
- an experimental NDS OpenGL Core 3.2 path that is mutually exclusive with
  `LocalMP`, owns one explicit surfaceless EGL display and two unshared context
  slots on a dedicated adapter worker, reads both texture layers back into the
  common CPU compositor, and leaves a complete software pair when private EGL
  negotiation is unavailable;
- dynamic Libretro fast-forward inhibition only while both NDS instances report
  themselves joined to the enabled transport;
- fixed 256x384 top-over-bottom per-machine frames, structured controller/touch
  input, compositor-aware bottom-screen pointer routing, and per-player
  right-stick/R3 stylus fallback with a high-contrast aim reticle that hides
  after three seconds without meaningful stick movement;
- engine debug messages discarded at the platform entrypoint before formatting
  or allocation, while info, warning, and error messages still reach the
  frontend;
- callback-committed NDS cartridge persistence: live melonDS SRAM may differ
  while an SPI transaction is in progress, completed callback ranges flow into
  the disk-facing shadow (including end-of-buffer wrap), and stale shadow bytes
  are applied only at explicit load/reset boundaries;
- normal same-ROM, subsystem, and M3U NDS selection with strict header detection
  and mixed-family rejection; and
- core-managed independent `.srm` and `.firmware.bin` files, with collision
  suffixes for duplicated cartridge identities.

NDS state callbacks are intentionally absent: manual savestates, rewind, and
runahead are unsupported instead of pretending that machine state contains the
pair-owned `LocalMP` queues. The required per-core override is:

```ini
rewind_enable = "false"
run_ahead_enabled = "false"
```

## Automated coverage exercised

The repository's test sources use generated synthetic ROMs rather than commercial
content. The passing native and x86-64 runs above exercised:

- Nintendo-header detection, family compatibility, composition geometry,
  player-only layouts, and coupled screen/controller swap;
- shared immutable normal-load content with independent mutable sessions,
  two-slot subsystem ownership, and idempotent partial-failure cleanup;
- paired save-state round trips, link payload restoration, battery/RTC
  preservation across state load, corrupt-container rejection before mutation,
  rollback, fatal rollback handling, and rejection of malformed inner mGBA
  scheduler event counts, types, and player IDs without live mutation;
- save-path stems and collision suffixes, local two-entry M3U parsing and
  rejection cases, atomic file replacement, canonical-over-legacy precedence,
  GBA `.sav` copy import, appended 16-byte RTC splitting, advisory-lock
  contention/read-only fallback, pathless identities, and dynamic mGBA extents;
- two live SameBoy instances using generated GB/GBC ROMs, symmetric link
  callbacks, distinct input/video/audio/state, and disk SaveRAM/RTC round trips
  after destroying and recreating both engines;
- two live mGBA instances using a generated ARM ROM, two-way SIO values, assigned
  player IDs, distinct controls/video/audio/state, dynamic save-type sizes, and a
  disk SaveRAM round trip after destroying and recreating both engines;
- generated GBA startup bursts containing more than 64 alternating RCNT mode
  changes per machine, with lossless event delivery, a measured queue high-water
  below the eight-event capacity, converged peer modes, and a subsequent
  two-way multiplayer transfer; and
- dynamic loading of the production shared object, ABI/interface registration,
  normal, subsystem, pathless, and M3U content paths, live option changes,
  paired state, SameBoy and GBA execution, and exact 32 KiB generated-ROM GBA
  saves;
- strict NDS header and CRC validation, NDS/non-NDS mixed-family rejection,
  512x384/256x768 compositor geometry, and inverse point mapping;
- two live melonDS objects executing generated ARM programs concurrently,
  distinct userdata, controls, touch coordinates, 256x384 video buffers,
  cartridge SaveRAM, validated generated firmware and unique persistent MAC
  addresses, plus partial and repeated load/unload cleanup;
- `LocalMP` registration restricted to IDs 0 and 1, a raw same-process packet
  round trip, live link disable/re-enable, transport-active transitions, and a
  stopped-worker failure followed by reset/recovery;
- a generated ARM7 program writing emulated `POWCNT2` and `W_POWER_US`, causing
  each real melonDS Wi-Fi device to reach `Platform::MP_Begin`; dynamic
  fast-forward inhibition then activates, clears when link is disabled, and
  returns when both machines rejoin;
- two simultaneous pointer indices passed through the public Libretro callbacks,
  the production compositor transform, two real melonDS objects, and actual TSC
  conversion reads with independently asserted coordinates;
- right-stick aiming shown before R3 touch, exact R3 coordinates reaching the
  real melonDS TSC, direct-pointer precedence, exact three-second expiry through
  the Libretro clock, 180-frame fallback timing, two-pixel drift filtering,
  layout/swap projection, edge clipping, and black/white contrast over light,
  dark, and colored frames;
- high-volume melonDS debug logging rejected before formatting/allocation, with
  info, warning, and error forwarding retained;
- a deterministic held-worker deadline test proving that a timed-out frame does
  not return before both workers quiesce, that the pair is poisoned, and that
  memory access and destruction are safe afterward;
- a real upstream 8 KiB EEPROM transaction left open across a DualBoy frame,
  proving live bytes remain unpublished until `SPIRelease` and then reach the
  shadow through `Platform::WriteNDSSave`, plus a wrapped callback covering the
  physical tail and prefix and a zero-masked full-device callback; and
- the exact 300-frame periodic save boundary, a forced flush, full 8 KiB disk
  image comparisons, a present Player 1 save with an absent `.srm.2`, distinct
  checksummed records for both machines, independent firmware images, and two
  destroy/recreate cycles restoring byte-identical shadow and live SRAM.

These are synthetic-harness results, not retail-game or real-RetroArch
compatibility claims. The generated cartridges and ARM instructions are authored
in the test sources; the fixed NDS logo bytes are hardware-mandated header data.
No downloaded homebrew, commercial program, Nintendo BIOS, or firmware dump was
used.

The disk round trips prove more than buffer independence. Generated guest code
changes SameBoy and mGBA SaveRAM. For NDS, a test-only hook drives the real
upstream `CartRetail` EEPROM SPI protocol through the adapter test interface and
its real `Platform::WriteNDSSave` callback; it is not an ARM guest save program.
Distinct per-machine records are flushed, both engine pairs are destroyed, new
pairs are created, and a new save manager loads the complete expected images
from separate files. The SameBoy case also checks its separate RTC regions.

## Current sanitizer, symbol, and package results

The current source completed the native ARM64 ASan+UBSan command:

```text
make asan-linux-native
100% tests passed, 0 tests failed out of 10
Total Test time (real) = 120.51 sec
```

This is not a clean UBSan claim. CTest returned success and the log contains no
AddressSanitizer error, but its passing-test output contains 22 recoverable UBSan
diagnostics: six from untouched SameBoy (`gb.c` null-pointer argument and
`sm83_cpu.c` negative shift) and sixteen from untouched melonDS (`CP15.cpp` and
`NDS.cpp` unaligned 32-bit accesses plus `SPU.cpp` zero-bound VLA). The melonDS
diagnostics occur in the real adapter and Libretro integration tests. Upstream is
intentionally unmodified, so these are recorded sanitizer blockers/technical
debt rather than suppressed or represented as a clean pass. An emulated
`linux/amd64` ASan process is not used on this ARM host because its virtual
address-space setup is unreliable.

`make symbols-linux-x86_64` printed:

```text
Libretro export check passed for build-linux-x86_64/dualboy_libretro.so
```

`git diff --check` also passed on the final source and documentation changes.

The current release build was installed with:

```sh
docker run --rm --platform linux/amd64 -u "$(id -u):$(id -g)" \
  -v "$PWD:/src" -w /src dualboy-linux-x86_64:bookworm \
  cmake --install build-linux-x86_64 --prefix /src/dist/linux-x86_64
```

The install tree contains the core and metadata plus `LICENSE`, `NOTICE`,
`THIRD_PARTY.md`, and these eleven component license/provenance files under
`share/doc/dualboy/licenses`: `FatFs.txt`, `FreeBIOS-BSD-2-Clause.txt`,
`GLAD-generated-code.txt`, `Khronos-Apache-2.0.txt`,
`Khronos-khrplatform.txt`, `SameBoy-Expat.txt`, `Teakra-MIT.txt`,
`blip-buf-LGPL-2.1.txt`, `mGBA-MPL-2.0.txt`, `melonDS-GPL-3.0.txt`, and
`tiny-AES-c-Unlicense.txt`.

The primary artifacts are:

```text
dist/linux-x86_64/lib/libretro/dualboy_libretro.so
dist/linux-x86_64/share/libretro/info/dualboy_libretro.info
```

`file` and `readelf -h` identify the core as ELF64, little-endian, System V,
x86-64 (`Advanced Micro Devices X86-64`), dynamically linked. The files have:

```text
69e7f426ca6bc0faa05b05de441238c697bb89b922e105f89a21330f6468db8a  dualboy_libretro.so
f16a80e35815d46705b92f5ef45b117ec78a7395ee59535ee4b63570e21f83be  dualboy_libretro.info
```

The installed shared object also passed `tools/check-libretro-symbols.sh`; no
melonDS, adapter-test, or nested Libretro entrypoint is exported. The first host
`shasum` attempt was interrupted by that host's invalid `C.UTF-8` Perl locale;
`sha256sum` in the pinned Linux container produced the hashes above.

## Implemented MVP surface

Implementation plus the automated evidence above establish these components:

- one exported Libretro core, XRGB8888 output, two RetroPad ports, 59.7275 Hz
  timing for GB/GBC/GBA, 59.8260982880808 Hz for NDS, and 48 kHz stereo output
  sourced only from machine 0;
- two-instance SameBoy for GB/GBC and two-instance mGBA with the adapted PR #318
  cooperative SIO scheduler for GBA;
- two-instance melonDS with two concurrent software/LocalMP frame workers or a
  sequential experimental OpenGL 3.2 path, built-in BIOS replacements,
  independent generated firmware, and pair-owned upstream same-process
  `LocalMP` for NDS;
- normal same-ROM, exactly-two-ROM `dualboylink` subsystem, and exactly-two-entry
  local M3U loading, with header-first detection and mixed-family rejection;
- side-by-side, top/bottom, Player 1 only, Player 2 only, coupled swap, mutually
  exclusive NDS OpenGL/local-link selection, link enable/disable, and Player
  1/disabled audio core options;
- collision-safe independent SaveRAM/RTC, hash-derived pathless identities,
  nondestructive GBA `.sav` import, atomic writes, per-region
  `.dualboy.lock` coordination, and read-only behavior under contention;
- stable 128 KiB mGBA SaveRAM shadows with dynamically detected on-disk extents;
  and
- one versioned, checksummed, transactional container for both SameBoy or mGBA
  machines and link state, with battery memory separated from state rollback;
  NDS states remain disabled.

This inventory is not a compatibility statement for retail software.

## Physical Steam Deck finding and mitigation

On 2026-09-07, a physical Steam Deck running RetroArch 1.22.2 loaded DualBoy
through RomM-Dock. Several GBA titles reached the boot logo and then terminated
RetroArch with `SIGSEGV`. Systemd coredump stacks consistently ended in
`_enqueueEvent` from `DualBoyGBASIOLockstepDriverSetMode`; disassembly confirmed
the release build dereferenced `player->freeList == NULL` after the inherited
eight-entry PR #318 queue was exhausted.

The fixed queue now uses the original eight-event capacity, with
current/high-water telemetry and a guarded failure path. Normal delivery is
event-driven: publishing a mode change yields the producing core, an earlier
queue head advances the receiving core's existing timing event, and a core that
has crossed its frame boundary still runs while link events remain pending. A
source-generated regression emits more than 64 alternating RCNT mode changes per
machine, measures a queue high-water of two events, observes zero drops and
converged modes, drains both queues, and then completes a two-way multiplayer
transfer. The version-2 link-state envelope remains unchanged for compatibility;
states containing at most eight pending events per machine remain readable,
while states containing more than the runtime queue can hold are rejected.
Battery-save formats are unchanged.

The replacement core has not yet been rerun with the affected commercial games,
so this is a synthetic regression fix, not yet a verified per-game fix. The
eight-event guard still drops an event instead of dereferencing a null free-list
entry if a future producer/consumer pattern defeats cooperative scheduling; any
such error remains a potential desynchronization and should be investigated.

## Unverified runtime boundaries and known MVP limits

- A pre-NDS build loaded in real RetroArch on Steam Deck, but controller ordering,
  performance, suspend/resume, and long-session persistence remain unverified.
- No NDS content has yet been loaded in a real RetroArch process or on a physical
  Steam Deck. The earlier Deck launch and GBA crash investigation do not validate
  the NDS path.
- The NDS OpenGL path deliberately does not use a frontend context. Its
  automated accepted-path gate uses Mesa's software rasterizer with explicit
  surfaceless EGL, proving context creation, two-instance rendering/readback,
  reset, live renderer/link transitions, teardown, and reload without proving
  hardware acceleration. Real RetroArch output/orientation and fallback
  messaging plus physical Steam Deck GPU selection, performance, suspend/resume,
  and long-session behavior remain unverified. The serial two-machine GL worker
  and CPU readback/composition mean a speedup must be measured, not assumed.
- The reported Pokémon Diamond save has not been rerun on its Steam Deck and was
  not imported into the test environment. The synthetic regression proves a
  general `CartRetail` corruption mechanism. Source inspection shows that the
  observed 512 KiB FLASH extent uses the same write-before-`SPIRelease` ordering,
  but this is an inference rather than direct commercial runtime validation. It
  does not prove that the existing file is repaired or that every cause of its
  invalid checksums is resolved. DualBoy deliberately has no game-specific
  repair logic; an already-corrupt save must be restored or replaced outside
  the core.
- No source-built or explicitly redistributable guest program has completed an
  emulated NDS local-wireless session. The generated ARM7 fixture powers on both
  emulated Wi-Fi devices and reaches `MP_Begin`, while a separate adapter test
  sends a raw packet through upstream `LocalMP`; no guest program sends or
  receives that packet. Retail/local-wireless compatibility remains unverified.
- Two simultaneous Libretro pointer indices have not yet been verified on the
  physical Deck/input-driver combination. The public-ABI automated test reaches
  two real melonDS TSC devices, but that does not establish hardware-driver
  behavior.
- NDS manual states, rewind, and runahead are unsupported. Users must disable
  rewind and runahead in the per-core override.
- NDS reports 59.8260982880808 Hz; GB/GBC/GBA retain 59.7275 Hz. NDS timing and
  performance have not been measured in a real frontend.
- The NDS ten-second worker deadline is soft. It safely poisons a pair and waits
  for quiescence, but a permanently wedged upstream `NDS::RunFrame()` cannot be
  cancelled and can still block `retro_run()` or unload indefinitely. Hard
  bounding requires upstream cancellation support or process isolation.
- The native sanitizer command exits successfully and has no ASan finding, but
  its log contains the 22 untouched-upstream UBSan diagnostics recorded above;
  this is not a clean UBSan pass.
- Commercial games were used only for user-run startup/crash observation. No
  commercial ROMs are stored in this repository, and no broad compatibility or
  gameplay-completion claim is made.
- SameBoy's subsystem advertises distinct custom SaveRAM and RTC IDs for both
  slots, and the automated harness observes distinct pointers. Actual RetroArch
  disk persistence of both custom RTC IDs is still unverified. Use M3U for
  core-managed independent RTC files until this is resolved.
- Only machine 0 / Player 1 audio is emitted. There is no mixer or machine-1 audio
  selection.
- There is no Internet/WFC, LAN or RetroArch netplay, real-DS connectivity,
  Download Play, DSi system/DSiWare, Slot-2 integration, machine-1 audio, or
  core-managed cheats. DS-compatible UnitCode 2 cartridges are accepted in DS
  mode; DSi-exclusive UnitCode 3 content is rejected. Cheat callbacks are no-ops.
- GBA link topology changes reset both machines by design.
- Lockstep timestamp ordering uses the standard 32-bit modular half-range rule.
  All actual event intervals are far below half the clock range, but a continuous
  run crossing the counter's sign boundary has not been separately accelerated
  as a dedicated runtime test.

## Environment

The development host is macOS ARM64. Docker supplies both the pinned Debian
`linux/amd64` release environment and a native Linux ARM64 sanitizer environment.
Native host/container diagnostics do not replace a Linux x86-64 release suite.
Neither a RetroArch executable nor Steam Deck hardware was available in the NDS
implementation environment; the earlier physical Deck observation was a separate
user-run pre-NDS test.
