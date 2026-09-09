# DualBoy

DualBoy is an experimental Libretro core that combines SameBoy, mGBA, and
melonDS so two local handheld systems can run together in one split-screen
RetroArch session.

Validation is currently Steam Deck-focused; use the links below to jump directly
to the relevant sections.

## Index

- [Build](#build)
- [Install in RetroArch](#install-in-retroarch)
- [Load content](#load-content)
- [Players, screens, and audio](#players-screens-and-audio)
- [Core options](#core-options)
- [Saves and save states](#saves-and-save-states)
- [Steam Deck](#steam-deck)
- [Scope and licenses](#scope-and-licenses)

Current behavior is intentionally focused on local two-player flows (duplicate ROM
mode, the `dualboylink` subsystem, or two-entry M3U). It includes no nested core
loading and no official claims beyond local local-multiplayer features.

The project is validated with source-generated ROM fixtures and does not include
commercial ROMs, BIOS files, firmware, user saves, or private artifacts.

See [implementation status](docs/status.md) for precise current evidence and
known limitations.

## Build

Initialize the pinned upstream revisions before any build:

```sh
git submodule update --init --recursive
```

The reference build is Linux x86-64 in the repository's digest-pinned Debian
container:

```sh
make linux-x86_64
make test-linux-x86_64
make symbols-linux-x86_64
```

The resulting core is `build-linux-x86_64/dualboy_libretro.so`. Docker is asked
for `linux/amd64`, so this is also the build path intended for an x86-64 Steam
Deck when developing on another architecture.

For ASan+UBSan, use `make asan-linux-native` when the Docker host is ARM. Use
`make asan-linux-x86_64` on a native x86-64 host; the ASan virtual-address-space
setup is not reliable when the x86-64 container itself is emulated.

A host build is useful for development but is not the reference Linux
validation:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

To stage an install tree:

```sh
cmake --install build-linux-x86_64 --prefix "$PWD/stage"
```

This installs the core below `lib/libretro`, its metadata below
`share/libretro/info`, and its notices, component license texts, and optional
RetroArch menu-control fragment below `share/doc/dualboy` in the selected
prefix.

## Install in RetroArch

RetroArch directory locations are configurable. Check **Settings > Directory**
for the active Core, Core Info, System/BIOS, Save Files, and Save States paths
rather than assuming a platform default.

1. Copy `dualboy_libretro.so` into the Core directory.
2. Copy `dualboy_libretro.info` into a writable Core Info directory.
3. Restart RetroArch or refresh its core information files, then confirm that
   **DualBoy** appears in **Information > Core Information**.
4. Optionally put a valid `gba_bios.bin` in the configured System/BIOS directory.
   GBA can run without it; an invalid image is ignored. SameBoy's open boot ROMs
   are embedded. NDS uses melonDS's built-in free BIOS and generated firmware;
   DualBoy does not read or ship Nintendo DS BIOS or firmware dumps.

If RetroArch's online updater owns a read-only info directory, select a writable
Core Info path or install the `.info` file using the packaging mechanism for
that RetroArch distribution.

## Load content

DualBoy accepts `.gb`, `.gbc`, `.gba`, `.nds`, and `.m3u`. Cartridge headers,
not filename extensions alone, select the engine. Both cartridges must use the
same engine family: GB and GBC may be paired, while NDS, GBA, and GB/GBC cannot
be mixed with one another.

### Normal load: duplicate one cartridge

Use RetroArch's normal **Load Content** flow and select one cartridge. DualBoy
shares the immutable ROM bytes but creates two independent machines, including
separate SaveRAM, applicable RTC or firmware, video, audio, and runtime state.

The equivalent command-line form is, for example:

```sh
retroarch -L /path/to/dualboy_libretro.so /path/to/game.gb
```

Selecting one `.nds` creates two independent DS consoles from the same immutable
cartridge bytes. It does not create a cartridge-less Download Play guest.

### Subsystem: two cartridges

In RetroArch, choose **Load Subsystem > DualBoy two-player link**, add Player 1
and Player 2 content in that order, then start the subsystem. Its identifier is
`dualboylink`; a command-line launch is:

```sh
retroarch -L /path/to/dualboy_libretro.so --subsystem dualboylink \
  /path/to/player1.gb /path/to/player2.gbc
```

### M3U: two explicit local paths

An M3U must contain exactly two nonblank, non-comment entries. Relative entries
are resolved from the playlist directory. For example:

```text
# Player 1 then Player 2
red.gb
blue.gb
```

Load the `.m3u` through the ordinary **Load Content** flow. URLs, network paths,
embedded NULs, directory-only entries, and paths escaping above an absolute root
are rejected. The playlist must be at most 64 KiB and each cartridge at most
512 MiB. Shell shorthand such as `~` is not expanded inside a playlist.

M3U is the most predictable two-cartridge mode for independent core-owned save
files because the two cartridge paths, not the playlist filename, determine the
save names.

## Players, screens, and audio

By default, RetroArch Controller Port 1 controls machine 0 / Player 1 and Port 2
controls machine 1 / Player 2. The two **Player Controller** core options can
select any Controller Port from 1 through 5 for either player while content is
running, and may assign one port to both players intentionally. DualBoy selects
only between Libretro ports; connect and order physical Bluetooth controllers
in RetroArch or Steam. On Steam Deck, the built-in controls commonly occupy Port
1 and two Bluetooth pads often appear as Ports 2 and 3, but verify the actual
order in the frontend. RetroArch's **Maximum Users** must be at least as high as
the highest port selected in DualBoy.
**Swap Screens** exchanges the two screen positions without changing either
controller selection. In a player-only display mode, the selected player's
machine remains selected; there is no second visible screen to exchange.

Dual mode is native-pixel composition: GB/GBC is 320x144 side by side or 160x288
top/bottom; GBA is 480x160 or 240x320; and NDS is 512x384 or 256x768. Each DS
machine is always a fixed 256x384 top-screen-over-bottom-screen frame. Player-only
modes use the corresponding native 160x144, 240x160, or 256x384 frame. The MVP
emits only machine 0 / Player 1 audio.

For NDS touch, RetroArch pointer contacts are routed by the displayed bottom
screen, including successive pointer indices for two simultaneous contacts. The
first contact landing on each DS wins because the hardware is single-touch. As
an independent controller fallback, move that player's right analog stick to
aim, then use R3 or the right trigger (R2) to press the touchscreen. R3 can
reveal and press a stationary aim point; R2 presses only while the cursor is
already visible. Stick movement shows a high-contrast black-and-white reticle
with a cyan Player 1 or orange Player 2 center; it hides after three seconds
without meaningful stick movement. A physical pointer contact temporarily takes
precedence over the corresponding stick reticle.
Whether a particular Steam Deck input driver preserves multiple pointer indices
still requires physical validation.

RetroArch, not the core, owns input while its menu is open. Enable RetroArch's
**All Users Control Menu** setting so either configured controller can navigate
and close it with the frontend's existing menu toggle. **Start + Select** is an
optional shared toggle that avoids DualBoy's R3 stylus binding. A ready-to-merge
configuration fragment and the exact Steam Deck steps are in
[Steam Deck setup](docs/steam-deck.md#let-either-controller-use-the-menu).

## Core options

| Option key | Values | Default | Effect |
| --- | --- | --- | --- |
| `dualboy_mode` | `dual`, `player1`, `player2` | `dual` | Show both screens or one player's screen. |
| `dualboy_layout` | `side_by_side`, `top_bottom` | `side_by_side` | Arrange the two native-resolution frames. |
| `dualboy_nds_renderer` | `software`, `opengl` | `software` | Select the NDS renderer live. OpenGL requires Linux, `libEGL.so.1`, explicit surfaceless EGL, and an OpenGL 3.2 driver. |
| `dualboy_link` | `enabled`, `disabled` | `enabled` | Attach or detach the emulated cable or NDS local-wireless transport. Changing this for GBA resets both machines; NDS changes dynamically without a reload. |
| `dualboy_player1_controller` | `port1`, `port2`, `port3`, `port4`, `port5` | `port1` | Select the RetroArch controller port that operates Player 1. |
| `dualboy_player2_controller` | `port1`, `port2`, `port3`, `port4`, `port5` | `port2` | Select the RetroArch controller port that operates Player 2. |
| `dualboy_swap_players` | `disabled`, `enabled` | `disabled` | Exchange the two screen positions without changing either controller selection. The legacy key is retained for existing overrides; Player-only modes are unaffected. |
| `dualboy_audio_source` | `player1`, `disabled` | `player1` | Emit Player 1 audio or suppress output while preserving timing. |

Options are exposed through Libretro core-options v2, v1, and the legacy variable
API and are read while content is running. Both player-controller options may
name the same port. Physical device-to-port assignment and controller order
remain frontend tasks; ports beyond RetroArch's configured **Maximum Users** do
not provide input.

For NDS, **OpenGL** and **Local Link** are mutually exclusive. Selecting OpenGL
turns Local Link off immediately; selecting Local Link first restores Software
and then turns the link on. Turning OpenGL off does not turn the link back on, so
both may be off. On Linux, OpenGL obtains an explicit surfaceless EGL display
and uses one worker with two dedicated, unshared contexts, one per emulated DS.
It reads the native frames back to the CPU and uses the same Libretro video
callback as Software; DualBoy does not request or replace a Libretro frontend
hardware context. If that EGL/OpenGL 3.2 path is unavailable, DualBoy safely
stays on Software.

The log identifies the selected GL vendor and renderer and warns for known Mesa
software rasterizers. Because both DS frames are serialized on one EGL worker
and then read back and composed on the CPU, OpenGL is not assumed to be faster.
The automated Linux gate exercises Mesa software rendering; real RetroArch,
physical Steam Deck GPU acceleration, and performance remain unverified.

## Saves and save states

Battery saves use RetroArch's Save Files directory. GBA's canonical extension is
`.srm`; a legacy `.sav` is only copied when no canonical file exists and is
never renamed or deleted. Same-ROM loads use collision-safe `.srm.2` and
`.rtc.2` names for machine 1. NDS firmware uses `.firmware.bin` and
`.firmware.bin.2`; each console also has its own `.srm` cartridge save and MAC
address. Persisted NDS firmware must be an intact 128 KiB DualBoy-generated
image; malformed content is rejected before melonDS sees it. Core-managed saves
use atomic replacement and per-file advisory locks, and a second process that
cannot obtain them loads those regions read-only. NDS persistence publishes
only cartridge transactions completed by melonDS; an in-progress SPI write can
cross frame boundaries without stale disk-shadow bytes replacing its live
state. DualBoy does not repair already-corrupt or game-specific save formats.

Save states are separate RetroArch-managed files. For GB/GBC/GBA, a DualBoy state
contains both machines and link state, but loading it deliberately does not roll
battery SaveRAM or RTC backward. `retro_serialize_size()` is zero during an NDS
session: manual states are disabled instead of claiming to preserve an active
wireless transport. Rewind and runahead must also be disabled for NDS with a
per-core override:

```ini
rewind_enable = "false"
run_ahead_enabled = "false"
```

See [save ownership and formats](docs/saves.md) for the exact mode-by-mode rules,
device sizes, legacy import behavior, and lock-file details.

## Steam Deck

Build or obtain the Linux x86-64 core, install both the `.so` and `.info`
files in RetroArch's configured directories, and use a two-entry M3U when a
launcher cannot express a Libretro subsystem. Flatpak and EmuDeck paths differ
from system-package paths; detailed discovery, installation, controller, and
backup guidance is in [Steam Deck setup](docs/steam-deck.md).

The general Steam Deck installation procedure has been exercised with the prior
GBA core. This core is only tested on Steam Deck at present; broader platform
coverage is explicitly tracked as a future task.

## Scope and licenses

NDS multiplayer is same-process local wireless only: there is no Internet/WFC,
LAN netplay, RetroArch netplay, real-DS connectivity, Download Play, DSi, or
Slot-2 support. Fast-forward remains frontend-controlled while NDS transport is
inactive and is dynamically inhibited only after both emulated consoles have
joined `LocalMP`. There is no rewind/runahead support for NDS or machine-1 audio
output. Core cheat entrypoints are no-ops.

DualBoy-authored source remains MPL-2.0. SameBoy is Expat-licensed, mGBA and the
PR-derived adaptation are MPL-2.0, and the Libretro API header carries an MIT
notice. Because melonDS is GPL-3.0-or-later and is statically linked, the combined
`dualboy_libretro` binary and a distribution containing it are conveyed under
GPLv3-compatible terms. See [`LICENSE`](LICENSE), [`NOTICE`](NOTICE), and
[`THIRD_PARTY.md`](THIRD_PARTY.md).
