# Save ownership and formats

DualBoy has two different persistence paths:

- **Frontend-managed memory** is exposed through Libretro memory IDs. RetroArch
  decides when and how to write it.
- **Core-managed memory** is loaded and atomically written by DualBoy. It is not
  exposed by `retro_get_memory_data`, preventing RetroArch and DualBoy from
  treating the same bytes as two live authorities.

SaveRAM and RTC are independent for both machines even when one ROM image is
duplicated. All battery paths use RetroArch's configured Save Files directory. If
the frontend supplies no save directory, DualBoy falls back to its System/BIOS
directory and logs a warning.

## Canonical names

DualBoy removes the last ordinary extension from each cartridge's basename, then
adds a persistence suffix. Directory names are not included in the stem. Examples:

| Cartridge identity | SaveRAM | RTC |
| --- | --- | --- |
| `/games/red.gb` | `<save dir>/red.srm` | `<save dir>/red.rtc` |
| `/games/title.gba` | `<save dir>/title.srm` | `<save dir>/title.rtc` |

GBA's canonical DualBoy and RetroArch extension is **`.srm`**, not `.sav`.

When the complete machine-0 and machine-1 `.srm` paths would be identical,
machine 1 receives a collision suffix:

| Machine | SaveRAM | RTC |
| --- | --- | --- |
| 0 / Player 1 | `game.srm` | `game.rtc` |
| 1 / Player 2 | `game.srm.2` | `game.rtc.2` |

This rule covers a normal same-ROM load and two cartridges from different
directories that have the same basename. It prevents the two mutable machines
from ever sharing one battery file.

For memory-loaded content without a usable path, DualBoy hashes the immutable ROM
bytes with 64-bit FNV-1a and creates a stable synthetic identity:

```text
dualboy-0123456789abcdef.gb
dualboy-0123456789abcdef.gbc
dualboy-0123456789abcdef.gba
```

The real hexadecimal value depends on the complete content bytes. A duplicated
pathless cartridge therefore uses the corresponding
`dualboy-<hash>.srm`/`.rtc` and
`dualboy-<hash>.srm.2`/`.rtc.2` files. Pathless regions are always
core-managed.

## Ownership by load mode

### SameBoy normal load

With a named `.gb` or `.gbc`, machine 0 is frontend-managed through the
standard `RETRO_MEMORY_SAVE_RAM` and `RETRO_MEMORY_RTC` IDs. RetroArch owns
its ordinary `<stem>.srm` and `<stem>.rtc` persistence. Machine 1 is
core-managed at `<stem>.srm.2` and `<stem>.rtc.2`.

If the content has no path, both machines are core-managed under the hash-derived
names above.

### SameBoy two-ROM subsystem

The `dualboylink` subsystem advertises these per-slot memory descriptors:

| Slot | SaveRAM descriptor | RTC descriptor |
| --- | --- | --- |
| Player 1 | extension `srm`, ID `0x100` | extension `rtc`, ID `0x101` |
| Player 2 | extension `srm`, ID `0x200` | extension `rtc`, ID `0x201` |

When both cartridges have named, distinct basenames, both slots are
frontend-managed through those custom IDs. The intended names are each slot's
`<stem>.srm` and `<stem>.rtc`.

When the derived names collide, machine 0 remains frontend-managed and machine 1
becomes core-managed at `.srm.2`/`.rtc.2`. A pathless slot is likewise
core-managed.

The automated harness confirms that the custom subsystem IDs expose distinct
pointers. It has not yet shown that a real RetroArch frontend writes both custom
RTC IDs to disk. Until that is verified, use M3U if independent core-owned RTC
files are important.

### SameBoy M3U

Both machines are core-managed. Each resolved cartridge path supplies its own
stem; colliding stems receive the normal machine-1 suffix. The playlist filename
does not become a save identity.

### mGBA

Both machines' SaveRAM and RTC are core-managed in normal, subsystem, and M3U
loads. Standard and custom Libretro memory IDs return no mGBA battery region, so
RetroArch cannot simultaneously write the same bytes. Paths follow the canonical
and collision rules above.

## Exact persistent extents

### GBA SaveRAM

The mGBA adapter keeps a stable 128 KiB shadow for each machine so its address
cannot change after the frontend has loaded content. The amount written to disk
is a separate detected extent:

| mGBA savedata type | Persistent bytes |
| --- | ---: |
| forced none | 0 |
| EEPROM512 | 512 B |
| EEPROM | 8 KiB (8,192 B) |
| SRAM | 32 KiB (32,768 B) |
| SRAM512 | 64 KiB (65,536 B) |
| FLASH512 | 64 KiB (65,536 B) |
| FLASH1M | 128 KiB (131,072 B) |

With mGBA autodetection, the extent is unknown until a cartridge access resolves
the save technology. DualBoy loads an existing file into an `0xff`-filled
128 KiB shadow but does not create or expand a file while the extent is unknown.
After detection, only the exact extent is normally written.

A short existing file is not expanded merely because the detected capacity is
larger. If an existing file contains non-`0xff` bytes beyond the detected
extent, DualBoy preserves that tail on later writes rather than truncating data it
cannot safely classify. An oversized tail containing only `0xff` may normalize
to the detected size.

### RTC and GB/GBC SaveRAM

mGBA's separate RTC record is exactly 16 bytes when cartridge hardware enables
RTC, otherwise 0 bytes. Its extent is unknown until the first reset applies
hardware overrides.

SameBoy exposes the battery-backed cartridge RAM size selected by the cartridge
controller/header rather than a fixed maximum. The pinned engine can select 256 B
(MBC7), 512 B (MBC2), the standard header sizes 2 KiB, 8 KiB, 32 KiB, 64 KiB,
and 128 KiB, or TPP1 sizes from 8 KiB through 2 MiB in powers of two (including
16 KiB, 256 KiB, 512 KiB, and 1 MiB). The adapter's separate SameBoy RTC region
is the pinned core's 32-byte RTC state section. A cartridge without
battery-backed RAM or RTC exposes a zero-length region.

## Nondestructive GBA `.sav` import

For each absent canonical GBA SaveRAM file, DualBoy looks for one legacy import
candidate:

| Canonical path | Legacy candidate |
| --- | --- |
| `title.srm` | `title.sav` |
| `title.srm.2` | `title.sav.2` |

The precedence and safety rules are:

1. If a regular canonical `.srm` exists, it is authoritative and the
   corresponding `.sav` is ignored.
2. If the canonical file is absent and a regular `.sav` exists, its bytes are
   loaded. A write-owning session copies them to the canonical path.
3. The original `.sav` is never renamed, overwritten, or deleted. Import does
   not create a permanently synchronized second authority.
4. Under save-lock contention, the legacy data may be loaded for the session but
   no canonical copy is created because the session is read-only.

DualBoy does not maintain an automatic `.sav` export mirror. After import, the
canonical `.srm` and optional `.rtc` files are the only live authorities.

Standalone mGBA saves may append its 16-byte RTC record to SaveRAM. DualBoy splits
that layout into canonical `.srm` and `.rtc` files only when it is
unambiguous:

- after save-type detection, total legacy length must equal the exact detected
  SaveRAM extent plus 16 bytes; or
- while autodetection is unresolved, subtracting 16 bytes must leave one of the
  supported GBA extents: 512 B, 8 KiB, 32 KiB, 64 KiB, or 128 KiB.

The first part becomes `.srm`, the final 16 bytes become `.rtc`, and the
source `.sav` remains untouched. Unsupported or oversized layouts are rejected
rather than guessed.

## Writes, flushing, and locks

Core-managed files are checked every 300 frontend frames, roughly five seconds at
DualBoy's 59.7275 Hz rate, and are force-checked before normal unload/deinit. A
hash suppresses writes when the tracked bytes did not change. For unresolved
mGBA extents, a full-capacity baseline is retained so the first guest write is
recognized when detection becomes known.

Each changed region is written to a uniquely created temporary file in the same
directory. DualBoy writes all bytes, calls `fsync` on the temporary file,
closes it, atomically renames it over the canonical destination, and makes a
best-effort `fsync` of the parent directory. The old canonical file is never
removed before rename, and a failed write removes its temporary file.

Before reading any core-managed region, DualBoy attempts sorted, exclusive,
nonblocking advisory locks. Each lock lives at:

```text
<exact save path>.dualboy.lock
```

For example, `game.srm.2` uses `game.srm.2.dualboy.lock`. Separate SaveRAM
and RTC paths have separate locks. The zero-length possibility does not remove
the corresponding coordination path. Lock files intentionally remain on disk
after descriptors are closed; they are small metadata files, not battery data,
and should not be synchronized as game saves.

If another process owns any required lock, DualBoy releases locks already
acquired and runs all of that session's core-managed regions read-only. Existing
saves are still loaded, but periodic and forced flushes do not write. Busy-lock
contention is a safe warning, not permission to race. Other lock I/O failures
(for example, an unwritable directory or a lock path that is a directory) reject
the content load. Frontend-managed SameBoy regions are outside DualBoy's lock
set.

## Save states are separate

RetroArch manages save-state files in its configured Save States directory.
DualBoy's state blob contains both machine states and the link scheduler under one
versioned, checksummed header. State loading validates the complete container and
rolls both machines back together on failure.

Battery SaveRAM and RTC are deliberately copied around state loading. Loading an
old save state therefore does **not** rewind either battery file, mark battery
memory dirty merely because it appeared in a state, or replace a canonical save.
Back up and synchronize battery files and save states as distinct data classes.
