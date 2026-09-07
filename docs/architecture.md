# Architecture

## Process boundary

DualBoy exports exactly one Libretro ABI and directly instantiates two emulator
objects inside the core. The frontend owns content, the paired session, layout,
input mapping, persistence paths, options, and the paired state container. Each
engine adapter exclusively owns its emulator objects and engine-specific link
scheduler. There is no nested or dynamically loaded Libretro core.

```text
RetroArch callbacks
        |
DualBoy Libretro ABI (one process-global callback context)
        |
content / session / options / saves / state / compositor
        |
dualboy_engine_ops
       / \
SameBoy   mGBA + PR #318 SIO lockstep
pair      pair
```

Libretro's callback ABI requires one process-global frontend context. Mutable
emulator state is not global: the active `dualboy_session` owns one pair, and
the pair owns two machines. Load failures and unloads unwind in reverse ownership
order; partial cleanup is idempotent.

## Content ingestion and identity

The detector validates cartridge headers before selecting an engine. A GB/GBC
image must contain the Nintendo logo/header region and consistent ROM-size
metadata. A GBA image must contain the fixed header byte and a valid header
checksum. Extensions provide an error hint and the advertised content filter but
are not the trust boundary.

Normal loading receives one memory-loaded cartridge and gives both machines one
immutable owned content copy. The machines do not share mutable memory. The
`dualboylink` subsystem requires exactly two memory-loaded cartridge slots.
GB and GBC are one SameBoy family and may be paired; a GB/GBC and GBA mixture is
rejected before engine creation.

M3U is an implemented third load mode, not a subsystem alias. The playlist itself
must be supplied by local path and is capped at 64 KiB. Its parser accepts an
optional UTF-8 BOM, ignores blank and `#` comment lines, trims horizontal
whitespace, and requires exactly two entries. Relative entries are resolved
lexically from the playlist directory. URLs, network-style paths, backslash-rooted
paths, NULs, directory-only entries, and traversal above an absolute root are
rejected. Each referenced cartridge is capped at 64 MiB. The resolved cartridge
paths are the persistence identities; the M3U filename is not.

When a frontend provides cartridge bytes without a path, the core derives a
stable identity `dualboy-<16-digit FNV-1a hash>.<family extension>`. These
pathless slots are always core-managed for persistence.

## Pair execution

The SameBoy adapter owns two `GB_gameboy_t` values and separate video, audio,
battery, RTC, and serialized state. Symmetric serial-bit and infrared callbacks
connect the machines. Frame production advances the machine that is behind until
both have completed a frame. Open SameBoy boot ROM data is embedded.

The mGBA adapter owns two `mCore` values and separate video, audio, save, RTC,
and serialized state. Each GBA SIO peripheral attaches to an adapted PR #318
`GBASIOLockstepDriver`. One cooperative scheduler advances unblocked cores
until both frame counters change, with a finite watchdog to turn a deadlock into
a load/run error. With link disabled, both cores run at frame granularity.
Changing the GBA link option resets both machines before the new topology is used.
A valid `gba_bios.bin` in the frontend's system directory is used when present;
an invalid file is ignored.

The adapted scheduler treats its 32-bit emulated timestamps as modular counters,
using half-range ordering for every comparison and difference. Link-state
deserialization validates the complete scheduler payload for both players before
mutating either driver. During per-machine mGBA state restore, SIO mode callbacks
remain observational until both machine clocks are restored; the paired link
payload then restores queues, barriers, and coordinator time together.

## Video, input, and audio

Adapters return native XRGB8888 frames: 160x144 for GB/GBC and 240x160 for GBA.
The compositor implements side-by-side, top/bottom, Player 1 only, Player 2 only,
and a coupled screen/controller swap without scaling. Geometry changes are
reported before the next frame.

Input is polled once per frontend frame and captured as two RetroPad masks.
RetroArch port 0 maps to machine 0 and port 1 to machine 1 before an optional
coupled swap. Machine 0 audio is resampled/buffered as interleaved signed 16-bit
stereo. Machine 1 audio is drained without emission so it cannot stall timing.
The audio-disabled option still drains emulated audio.

## Persistence ownership

The save manager starts with every region core-managed, then delegates only the
SameBoy regions a Libretro frontend can name unambiguously:

| Engine/load | Machine 0 | Machine 1 |
| --- | --- | --- |
| SameBoy normal, named content | frontend standard SaveRAM/RTC | core-managed collision suffix |
| SameBoy two-ROM subsystem, distinct named stems | frontend custom slot IDs | frontend custom slot IDs |
| SameBoy subsystem with colliding names | frontend custom slot IDs | core-managed collision suffix |
| SameBoy M3U | core-managed | core-managed |
| SameBoy pathless slot | core-managed | core-managed for that slot |
| mGBA, every load mode | core-managed | core-managed |

The canonical paths are based on the last-extension-stripped cartridge basename.
If the two complete SRAM paths collide, machine 1 uses `.srm.2` and
`.rtc.2`. Core-managed regions acquire sorted, nonblocking advisory locks at
`<region path>.dualboy.lock`. Contention makes the core-managed regions
read-only for that session; other lock errors reject loading. Lock files are
persistent coordination metadata.

Core-managed saves are polled every 300 frontend frames and forced through the
same change detector during unload/deinit. Writes use a same-directory temporary
file, file `fsync`, and atomic rename over the canonical path, followed by a
best-effort parent-directory `fsync`. A pre-existing canonical file always
wins over an optional GBA `.sav` import, and imports copy rather than rename or
delete the source. See [save ownership and formats](saves.md).

## mGBA save-memory model

mGBA save technology may be unknown at load and may resolve only after guest
access. Each machine therefore presents the adapter and save manager with one
stable 128 KiB SaveRAM shadow: its pointer and capacity do not change when mGBA
autodetects EEPROM, SRAM, or flash.

Disk length is a separate dynamic extent. While the type is
`GBA_SAVEDATA_AUTODETECT`, no save length is considered safe and the manager
does not create or expand a canonical file. Once an override or guest access
resolves the type, the adapter reports exactly 0, 512 B, 8 KiB, 32 KiB, 64 KiB,
or 128 KiB. RTC is likewise unknown before the first reset and then resolves to
0 or the 16-byte mGBA RTC record. The manager retains a full-capacity baseline
while an extent is unknown, so the guest write that resolves detection is not
lost even if mGBA's delayed dirty callback has not fired.

Short existing files load into an `0xff`-filled shadow without being expanded
merely because the detected device is larger. A non-`0xff` tail beyond the
detected extent is preserved on a later write to avoid destructive truncation;
an all-`0xff` oversized tail may normalize to the detected extent.

## Paired save states

`retro_serialize_size` is a stable conservative upper bound after load. Version
1 uses a 112-byte little-endian header beginning with `DUALBST\0`. It records
the engine family, both platform types, link-enabled state, ROM lengths and CRCs,
both machine payload lengths and CRCs, and the link-scheduler payload length and
CRC, followed by all three payloads.

Unserialize validates identifiers, arithmetic, capacities, ROM identity, and
checksums before mutation. DualBoy snapshots the live pair, restores both
machines and link state, and rolls the whole pair back if any restore step fails.
If rollback itself fails, the session is discarded and frontend shutdown is
requested rather than continuing with a split pair.

SaveRAM and RTC are copied aside and restored around state loading. Consequently,
loading a save state changes both machines' volatile state atomically but does not
roll either battery region backward or manufacture disk-save progress. RetroArch
owns the save-state file separately from the battery-save manager.

## Reproducible Linux build

Exact upstream revisions are Git submodule gitlinks documented in
[`THIRD_PARTY.md`](../THIRD_PARTY.md). The reference x86-64 build uses
`debian:bookworm-slim` at image digest
`sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171`
with the versioned package manifest under `tools/linux-x86_64`. Docker is
explicitly asked for `linux/amd64`, so an ARM development host cannot silently
produce the wrong architecture.
