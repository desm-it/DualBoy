# Nintendo DS SaveRAM corruption investigation

Investigate and fix Nintendo DS SaveRAM corruption in DualBoy.

## Working rules

- Read `AGENTS.md` and `docs/saves.md` completely before changing anything.
- Inspect the current working tree first and preserve all in-progress work.
- Do not modify `third_party/melonDS`.
- Follow test-driven development: reproduce the failure with a regression test before changing production code.
- Do not use or commit commercial ROMs, user saves, BIOS files, firmware, credentials, or other private artifacts.

## Observed failure

DualBoy creates a Nintendo DS save file for Pokémon Diamond, but reopening the same ROM presents **New Game**.

The live Deck save was:

```text
/home/deck/.var/app/org.libretro.RetroArch/config/retroarch/saves/Pokémon Diamond Version.srm
```

Observed metadata:

```text
size:     524288 bytes
mode:     0600
owner:    deck:deck
SHA-256:  b8bc232fc0d4a3a414030ddd88f8b1036561240ca1cffc0b932969acb55c53cd
```

The file was not empty or uniformly erased:

```text
non-FF bytes: 5015
two 256-KiB partitions differed: yes
```

There was no machine-two `Pokémon Diamond Version.srm.2`; only its ownership lock existed. Both firmware files existed. No save-initialization, periodic-flush, or unload-flush error was found in the logs.

## Integrity analysis

The file was checked using the Pokémon Diamond/Pearl Generation IV block layout and the same CRC16-CCITT algorithm used by current PKHeX.

Relevant invalid checksums:

```text
Partition 0 general:
  stored:     0xe0cf
  calculated: 0xbffa

Partition 1 general:
  stored:     0xc16d
  calculated: 0x9721

Partition 1 storage:
  stored:     0x7eb3
  calculated: 0xbb41
```

Neither redundant partition had a valid general block, explaining why the game rejected the file even though DualBoy had written a 512-KiB `.srm`.

## Current persistence flow

Inspect at least:

- `src/engines/melonds/nds_adapter.cpp`
- `src/frontend/save_manager.c`
- `src/frontend/save_manager.h`
- `src/libretro/libretro_core.c`
- `tests/melonds_adapter_test.cpp`
- `docs/saves.md`
- Upstream melonDS cartridge SaveRAM behavior under `third_party/melonDS/src/NDSCart/`

The intended load path appears to be:

1. `dualboy_save_manager_init()` derives and loads the per-machine files.
2. SaveRAM is loaded into each machine's `save_shadow`.
3. The engine's `persistent_memory_loaded` callback runs.
4. `PersistentMemoryLoaded()` invokes `Reset()`.
5. `Reset()` installs the shadow using `NDS::SetNDSSave()`.
6. Core-managed regions are flushed atomically every 300 frames and on clean unload.

## Primary suspect

In `nds_adapter.cpp`, `RunFrame()` currently compares melonDS's live cartridge SaveRAM against `save_shadow` before every frame. If they differ, it copies `save_shadow` back into melonDS:

```cpp
if (!machine.save_shadow.empty() &&
    machine.nds->GetNDSSave() != nullptr &&
    (... ||
     std::memcmp(machine.nds->GetNDSSave(),
                 machine.save_shadow.data(),
                 machine.save_shadow.size()) != 0)) {
    ...
    machine.nds->SetNDSSave(machine.save_shadow.data(), ...);
}
```

Upstream melonDS modifies its cartridge SRAM and calls `Platform::WriteNDSSave()` after a write operation or range completes. If live SRAM legitimately differs from the shadow while an emulated cartridge write is in progress, the pre-frame reconciliation may restore stale bytes before melonDS reports the completed write.

This is a strong hypothesis, not yet a proven root cause. Prove or disprove it with a deterministic test before changing it.

## Existing test gap

`TestPersistenceAcrossDestruction()` currently injects individual bytes through a debug helper, flushes them, and confirms they reload. It proves basic byte persistence and path separation, but it does not reproduce real cartridge behavior.

Add coverage that exercises the relevant ordering:

- melonDS changes its live SRAM before invoking `WriteNDSSave()`.
- The change remains pending across at least one DualBoy frame boundary.
- DualBoy must not overwrite valid in-progress engine state with stale shadow data.
- Once the write callback completes, the updated data must reach `save_shadow`.
- Periodic and forced flushes must write the complete correct extent.
- Destroying and recreating the pair must restore identical bytes into both adapter memory and melonDS's live SaveRAM.
- Machine 0 and machine 1 must remain independent.
- A missing `.srm.2` must not affect machine 0.
- Existing firmware and write-ownership behavior must remain intact.

If practical with legal source-generated test content, create an integration test that performs a realistic multi-range or multi-frame SaveRAM operation and validates a transaction/checksum marker after reload. Do not hard-code copyrighted Pokémon data.

## Investigation requirements

1. Trace every writer and reader of:
   - `MachineContext::save_shadow`
   - `NDS::GetNDSSave()`
   - `NDS::SetNDSSave()`
   - `Platform::WriteNDSSave()`
   - `save_dirty`
   - Save-manager tracking baselines

2. Establish synchronization:
   - Which thread modifies live SRAM?
   - Which thread executes `WriteNDSSave()`?
   - When can the main Libretro thread read or copy SaveRAM?
   - Are the frame-completion barriers sufficient?
   - Can save-manager hashing or flushing race a worker?

3. Determine why the per-frame shadow-to-engine reconciliation exists:
   - If it supports frontend writes, verify whether melonDS SaveRAM is actually exposed as frontend-managed memory.
   - `choose_ownership()` currently marks SaveRAM core-managed.
   - Do not preserve unnecessary reconciliation merely because it exists.
   - If external mutation must be supported, introduce explicit ownership/versioning rather than treating every byte difference as proof that the shadow is authoritative.

4. Review failure behavior:
   - Clean RetroArch unload
   - Periodic flush
   - Abrupt frontend termination
   - Dirty state while an emulated write is incomplete
   - Existing invalid or wrong-sized save files

5. Do not silently repair arbitrary game saves unless there is a format-independent, engine-correct reason. DualBoy should preserve emulator state correctly; it should not contain Pokémon-specific save logic.

## Acceptance criteria

- A regression test fails on the current implementation for the demonstrated ordering/corruption mechanism.
- The production fix makes that test pass.
- Live melonDS SRAM is never replaced by stale shadow data during normal emulation.
- Completed melonDS SaveRAM callbacks update the persistent representation correctly.
- Periodic and forced flushes preserve complete, byte-identical SaveRAM.
- SaveRAM reload survives pair destruction and recreation.
- Both NDS instances retain independent saves and firmware.
- GB/GBC/GBA persistence behavior remains unchanged.
- No changes are made under `third_party/`.
- No copyrighted ROM, BIOS, firmware, or user save is committed.
- `git diff --check` passes.
- The strict Linux x86-64 test suite passes.
- The native ASan suite passes.
- The Libretro symbol check passes.

## Required validation

Run the repository-prescribed commands from `AGENTS.md`, including:

```sh
make test-linux-x86_64
make asan-linux-native
make symbols-linux-x86_64
git diff --check
```

Do not claim the Steam Deck or a commercial game is fixed unless that exact runtime validation is subsequently performed. In the final report, clearly separate:

- Proven root cause
- Implemented fix
- Regression coverage
- Local/container validation
- Remaining Deck validation
- Any pre-existing unrelated failures

## Resolution record (2026-09-08)

The stale-shadow hypothesis was proven with the pinned upstream `CartRetail`
SPI implementation and source-generated test content. A real multi-byte EEPROM
program operation changed melonDS's live SRAM, deliberately remained selected
across one DualBoy frame, and had not yet invoked `WriteNDSSave`. Before the
production fix, `melonds_adapter_unit` then failed its assertion that those live
bytes survived the frame boundary: `RunFrame` had copied the older
`save_shadow` over them. Releasing the transaction afterward could therefore
publish stale or mixed bytes into the shadow, which the save manager would
correctly hash and atomically persist without reporting an I/O error.

NDS SaveRAM is core-managed and unavailable through Libretro memory IDs, so
there is no frontend writer that requires per-frame shadow reconciliation. The
adapter now treats melonDS's callback as the normal-emulation commit boundary:
explicit load/reset still installs the shadow, while ordinary frames only
validate that the live extent and pointer remain stable. Completed callbacks
copy live ranges into the complete disk-facing shadow. The callback also mirrors
upstream range semantics for end-of-device wrapping and the zero-masked length
produced by an exactly full-device `CartRetail` transaction; invalid offsets are
ignored.

The final deterministic coverage exercises the pre-fix ordering, wrapped and
full-device callbacks, exact 300-tick periodic flushing, forced flushing, full
8 KiB disk-image comparison, an absent `.srm.2`, distinct checksummed records
for both machines, independent firmware, and two pair destruction/recreation
cycles with byte-identical shadow and live SRAM. Worker completion under
`frame_mutex` is the happens-before/quiescence boundary before main-thread save
hashing, so no save-manager/worker race was found.

Final automated results on this host were 10/10 for the strict native Linux
suite (6.40 seconds), 10/10 for `make test-linux-x86_64` (61.64 seconds), and
10/10 for `make asan-linux-native` (120.51 seconds). The symbol allowlist passed.
`git diff --check` passed. The sanitizer log retained 22 recoverable diagnostics
in untouched upstream SameBoy/melonDS code and contained no AddressSanitizer
finding. Direct macOS CMake/CTest commands remain unavailable because the host
has neither executable installed; the pinned Linux containers completed those
phases instead.

No NDS content, commercial game, existing user save, real RetroArch process, or
physical Steam Deck was used for this resolution. Source inspection shows that
the observed 512 KiB FLASH extent shares the proven `CartRetail`
write-before-release ordering, but the reported game and Deck still require
runtime validation. Existing corrupt saves are not repaired by this change.
