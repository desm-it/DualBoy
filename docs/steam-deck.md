# Steam Deck setup

> A pre-NDS Linux x86-64 core was installed and launched through RetroArch on a
> physical Steam Deck. That result does not validate the new NDS engine. NDS
> local wireless, multitouch, performance, suspend/resume, long-session save
> reliability, and compatibility still require the smoke checks below.

## Build or transfer the core

DualBoy's reference artifact is Linux x86-64, the Steam Deck's native userspace
architecture. From a development checkout with Docker:

```sh
git submodule update --init --recursive
make test-linux-x86_64
make symbols-linux-x86_64
```

Transfer these two files to the Deck:

- `build-linux-x86_64/dualboy_libretro.so`
- `dualboy_libretro.info`

If conveying the binary to anyone else, use the CMake install tree or otherwise
include its `share/doc/dualboy` notices and license texts plus the corresponding
source/build offer required by GPLv3. A personal copy between your own machines
does not replace those files in a redistributable package.

Do not copy a macOS or ARM host build. On the Deck, `file
dualboy_libretro.so` should identify an x86-64 ELF shared object.

## Find RetroArch's active directories

Switch to Desktop Mode and open the same RetroArch installation that Game Mode or
EmuDeck will launch. In **Settings > Directory**, record:

- **Cores**
- **Core Info**
- **System/BIOS**
- **Save Files**
- **Save States**

Those settings are authoritative. RetroArch can be installed as a Flatpak, a
system package, or another bundle, and changing one path does not change the
others.

For the Flathub build, a common writable core location is:

```text
/home/deck/.var/app/org.libretro.RetroArch/config/retroarch/cores
```

Its packaged Core Info location may be read-only. Point RetroArch's Core Info
directory at a writable directory under its Flatpak configuration tree before
copying `dualboy_libretro.info`, or use the distribution's supported packaging
mechanism.

An EmuDeck installation commonly links RetroArch battery saves into:

```text
/home/deck/Emulation/saves/retroarch/saves
```

and commonly uses:

```text
/home/deck/Emulation/bios
```

for system files. Verify both in RetroArch: users can relocate the Emulation
directory, and EmuDeck configuration changes can update paths.

## Install

1. Copy `dualboy_libretro.so` to the recorded Cores directory.
2. Copy `dualboy_libretro.info` to the recorded writable Core Info directory.
3. Restart RetroArch or refresh core information.
4. Open **Load Core** and select **DualBoy**.
5. Open **Information > Core Information** and confirm the supported extensions
   include `gb|gbc|gba|nds|m3u` and the license is GPLv3.

An EmuDeck update does not know how to reinstall this project-specific core. Keep
the two source files somewhere outside generated RetroArch directories so they
can be restored after an update or configuration reset.

## BIOS and firmware

DualBoy checks for `gba_bios.bin` in the active System/BIOS directory. A valid
image is optional; GBA falls back to mGBA's normal high-level boot when it is
absent, and an invalid file is ignored with a warning. SameBoy's open GB/GBC boot
ROMs are embedded and need no external file.

NDS uses melonDS's built-in free BIOS implementation and creates an independent
writable firmware image for each internal console. DualBoy neither reads nor
ships Nintendo DS BIOS or firmware dumps, and arbitrary external firmware dumps
are not accepted. Its generated firmware is persisted in the Save Files
directory, not the System/BIOS directory; malformed or wrong-size persisted
images are rejected before they enter the emulator.

## Launch two players

For a duplicated cartridge, use RetroArch's ordinary **Load Content** flow. It
creates two independent machines from the same immutable ROM.

For different cartridges, the most launcher-friendly form is a local M3U next to
the ROMs:

```text
# First entry is Player 1
player-one.gba
player-two.gba
```

For NDS local wireless, both entries must instead be valid `.nds` cartridges;
mixed NDS/non-NDS pairs are rejected. A normal single-ROM load duplicates the
same NDS cartridge into two independent consoles. Download Play and an empty
second slot are not supported.

Load the M3U as normal content. It must contain exactly two local cartridge paths.
Relative paths are resolved from the playlist's directory. Do not use `~`,
URLs, or Windows/network paths. Keeping the playlist and cartridges under a
directory already visible to the RetroArch Flatpak avoids sandbox permission
problems.

The explicit alternative is RetroArch's **Load Subsystem > DualBoy two-player
link** flow. Add Player 1 and Player 2 content in order. A launcher or Steam ROM
Manager parser may not be able to express a two-content subsystem, which is why
M3U is recommended for a Game Mode shortcut.

If an automatic parser ignores `.m3u`, add the playlist as a non-Steam game or
create a custom parser entry that launches the same RetroArch installation with
DualBoy. Do not put both ROM paths into one shell-quoted playlist line; they must
be two separate lines inside the file.

## Controllers and display

Connect and order two controllers in Steam's controller settings before launch.
RetroArch port 1 drives DualBoy machine 0 and port 2 drives machine 1. The Deck's
built-in controls can occupy one port; the other player needs another controller
or a deliberately configured second input device.

Each NDS machine always shows its 256x192 top screen above its 256x192 bottom
touch screen. RetroArch pointer contacts are accepted only inside displayed
bottom-screen rectangles. If the active input driver exposes multiple pointer
indices, contacts on the two bottom screens can drive both consoles at once; this
must be verified on the installed Deck/input-driver combination. For an
independent controller stylus fallback, move that player's right stick to aim,
then hold R3 to press. Moving the stick displays a
black-and-white aiming reticle on that player's bottom screen before the touch is
pressed. Player 1 has a cyan center and Player 2 an orange center; the reticle
hides after three seconds without meaningful movement. A direct touchscreen
contact takes priority for that player.

Useful core options are:

- **Display Mode = Dual**
- **Dual-screen Layout = Side by Side** for the Deck's landscape screen
- **Local Link = Enabled**
- **Swap Players/Screens = Enabled** when physical port order is reversed
- **Audio Source = Player 1** or **Disabled**

Swap changes controller assignment and screen placement together. GBA link
changes reset both machines, so configure the cable before beginning play. NDS
link changes dynamically: disabling it disconnects local wireless without a
content reload. Fast-forward is available until both NDS consoles join the
enabled transport, then DualBoy requests a frontend override to inhibit it; the
override is released when either console leaves or link is disabled. The MVP
emits only Player 1 audio. Enable **Local Link** before entering a multiplayer
lobby; toggling it off during play intentionally disconnects the session.

NDS manual states, rewind, and runahead are unsupported because the two machine
states do not contain the pair-owned `LocalMP` queues. Save a per-core override
for DualBoy containing exactly:

```ini
rewind_enable = "false"
run_ahead_enabled = "false"
```

## Saves and backups

Confirm the active Save Files directory before the first run. In normal same-ROM
mode, expect machine 0's canonical `game.srm` and machine 1's
`game.srm.2`; RTC files use `.rtc` and `.rtc.2`. GBA also uses canonical
`.srm` files. NDS adds `game.firmware.bin` and `game.firmware.bin.2` for the two
independent writable firmware images. M3U saves are named from each cartridge
path, not the playlist.

Back up:

- `*.srm`, `*.srm.2`, `*.rtc`, `*.rtc.2`, `*.firmware.bin`, and
  `*.firmware.bin.2` from Save Files;
- RetroArch save-state files from the separately configured Save States
  directory; and
- the M3U when its relative layout matters.

`*.dualboy.lock` files are advisory coordination metadata and do not contain
game progress. They may remain after RetroArch exits and do not need cloud
synchronization. Do not delete or overwrite battery files while RetroArch is
running. If another process already owns a required lock, DualBoy loads
core-managed saves read-only and logs a warning.

See [save ownership and formats](saves.md) before importing standalone mGBA
`.sav` files or reorganizing ROM basenames.

## First-run smoke checklist

Before trusting a long session:

1. Load a source-generated or otherwise legally redistributable test cartridge.
2. Confirm both screens advance and two controller ports affect different
   machines.
3. Toggle **Swap Players/Screens** and verify both input and placement swap.
4. Create distinct in-game battery progress on both machines, cleanly unload
   content, reload, and verify both.
5. For GB/GBC/GBA, create a RetroArch save state, advance both machines, load it,
   and verify volatile state rewinds together while battery progress remains
   intact. Do not perform this step for NDS.
6. For NDS, enter a local-wireless lobby using source-built or explicitly
   redistributable test software, verify both peers exchange data, then toggle
   **Local Link** off and on without reloading.
7. For NDS, place two simultaneous pointer contacts on different displayed
   bottom screens and record the active RetroArch/input driver and result.
8. Inspect the RetroArch log for a worker, transport, save-lock, or write error.

Passing the repository's automated harness does not replace this real-frontend
check.

## Troubleshooting

- **DualBoy is not listed:** verify both the Core and Core Info paths, file
  permissions, and the x86-64 ELF architecture.
- **Core loads but a playlist does not:** use a real local path, exactly two
  non-comment entries, compatible engine families, and files accessible inside
  the Flatpak sandbox.
- **Only one controller works:** check Steam controller order and RetroArch's
  Port 1/Port 2 device assignments.
- **Saves do not update:** check the Save Files directory, log output, directory
  writability, and whether another RetroArch process owns a
  `.dualboy.lock`.
- **GBA link option appears to restart play:** this reset is intentional when
  changing the mGBA link topology.
- **NDS save states are unavailable:** this is intentional; disable rewind and
  runahead with the override above and use in-game battery saves.
- **NDS fast-forward stops in a multiplayer lobby:** this is the intended
  transport-safety override. Leave the lobby or disable **Local Link** to release
  it.

Reference documentation:

- [RetroArch command-line guide](https://docs.libretro.com/guides/cli-intro/)
- [RetroArch core installation guide](https://docs.libretro.com/guides/download-cores/)
- [Libretro BIOS placement guide](https://docs.libretro.com/library/bios/)
- [EmuDeck RetroArch guide](https://emudeck.github.io/emulators/steamos/retroarch/)
