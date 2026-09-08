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
       /       |       \
SameBoy      mGBA      melonDS + LocalMP/OpenGL
pair         pair      pair + 2 software workers + EGL worker
```

Libretro's callback ABI requires one process-global frontend context. Mutable
emulator state is not global: the active `dualboy_session` owns one pair, and
the pair owns two machines. Load failures and unloads unwind in reverse ownership
order; partial cleanup is idempotent.

## Content ingestion and identity

The detector validates cartridge headers before selecting an engine. A GB/GBC
image must contain the Nintendo logo/header region and consistent ROM-size
metadata. A GBA image must contain the fixed header byte and a valid header
checksum. An NDS image must contain the logo, valid logo and header CRC16 values,
and in-bounds nonempty ARM9 and ARM7 sections. UnitCode 0 is a native DS image;
UnitCode 2 is accepted because DSi-enhanced cartridges retain a DS-compatible
partition. UnitCode 3 is DSi-exclusive and is rejected. Extensions provide an
error hint and the advertised content filter but are not the trust boundary.

Normal loading receives one memory-loaded cartridge and gives both machines one
immutable owned content copy. The machines do not share mutable memory. The
`dualboylink` subsystem requires exactly two memory-loaded cartridge slots.
GB and GBC are one SameBoy family and may be paired; NDS, GBA, and GB/GBC
families cannot be mixed and are rejected before engine creation.

M3U is an implemented third load mode, not a subsystem alias. The playlist itself
must be supplied by local path and is capped at 64 KiB. Its parser accepts an
optional UTF-8 BOM, ignores blank and `#` comment lines, trims horizontal
whitespace, and requires exactly two entries. Relative entries are resolved
lexically from the playlist directory. URLs, network-style paths, backslash-rooted
paths, NULs, directory-only entries, and traversal above an absolute root are
rejected. Each referenced cartridge is capped at 512 MiB. The resolved cartridge
paths are the persistence identities; the M3U filename is not. NDS normal,
subsystem, and M3U loads use the same ownership rules as the older platforms;
normal loading duplicates one cartridge and never creates an empty Download Play
guest.

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

The melonDS adapter owns two distinct `melonDS::NDS` objects, two instance
contexts, one untouched upstream `melonDS::LocalMP`, and two persistent worker
threads. In software mode, each frontend frame captures both inputs, releases
both workers to call one `NDS::RunFrame()` concurrently, waits for both
completions, and then exposes their software framebuffers. Running linked
instances concurrently is required because `LocalMP` can wait for the peer.
Teardown wakes and joins both workers before destroying engine objects,
including partial-load paths.

The worker barrier has a ten-second soft deadline. Crossing it requests an
abort, removes both machines from `LocalMP`, waits for both workers to become
quiescent, and permanently poisons that pair; no failed call returns while a
worker can still access pair-owned memory. This preserves the synchronous engine
ownership contract, but it is not hard cancellation: if upstream
`NDS::RunFrame()` itself never returns, the frontend thread must still wait for
quiescence. Safely bounding that permanent-wedge case would require upstream
cancellation support or process isolation.

The platform shim derives instance IDs from per-machine userdata and exposes only
IDs 0 and 1 to `LocalMP`, leaving upstream's 16-instance capacity unchanged.
`MP_Begin` records a machine's request; the live link option registers it only
while enabled, and `MP_End` or disabling the option unregisters it without
reloading content. Stop, failure, reset, and teardown paths end registrations;
reset also clears requested membership and stale queue/read offsets before the
machines restart. The Libretro layer inhibits fast-forward only when both IDs are registered and
releases the override as soon as either leaves. This transport is wholly inside
the process: no WFC, LAN, Internet, or nested Libretro core is involved.

Each NDS is created with melonDS's free BIOS implementation and its own generated
128 KiB firmware copy. Persisted firmware is treated as untrusted: its exact
size, generated `MELN` identity, DS Lite console type, user-settings offset, and
all dynamic ranges used by melonDS are validated before bytes enter the live
engine. The two deterministic locally administered MAC addresses end in `00`
and `01`; invalid or duplicate persisted identities are repaired and checksummed
before reset. No Nintendo BIOS or external firmware dump is bundled or loaded.
The melonDS regular OpenGL renderer is enabled from the byte-clean pin, with the
hash-verified build-tree `GPU2D_OpenGL.cpp` lifetime adaptation documented
below; the JIT remains disabled. Software is the default. On Linux, the NDS
option can create one dynamically loaded, explicitly surfaceless EGL display on
a dedicated adapter worker. DualBoy creates two pbuffer-backed OpenGL 3.2 core
contexts on that display, one for each machine. The contexts have no share
group, so the two upstream
renderers have separate object-name and binding namespaces. Context creation is
serialized before either renderer is published, and every renderer install,
frame, reset, and teardown runs synchronously on the same context-owning worker.
No GL call or EGL transition occurs on Libretro's run thread. DualBoy does not
request or replace a Libretro frontend hardware context; it obtains an explicit
surfaceless EGL display and creates dedicated contexts on its worker. EGL 1.5
may return the same display handle to another process component for the same
platform/native/attribute tuple, so teardown destroys DualBoy's contexts and
surfaces and releases its worker thread but deliberately does not call
`eglTerminate` on that process-shared display. The dynamically loaded EGL
library is marked non-unloadable for the same reason and stays resident for the
rest of the process.

OpenGL mode disables `LocalMP` and runs the two machines sequentially by
switching between their private context slots. Software mode retains the two
persistent concurrent frame workers required by `LocalMP`. The option applies
live: selecting OpenGL first disables LocalMP, selecting LocalMP first restores
both software renderers, and both options may be off. Both Libretro policy and
the adapter enforce the exclusion, and transactional failures either restore a
complete software pair or retire the pair without destroying GL objects on the
wrong thread/context.

DualBoy reads the two layers of each upstream output texture into the existing
native XRGB buffers. The established CPU compositor, touch-reticle path, and
ordinary Libretro CPU video callback therefore remain common to both renderers.
Missing `libEGL.so.1`, explicit surfaceless-platform/config failure, or a driver
below OpenGL 3.2 leaves a valid software pair. The worker logs the GL vendor and
renderer and identifies known Mesa software rasterizers. Since two GL frames,
context switches, CPU readback, and CPU composition remain serialized, this
architecture does not by itself establish a speedup; physical Deck profiling is
still required. The ten-second soft deadline is checked after each synchronous
GL machine call, so a slow first call can prevent the second from running.
Because an in-progress upstream call cannot be interrupted, a call that never
returns remains uncancellable in either mode.

## Video, input, and audio

Adapters return native XRGB8888 frames: 160x144 for GB/GBC, 240x160 for GBA, and
256x384 for NDS. Each NDS frame is a fixed 256x192 top screen followed by its
256x192 bottom/touch screen. The compositor implements side-by-side, top/bottom,
Player 1 only, Player 2 only, and a presentation-only screen swap without
scaling. NDS dual geometry is therefore 512x384 or 256x768. Geometry changes are
reported before the next frame, and the maximum allocation is 512x768. Screen
swapping exchanges the two dual-mode display slots; it does not alter a
player-only selection or controller-to-machine routing.

Input is polled once per frontend frame and captured as two structured values
containing a RetroPad mask and optional touch coordinates. Live core options
select RetroArch ports 0 through 4 independently for each machine; defaults are
port 0 for machine 0 and port 1 for machine 1, and duplicate selections are
valid. DualBoy advertises five RetroPad source ports, but they feed exactly two
emulated machine inputs. It cannot enumerate or reorder physical controllers
behind those Libretro ports, which remains frontend/platform state, and a source
port beyond the frontend's configured maximum-user count cannot provide input.
During NDS sessions, successive pointer indices are inverse-mapped through the
active compositor geometry and accepted only inside a displayed bottom screen,
with one first-wins contact per DS, independently of controller selection. Each
machine's selected port supplies its right analog stick plus R3 or R2 stylus
fallback.
Right-stick movement draws a clipped black/white reticle over that machine's
composed bottom screen without modifying either engine frame. Cursor state is
machine-owned and reset when that machine changes source port, so a visible
cursor from an old controller cannot authorize R2 on a new one. R3 may reveal
and press a stationary aim point; R2 presses only while the cursor is already
visible and does not itself reveal or extend the cursor. A two-pixel activity
threshold filters stick noise, and the cursor hides after three seconds using
the optional Libretro performance clock or after 180 frames when that clock is
unavailable. Physical pointer input suppresses the corresponding analog cursor
for that frame. Machine 0 audio is
resampled/buffered as interleaved signed 16-bit stereo.
Machine 1 audio is drained without emission so it cannot stall timing. The
audio-disabled option still drains emulated audio.

GB/GBC/GBA sessions retain the existing 59.7275 Hz frontend timing. NDS reports
the pinned engine's 59.8260982880808 Hz frame rate; all engines emit 48 kHz
stereo from machine 0 only.

## Persistence ownership

The save manager makes SaveRAM and engine-relevant RTC/firmware regions
core-managed, then delegates only the SameBoy regions a Libretro frontend can
name unambiguously:

| Engine/load | Machine 0 | Machine 1 |
| --- | --- | --- |
| SameBoy normal, named content | frontend standard SaveRAM/RTC | core-managed collision suffix |
| SameBoy two-ROM subsystem, distinct named stems | frontend custom slot IDs | frontend custom slot IDs |
| SameBoy subsystem with colliding names | frontend custom slot IDs | core-managed collision suffix |
| SameBoy M3U | core-managed | core-managed |
| SameBoy pathless slot | core-managed | core-managed for that slot |
| mGBA, every load mode | core-managed | core-managed |
| melonDS, every load mode | core-managed SaveRAM and firmware | core-managed SaveRAM and firmware |

The canonical paths are based on the last-extension-stripped cartridge basename.
If the two complete SRAM paths collide, machine 1 uses `.srm.2`, `.rtc.2`, and
`.firmware.bin.2`; otherwise NDS writable firmware uses `.firmware.bin` beside
each cartridge identity. Core-managed regions acquire sorted, nonblocking
advisory locks at
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

For SameBoy and mGBA, `retro_serialize_size` is a stable conservative upper bound
after load. Version 1 uses a 112-byte little-endian header beginning with
`DUALBST\0`. It records
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

NDS deliberately leaves the engine state callbacks unset, so
`retro_serialize_size()` returns zero for an NDS session. melonDS machine states
do not include `LocalMP` queues and waits, and a transactional two-machine state
cannot currently restore or safely abandon that transport. Manual states,
rewind, and runahead are therefore unsupported for NDS rather than presented as
complete. This does not change linked state behavior for SameBoy or mGBA.

## Reproducible Linux build

Exact upstream revisions are Git submodule gitlinks documented in
[`THIRD_PARTY.md`](../THIRD_PARTY.md). The reference x86-64 build uses
`debian:bookworm-slim` at image digest
`sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171`
with the versioned package manifest under `tools/linux-x86_64`. Docker is
explicitly asked for `linux/amd64`, so an ARM development host cannot silently
produce the wrong architecture.

The melonDS build is pinned to an exact gitlink and configured with
`ENABLE_OGLRENDERER` on and `BUILD_QT_SDL`, `ENABLE_GDBSTUB`, `ENABLE_JIT`,
`ENABLE_LTO_RELEASE`, and `MELONDS_EMBED_BUILD_INFO` off. Upstream's `core`
archive and the otherwise-unselected, untouched `src/net/LocalMP.cpp` translation
unit are statically linked. A hash-verified build-tree adaptation of
`GPU2D_OpenGL.cpp` initializes and releases two CPU vertex arrays omitted by the
pinned destructor; the gitlink remains byte-clean and the generated source keeps
its upstream GPL header. The untouched generated GLAD source normally owned by
melonDS's desktop frontend is built privately and resolves functions through
the dedicated EGL contexts; all platform glue remains in DualBoy. A CMake
install also stages `LICENSE`, `NOTICE`, `THIRD_PARTY.md`, the optional RetroArch
menu-control fragment, and the selected dependency license texts under
`share/doc/dualboy`.

melonDS is GPL-3.0-or-later. Consequently the combined shared object and its
binary distribution are conveyed under GPLv3-compatible terms, while
DualBoy-authored files retain their MPL-2.0 notices. The exact dependency and
bundled-source license inventory is in [`THIRD_PARTY.md`](../THIRD_PARTY.md).
