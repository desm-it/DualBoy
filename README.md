# DualBoy

DualBoy is one Libretro core that runs exactly two linked Game Boy, Game Boy
Color, or Game Boy Advance machines in one RetroArch session. GB/GBC uses two
SameBoy instances. GBA uses two mGBA instances and the cooperative SIO lockstep
implementation adapted from libretro/mgba PR #318. DualBoy does not load another
Libretro core.

The MVP supports one cartridge duplicated into both machines, the two-content
`dualboylink` subsystem, and local two-entry M3U playlists. It provides paired
video, two RetroPad ports, one-machine audio, independent battery saves, and one
transactional save-state containing both machines and their link state.

This is an integration-stage project. Automated tests use source-generated ROMs;
there has not yet been an end-to-end run in a real RetroArch process or on Steam
Deck hardware, and no commercial-game compatibility is claimed. See
[implementation status](docs/status.md) for the exact verification boundary.

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

This installs the core below `lib/libretro` and its metadata below
`share/libretro/info` in the selected prefix.

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
   are embedded, so no external GB/GBC boot ROM is needed.

If RetroArch's online updater owns a read-only info directory, select a writable
Core Info path or install the `.info` file using the packaging mechanism for
that RetroArch distribution.

## Load content

DualBoy accepts `.gb`, `.gbc`, `.gba`, and `.m3u`. Cartridge headers, not
filename extensions alone, select the engine. Both cartridges must use the same
engine family: GB and GBC may be paired, but GB/GBC cannot be mixed with GBA.

### Normal load: duplicate one cartridge

Use RetroArch's normal **Load Content** flow and select one cartridge. DualBoy
shares the immutable ROM bytes but creates two independent machines, including
separate SaveRAM, RTC, video, audio, and state.

The equivalent command-line form is:

```sh
retroarch -L /path/to/dualboy_libretro.so /path/to/game.gb
```

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
64 MiB. Shell shorthand such as `~` is not expanded inside a playlist.

M3U is the most predictable two-cartridge mode for independent core-owned save
files because the two cartridge paths, not the playlist filename, determine the
save names.

## Players, screens, and audio

RetroArch port 1 controls machine 0, shown on the left or top. Port 2 controls
machine 1, shown on the right or bottom. **Swap Players/Screens** changes both
controller assignment and screen position together.

Dual mode is native-pixel composition: GB/GBC is 320x144 side by side or 160x288
top/bottom; GBA is 480x160 or 240x320. Player-only modes use the corresponding
native 160x144 or 240x160 frame. The MVP emits only machine 0 / Player 1 audio.

## Core options

| Option key | Values | Default | Effect |
| --- | --- | --- | --- |
| `dualboy_mode` | `dual`, `player1`, `player2` | `dual` | Show both screens or one player's screen. |
| `dualboy_layout` | `side_by_side`, `top_bottom` | `side_by_side` | Arrange the two native-resolution frames. |
| `dualboy_link` | `enabled`, `disabled` | `enabled` | Attach or detach the emulated link cable. Changing this for GBA resets both machines. |
| `dualboy_swap_players` | `disabled`, `enabled` | `disabled` | Swap screens and controller ports as one operation. |
| `dualboy_audio_source` | `player1`, `disabled` | `player1` | Emit Player 1 audio or suppress output while preserving timing. |

Options are exposed through Libretro core-options v2, v1, and the legacy variable
API and are read while content is running.

## Saves and save states

Battery saves use RetroArch's Save Files directory. GBA's canonical extension is
`.srm`; a legacy `.sav` is only copied when no canonical file exists and is
never renamed or deleted. Same-ROM loads use collision-safe `.srm.2` and
`.rtc.2` names for machine 1. Core-managed saves use atomic replacement and
per-file advisory locks, and a second process that cannot obtain them loads those
regions read-only.

Save states are separate RetroArch-managed files. A DualBoy state contains both
machines and link state, but loading it deliberately does not roll battery SaveRAM
or RTC backward. Back up battery files and save states independently.

See [save ownership and formats](docs/saves.md) for the exact mode-by-mode rules,
device sizes, legacy import behavior, and lock-file details.

## Steam Deck

Build or obtain the Linux x86-64 core, install both the `.so` and `.info`
files in RetroArch's configured directories, and use a two-entry M3U when a
launcher cannot express a Libretro subsystem. Flatpak and EmuDeck paths differ
from system-package paths; detailed discovery, installation, controller, and
backup guidance is in [Steam Deck setup](docs/steam-deck.md).

The Steam Deck procedure is documented from the implemented interfaces and public
RetroArch/EmuDeck conventions. It has not yet been exercised on Deck hardware.

## Scope and licenses

The MVP has no link netplay, wireless multiplayer, fast-forward coordination,
rewind coordination, or machine-1 audio mixer. Core cheat entrypoints are no-ops.

DualBoy-authored code is MPL-2.0. SameBoy is Expat-licensed, mGBA and the PR-derived
adaptation are MPL-2.0, and the Libretro API header carries an MIT notice. See
[`LICENSE`](LICENSE) and [`THIRD_PARTY.md`](THIRD_PARTY.md).
