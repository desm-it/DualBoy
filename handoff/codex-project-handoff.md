# Codex handoff: build DualBoy, a dual-instance GB/GBC/GBA Libretro core

You are the primary implementation agent for a new open-source project named **DualBoy**. Work autonomously: inspect the workspace, create or update the repository, implement the software, run real builds/tests, fix failures, and leave the repository in a clean, documented state. Do not stop after writing a plan or scaffolding.

If the workspace is empty, initialize a Git repository first. Before substantial implementation, create a concise root `AGENTS.md` describing the architecture, commands, constraints, and current status so future agents can continue safely. Maintain a detailed but concise `docs/architecture.md` and `docs/status.md` as the design and implementation evolve.

## Product goal

Build one Linux-compatible Libretro core, `dualboy_libretro.so`, that runs two linked handheld emulator instances in one RetroArch session:

- GB and GBC through the SameBoy engine.
- GBA through the mGBA engine, starting from/adapting the work in libretro/mgba PR #318.
- One combined video framebuffer containing both emulated screens.
- RetroPad port 1 controls machine/screen 1.
- RetroPad port 2 controls machine/screen 2.
- Loading one ROM normally starts two independent instances of that ROM by default.
- A paired-content mechanism loads two different ROMs, for example Pokémon Red plus Pokémon Blue.
- Each machine has independently persisted battery save and RTC data.
- Link-cable emulation is enabled by default when dual mode is active.
- Target platform is initially RetroArch on Linux x86-64, especially Steam Deck/SteamOS.

This is a single Libretro frontend linked to emulator engine code. **Do not attempt to load or nest `sameboy_libretro` or `mgba_libretro` cores inside DualBoy.** Instantiate the engines through their internal APIs.

## Authoritative upstream references

Inspect these sources directly before choosing APIs or copying code:

1. SameBoy upstream and its proven dual-instance Libretro implementation:
   - https://github.com/LIJI32/SameBoy
   - https://github.com/LIJI32/SameBoy/blob/master/libretro/libretro.c
   - https://docs.libretro.com/library/sameboy/
2. mGBA upstream:
   - https://github.com/mgba-emu/mgba
3. Open mGBA split-screen multiplayer work:
   - https://github.com/libretro/mgba/pull/318
   - Known inspected PR head at handoff time: `fa743c965939f091350df094f57e639933bc17e3`
   - https://github.com/libretro/mgba/blob/fa743c965939f091350df094f57e639933bc17e3/src/platform/libretro/libretro_multiplayer.c
   - https://github.com/libretro/mgba/blob/fa743c965939f091350df094f57e639933bc17e3/src/platform/libretro/libretro_lockstep.c
4. Canonical Libretro API:
   - https://github.com/libretro/libretro-common/blob/master/include/libretro.h

Pin exact upstream commits. Record the commit IDs, source URLs, licenses, local modifications and update procedure in `THIRD_PARTY.md`. Preserve all required copyright and license notices.

Licensing assumptions to verify, not blindly trust:

- SameBoy: MIT/Expat-style license.
- mGBA: MPL-2.0.
- DualBoy's own license must be compatible with both and clearly documented. Do not copy GPL-only code from TGB Dual, VBA-M, Gearboy or other projects merely for convenience.

## Architecture

Create a thin engine-independent Libretro frontend with adapters similar to:

```c
struct dualboy_engine_ops {
    bool (*create_pair)(...);
    bool (*load_rom)(unsigned machine, const void *data, size_t size, ...);
    bool (*connect_link)(...);
    void (*reset)(...);
    void (*run_frame)(...);
    uint32_t *(*video_buffer)(unsigned machine, ...);
    void (*set_input)(unsigned machine, uint16_t buttons);
    bool (*save_ram_info)(unsigned machine, void **data, size_t *size);
    bool (*rtc_info)(unsigned machine, void **data, size_t *size);
    size_t (*serialize_size)(...);
    bool (*serialize)(...);
    bool (*unserialize)(...);
    void (*destroy_pair)(...);
};
```

This is illustrative, not a mandated ABI. Keep ownership explicit and avoid shared mutable globals in DualBoy. The two engines may require different timing/link orchestration internally.

Suggested repository shape:

```text
dualboy/
├── AGENTS.md
├── Makefile
├── CMakeLists.txt
├── dualboy_libretro.info
├── LICENSE
├── THIRD_PARTY.md
├── README.md
├── docs/
│   ├── architecture.md
│   ├── saves.md
│   ├── status.md
│   └── steam-deck.md
├── src/
│   ├── libretro/
│   ├── frontend/
│   ├── engines/sameboy/
│   └── engines/mgba/
├── tests/
└── third_party/
    ├── sameboy/
    └── mgba/
```

Prefer reproducible submodules or a documented vendoring script. Do not depend on floating upstream branches during normal builds.

## Content loading

Implement these modes:

### 1. Normal single-content load — mandatory

`retro_load_game()` receives one `.gb`, `.gbc` or `.gba` ROM and:

1. Detects the platform from content/header, not only the filename extension.
2. Selects SameBoy for GB/GBC or mGBA for GBA.
3. Creates two independent engine instances.
4. Loads the same immutable ROM bytes into both.
5. Connects the appropriate emulated link cable.
6. Assigns independent save/RTC storage.
7. Starts side-by-side dual display.

Dual mode and link mode are enabled by default.

### 2. Two-content load — mandatory

Register a two-ROM Libretro subsystem in `retro_set_environment()` with `RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO`, and implement `retro_load_game_special()`.

Use an identifier such as `dualboy_link_2p`. Both entries are required and accept `gb|gbc|gba`. Runtime detection decides the engine.

Rules:

- GB + GB, GBC + GBC and compatible GB + GBC combinations use SameBoy.
- GBA + GBA uses mGBA.
- Mixed GB/GBC + GBA is rejected with a clear frontend log message.
- Each subsystem content slot declares its own SaveRAM and RTC memory descriptors where frontend support permits.
- Never allow both machines to reference the same writable save buffer.

### 3. M3U paired-content convenience — desirable after subsystem support

Support an `.m3u` containing exactly two ROM paths so Steam/EmulationStation can launch a pair easily. Resolve relative paths against the playlist directory, reject network URLs, validate exactly two entries, and provide actionable errors.

Because frontend persistence semantics differ between subsystem content and playlists, do not add M3U by bypassing or weakening save safety. Document its exact save naming and ownership.

## Video and controls

The Libretro API provides one video callback, so compose both engine frames into one software framebuffer.

Required layouts:

- Side by side — default.
- Top/bottom.
- Player 1 only.
- Player 2 only.

Native combined dimensions:

- GB/GBC side by side: `320x144`.
- GB/GBC top/bottom: `160x288`.
- GBA side by side: `480x160`.
- GBA top/bottom: `240x320`.

Use a well-supported pixel format and report geometry/aspect changes correctly. Do not stretch one machine differently from the other.

Input contract:

- Libretro port 0 controls machine 0 and its corresponding left/top display.
- Libretro port 1 controls machine 1 and its corresponding right/bottom display.
- Poll input once per frontend frame, then pass each port's state to the corresponding machine.
- Add `Swap players/screens`, default `OFF`, which swaps both video placement and controller association together.
- Publish correct controller descriptors for two standard RetroPads.

## Audio scope

Audio mixing is explicitly not a priority for the first working version. Do not spend significant time on a mixer.

For the MVP:

- Output player 1/machine 0 audio only.
- Keep timing correct and avoid buffer overruns or stalls.
- Document this limitation.
- Structure the interface so selectable or mixed audio can be added later without changing engine ownership.

Silence is acceptable temporarily during an early implementation milestone, but the final MVP should output machine 0 audio if the engine integration makes this reasonably possible.

## Persistent saves and RTC

Persistence is a first-class requirement, not a follow-up.

Required logical naming:

### Same ROM loaded twice

GB/GBC:

```text
<rom-stem>.srm
<rom-stem>.srm.2
```

GBA preferred naming:

```text
<rom-stem>.sav
<rom-stem>.sav.2
```

### Two different ROMs

GB/GBC:

```text
<first-rom-stem>.srm
<second-rom-stem>.srm
```

GBA preferred naming:

```text
<first-rom-stem>.sav
<second-rom-stem>.sav
```

RTC data must likewise remain independent, using documented names such as `.rtc` and `.rtc.2` where needed.

Before committing to extensions, verify actual RetroArch behavior for standard SaveRAM, custom subsystem memory IDs and the save directory. RetroArch conventionally uses `.srm`, including for mGBA, while standalone GBA emulators commonly use `.sav`. The required invariant is **two stable, discoverable, non-clobbering files**. If `.sav` cannot be implemented cleanly through normal Libretro persistence, use `.srm` as the canonical RetroArch format and provide a documented, non-destructive `.sav` import/export path. Do not silently create duplicate competing save authorities.

Persistence requirements:

- Query and respect the frontend save directory.
- Use Libretro-managed memory descriptors where they work correctly.
- If the second same-ROM save must be core-managed, write it atomically through a temporary file plus rename.
- Flush on clean unload and at a safe periodic/dirty-state boundary so crashes do not lose an entire session.
- Never overwrite or rename an existing user save without an explicit backup.
- Detect/import existing SameBoy/mGBA/RetroArch saves when unambiguous; copy rather than destructively move.
- Test different cartridge save types and sizes.
- Test RTC persistence separately from battery RAM.
- Document exact filename rules and migration behavior in `docs/saves.md`.

## Save states

A DualBoy savestate must represent the complete paired session, not only machine 0.

Create a versioned container containing at least:

- Magic and format version.
- Engine/platform identifier.
- State length for each machine.
- Both engine states.
- Link/hub state and any pending serial-transfer/scheduler state not already captured by engine serialization.
- Enough metadata to reject incompatible or corrupt states safely.

`retro_serialize_size()` must return a stable upper bound after content load, as required by Libretro rewind/runahead behavior. `retro_unserialize()` must be all-or-nothing: validate before mutating live state where practical. Test round trips during idle play and during active link communication.

Battery saves and save states are separate concepts. Loading a savestate must not accidentally overwrite persisted SaveRAM on disk until the emulated software subsequently changes it and normal persistence occurs.

## Core options and defaults

At minimum provide:

```text
dualboy_mode                 = dual | player1 | player2       # default dual
dualboy_layout               = side_by_side | top_bottom     # default side_by_side
dualboy_link                 = enabled | disabled             # default enabled
dualboy_swap_players         = disabled | enabled             # default disabled
dualboy_audio_source         = player1 | disabled             # default player1
```

Mark restart-required options correctly. Hide irrelevant options when possible. Engine-specific options should be namespaced and should not expose hundreds of upstream frontend options in the MVP.

## mGBA integration requirements

Start by reproducing and understanding PR #318 rather than blindly copying it. The inspected branch already created secondary `mCore` instances, attached GBA multiplayer SIO, mapped controller ports, composed frames and serialized secondary cores.

Adapt that work so:

- Dual mode is the normal default.
- Two different GBA ROMs can be supplied.
- Each core gets its own cartridge save and RTC state.
- The combined savestate includes all required link synchronization state.
- All ownership and cleanup paths work after failed partial loads.
- Upstream mGBA APIs are used where possible instead of private struct access.

Do not claim GB/GBC linking works through mGBA merely because mGBA can load GB ROMs. Use SameBoy for linked GB/GBC unless direct tests prove a complete mGBA path is superior.

## SameBoy integration requirements

Use SameBoy's existing dual Libretro implementation as proven reference behavior, but move engine orchestration behind DualBoy's adapter rather than retaining a second Libretro entrypoint.

Verify and preserve:

- Two independent `GB_gameboy_t` instances.
- Serial/link connection and deterministic stepping.
- One input port per instance.
- Independent cartridge RAM and RTC.
- GB/GBC model selection and compatibility.
- Proper teardown and repeated load/unload without leaks.

## Error handling and logging

Use the Libretro log interface when available and provide clear failures for:

- Unsupported or corrupt ROMs.
- Mixed GB/GBA pairs.
- Wrong number of subsystem/M3U entries.
- Engine initialization failure.
- Link initialization failure.
- Save directory or write failures.
- Savestate version/platform mismatch.

Do not continue in a partially initialized dual session after a machine or link setup failure. Cleanup must be idempotent.

## Testing and verification

Do not use or download commercial ROMs. Use legal public-domain/homebrew/test ROMs, or build minimal test ROMs from source where practical.

Create automated tests for:

- Header/platform detection.
- Single ROM duplicated into independent machines.
- Two distinct content entries.
- Mixed-family rejection.
- Controller-port separation.
- Pixel composition and dimensions for each layout.
- Screen/player swapping.
- Save filename derivation.
- SaveRAM independence and reload persistence.
- RTC independence where testable.
- Combined savestate encode/decode and corruption rejection.
- Repeated load/unload and failed partial-load cleanup.
- M3U parsing if implemented.

Run sanitizers where supported. At minimum build and test on Linux x86-64. Produce:

```text
dualboy_libretro.so
dualboy_libretro.info
```

Verify the `.so` exports the required Libretro symbols. If RetroArch is available, run a real smoke test and capture the exact command and result. If Steam Deck hardware is unavailable, state that clearly; do not claim Deck validation based only on a desktop build.

## Phased implementation order

Follow this order while continuing autonomously between phases:

1. Repository/bootstrap, licenses, pinned upstreams, reproducible build and minimal Libretro core load.
2. Engine adapter interface and platform detection.
3. SameBoy GB/GBC same-ROM dual mode: video, controllers, link, independent saves.
4. SameBoy two-ROM subsystem mode.
5. mGBA PR #318 integration for GBA same-ROM dual mode.
6. mGBA two-ROM mode and independent SaveRAM/RTC.
7. Combined/versioned savestates.
8. Optional M3U convenience loading.
9. Linux release build, core-info packaging, documentation and final validation.

At every phase, keep the tree buildable and commit cohesive changes. Do not hide failing tests or replace unavailable runtime tests with fabricated output.

## MVP acceptance criteria

The MVP is complete only when all of the following are backed by real test/build output:

- `dualboy_libretro.so` builds reproducibly on Linux x86-64.
- Loading one GB/GBC ROM creates two visible linked machines by default.
- Loading one GBA ROM creates two visible linked machines by default.
- Two controller ports independently control the corresponding machines.
- Side-by-side is the default; top/bottom and swap work.
- The two-ROM subsystem accepts two GB/GBC games and two GBA games.
- Mixed GB/GBA pairs fail safely and clearly.
- Same-ROM sessions persist two independent saves without clobbering.
- Different-ROM sessions persist one save under each ROM's name.
- Save reload is demonstrated for both engine paths.
- A combined save state round-trips both machines.
- Machine 0 audio works or, if still unavailable, the limitation is explicit and does not break emulation timing.
- `README.md` contains build, installation, RetroArch subsystem and Steam Deck instructions.
- `docs/status.md` accurately distinguishes verified functionality, known limitations and untested claims.

## Non-goals for the MVP

Do not spend MVP time on:

- Audio mixing or stereo separation between machines.
- More than two players, even though PR #318 has four-player code.
- Network link/netplay.
- Two independent physical monitor outputs; Libretro supplies one framebuffer.
- Mixed GB/GBC-to-GBA link sessions.
- Mobile, Windows, macOS or console releases.
- Elaborate shaders, bezels or UI overlays.
- RetroAchievements integration.

## Required final report

When the implementation run ends, report concisely:

1. Architecture implemented and important deviations from this handoff.
2. Upstream commits pinned.
3. Files/modules added or changed.
4. Exact build and test commands executed.
5. Real results, including artifact path and architecture.
6. Which acceptance criteria are verified.
7. Known failures, unverified hardware behavior and technical debt.
8. Current Git status and commit list.
9. The next smallest concrete task if the MVP is not complete.

Do not report success for functionality that was only code-reviewed or inferred. Build success, core-load success, emulation success, link success and save-persistence success are separate claims and must be reported separately.