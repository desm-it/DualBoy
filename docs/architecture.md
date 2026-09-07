# Architecture

## Process boundary

DualBoy exports exactly one Libretro ABI and directly instantiates two engine
objects inside the core. The frontend owns the session, immutable content copies,
layout framebuffer, input mapping, save paths, and paired state container. An
engine adapter owns every emulator object and engine-specific link scheduler.
Neither engine exposes a second Libretro entrypoint to the frontend.

```text
RetroArch callbacks
        |
DualBoy Libretro ABI (one global ABI context required by Libretro)
        |
session/content/options/persistence/state/compositor
        |
dualboy_engine_ops
       / \
SameBoy   mGBA + PR #318 SIO lockstep
pair      pair
```

Libretro's callback ABI requires one process-global frontend context. Mutable
emulator state is not global: it is owned by the active `dualboy_session`, then by
one adapter pair. Cleanup unwinds in reverse ownership order and is safe after a
partial load.

## Content and engine selection

The detector first validates cartridge headers and only uses an extension as an
error hint. A GB/GBC cartridge needs the Nintendo logo/header region and sane ROM
size metadata; a GBA cartridge needs its fixed header byte and header checksum.
Single content is duplicated as immutable bytes. The subsystem passes exactly two
required slots. Both slots must resolve to the same engine family; GB and GBC are
one family, while any GB/GBA mix is rejected before engine creation.

An M3U loader, after subsystem support is complete, may resolve exactly two local
paths relative to the playlist. It rejects URLs and never changes persistence
ownership to make a playlist load succeed.

## Pair execution

SameBoy owns two `GB_gameboy_t` values, separate pixel/audio/battery/RTC memory,
and symmetric serial bit callbacks. It advances the machine that is behind until
both have produced a frame, matching SameBoy's proven Libretro implementation.

mGBA owns two independent `mCore` values and two video/save/audio resources. Each
GBA SIO peripheral attaches to a PR #318 `GBASIOLockstepDriver`. One cooperative
scheduler advances unblocked cores until both frame counters change, guarded by a
finite watchdog. Link disablement detaches both peripherals and uses frame-level
stepping.

## Video, input, and audio

Adapters return native XRGB8888 frames: 160x144 for GB/GBC and 240x160 for GBA.
The common compositor implements side-by-side, top/bottom, player-only, and paired
player/screen swap without scaling. Geometry changes are reported before the next
frame. Input is polled once and captured as two RetroPad masks, then mapped by the
active adapter. Machine 0 audio is buffered as interleaved signed 16-bit stereo;
machine 1 audio is consumed or disabled without stalling its timing.

## Persistence

The session queries `RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY`. For ordinary content,
the frontend-managed standard SaveRAM/RTC belongs to machine 0; machine 1 is
core-managed at the collision-safe `.srm.2`/`.rtc.2` names. The two-ROM subsystem
publishes distinct custom memory IDs for each slot, allowing each content stem to
own its own files. If both subsystem paths name the same content, machine 1's
custom regions are suppressed and the collision-safe core-managed files are used.
M3U sessions are core-managed because the playlist path is not either cartridge's
identity. Core-managed writes use same-directory temporary files, `fsync`, and
atomic rename, and never rename or delete a pre-existing user save.

SameBoy exposes cartridge RAM and RTC separately even though its standalone
battery format can combine them. mGBA uses one independent fixed-capacity backing
VFile per core; persisted length follows the detected save technology. RetroArch's
canonical extension is `.srm`, including GBA. `.sav` is an explicit copy-based
import/export compatibility path, never a second live authority.

## Paired save states

The stable post-load size is a conservative upper bound. A little-endian container
has a magic, version, engine family, flags, per-machine lengths/checksums, link
length/checksum, and reserved bytes, followed by both engine states and link state.
Decode validates all arithmetic, identifiers, sizes, and checksums before mutation.
Where an engine cannot transactionally validate a state, DualBoy first snapshots
both live machines and rolls both back if either restore fails. Battery persistence
is not marked dirty merely because a save state contains SaveRAM.

## Reproducible Linux build

Upstream source revisions are gitlinks recorded by the parent commit. The reference
Linux x86-64 build uses `debian:bookworm-slim` pinned to image digest
`sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171`
with an explicitly versioned tool manifest in `tools/linux-x86_64`. Docker is asked
for `linux/amd64`, so the artifact is x86-64 even when the developer host is ARM.
