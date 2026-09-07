# Implementation status

Last updated: 2026-09-07.

DualBoy's MVP feature set is implemented. The strict Linux x86-64 release suite,
native Linux sanitizer suite, exported-symbol check, and install-tree check pass.
A real RetroArch process and physical Steam Deck remain external validation
boundaries. This document separates observed results from source-level inventory
so a build artifact is not mistaken for completed platform validation.

## Verified upstream facts

- SameBoy, the libretro/mgba PR #318 snapshot, canonical mGBA, the Libretro API
  header, and their licenses were inspected at the exact revisions recorded in
  [`THIRD_PARTY.md`](../THIRD_PARTY.md).
- GitHub reported
  [libretro/mgba PR #318](https://github.com/libretro/mgba/pull/318) open,
  unmerged, non-draft, and conflicting on 2026-09-07. Its head remained
  `fa743c965939f091350df094f57e639933bc17e3`, the pinned submodule commit.
- The pinned Linux build environment ran as `linux/amd64` on the ARM64
  development host, and earlier artifacts were identified as x86-64 ELF shared
  objects built by GCC 12.2.0.

## Current strict Linux x86-64 result

The final source tree completed the reference container command successfully:

```text
make test-linux-x86_64
100% tests passed, 0 tests failed out of 8
Total Test time (real) = 29.00 sec
sameboy_adapter_unit: 9.96 seconds
mgba_adapter_unit: 4.64 seconds
libretro_abi_smoke: 13.93 seconds
```

The eight passing tests were `frontend_unit`, `session_unit`,
`state_unit`, `persistence_unit`, `save_manager_unit`,
`sameboy_adapter_unit`, `mgba_adapter_unit`, and
`libretro_abi_smoke`.

## Test coverage present in the tree

The repository's test sources use generated synthetic ROMs rather than commercial
content. Source inspection confirms that the registered tests cover:

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
  disk SaveRAM round trip after destroying and recreating both engines; and
- dynamic loading of the built Libretro core, ABI/interface registration, normal,
  subsystem, pathless, and M3U content paths, live option changes, paired state,
  SameBoy and GBA execution, and exact 32 KiB generated-ROM GBA saves.

These bullets describe the assertions exercised by the passing current suite.
They remain synthetic-harness results, not retail-game or real-RetroArch
compatibility claims.

The engine disk round trips prove more than buffer independence: guest code first
changes each live engine's SaveRAM, distinct per-machine sentinels are flushed,
both engine pairs are destroyed, new pairs are created with poisoned/blank
memory, and a new save manager loads the expected bytes from separate on-disk
paths. The SameBoy case performs the same destroy/recreate check for its separate
RTC regions. A test that only compared two live pointers would not establish
cross-lifetime persistence.

## Sanitizer, symbol, and package results

The complete suite also passed with AddressSanitizer and UndefinedBehaviorSanitizer
in a native Linux container on the ARM64 development host:

```text
make asan-linux-native
100% tests passed, 0 tests failed out of 8
Total Test time (real) = 25.87 sec
```

`make asan-linux-x86_64` was attempted on this ARM64 Docker Desktop host. All
eight x86-64 ASan processes were killed by the emulation environment before test
code produced output; verbose diagnosis showed the ASan interceptor/address-space
setup failing under emulated `linux/amd64`. This is why the Makefile has a native
sanitizer gate. It is not recorded as an x86-64 sanitizer pass.

`make symbols-linux-x86_64` printed:

```text
Libretro export check passed for build-linux-x86_64/dualboy_libretro.so
```

The release build was installed with:

```sh
docker run --rm --platform linux/amd64 -u "$(id -u):$(id -g)" \
  -v "$PWD:/src" -w /src dualboy-linux-x86_64:bookworm \
  cmake --install build-linux-x86_64 --prefix /src/dist/linux-x86_64
```

The resulting install tree contains:

```text
dist/linux-x86_64/lib/libretro/dualboy_libretro.so
dist/linux-x86_64/share/libretro/info/dualboy_libretro.info
```

`file` and `readelf -h` identify the core as ELF64, little-endian, System V,
x86-64 (`Advanced Micro Devices X86-64`), dynamically linked. The files have:

```text
e4a6f500df977deb0962e81987345ec13e8cbbc408b924e03ed1992494ea0260  dualboy_libretro.so
3ea55624bb8661d1336a239207fdd81109df37ea1e5c7c48aea0d038737685c4  dualboy_libretro.info
```

The installed shared object passed the same Libretro export allowlist check.

## Implemented MVP surface

Source and test inspection show these components in the working tree:

- one exported Libretro core, XRGB8888 output, two RetroPad ports, 59.7275 Hz
  timing, and 48 kHz stereo output sourced only from machine 0;
- two-instance SameBoy for GB/GBC and two-instance mGBA with the adapted PR #318
  cooperative SIO scheduler for GBA;
- normal same-ROM, exactly-two-ROM `dualboylink` subsystem, and exactly-two-entry
  local M3U loading, with header-first detection and mixed-family rejection;
- side-by-side, top/bottom, Player 1 only, Player 2 only, coupled swap, link
  enable/disable, and Player 1/disabled audio core options;
- collision-safe independent SaveRAM/RTC, hash-derived pathless identities,
  nondestructive GBA `.sav` import, atomic writes, per-region
  `.dualboy.lock` coordination, and read-only behavior under contention;
- stable 128 KiB mGBA SaveRAM shadows with dynamically detected on-disk extents;
  and
- one versioned, checksummed, transactional container for both machines and link
  state, with battery memory separated from state rollback.

This inventory is not a compatibility statement for retail software.

## Unverified runtime boundaries and known MVP limits

- No end-to-end load has been performed in an actual RetroArch process. The
  `libretro_abi_smoke` executable is a custom `dlopen` frontend harness, not
  RetroArch itself.
- No build, install, controller, performance, suspend/resume, or persistence
  check has been performed on physical Steam Deck hardware.
- No commercial ROM has been used, and no commercial-game compatibility or
  gameplay-completion claim is made.
- SameBoy's subsystem advertises distinct custom SaveRAM and RTC IDs for both
  slots, and the automated harness observes distinct pointers. Actual RetroArch
  disk persistence of both custom RTC IDs is still unverified. Use M3U for
  core-managed independent RTC files until this is resolved.
- Only machine 0 / Player 1 audio is emitted. There is no mixer or machine-1 audio
  selection.
- Link netplay, wireless multiplayer, fast-forward coordination, rewind
  coordination, and core-managed cheats are outside the MVP. Cheat callbacks are
  no-ops.
- GBA link topology changes reset both machines by design.
- Lockstep timestamp ordering uses the standard 32-bit modular half-range rule.
  All actual event intervals are far below half the clock range, but a continuous
  run crossing the counter's sign boundary has not been separately accelerated
  as a dedicated runtime test.

## Environment

The development host is macOS ARM64. Docker supplies both the pinned Debian
`linux/amd64` release environment and a native Linux ARM64 sanitizer environment.
Native host/container diagnostics do not replace the passing Linux x86-64 release
suite. Neither a RetroArch executable nor Steam Deck hardware was available in
the recorded test environment.
