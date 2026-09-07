# Steam Deck setup

> The Linux x86-64 core has been installed and launched through RetroArch on a
> physical Steam Deck. Controller ordering, suspend/resume, long-session save
> reliability, and broad game compatibility still require the smoke checks below.

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
   include `gb|gbc|gba|m3u`.

An EmuDeck update does not know how to reinstall this project-specific core. Keep
the two source files somewhere outside generated RetroArch directories so they
can be restored after an update or configuration reset.

## Optional GBA BIOS

DualBoy checks for `gba_bios.bin` in the active System/BIOS directory. A valid
image is optional; GBA falls back to mGBA's normal high-level boot when it is
absent, and an invalid file is ignored with a warning. SameBoy's open GB/GBC boot
ROMs are embedded and need no external file.

Only use firmware you are legally entitled to use. The project does not ship
Nintendo firmware.

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

Useful core options are:

- **Display Mode = Dual**
- **Dual-screen Layout = Side by Side** for the Deck's landscape screen
- **Link Cable = Enabled**
- **Swap Players/Screens = Enabled** when physical port order is reversed
- **Audio Source = Player 1** or **Disabled**

Swap changes controller assignment and screen placement together. GBA link
changes reset both machines, so configure the cable before beginning play. The
MVP emits only Player 1 audio.

## Saves and backups

Confirm the active Save Files directory before the first run. In normal same-ROM
mode, expect machine 0's canonical `game.srm` and machine 1's
`game.srm.2`; RTC files use `.rtc` and `.rtc.2`. GBA also uses canonical
`.srm` files. M3U saves are named from each cartridge path, not the playlist.

Back up:

- `*.srm`, `*.srm.2`, `*.rtc`, and `*.rtc.2` from Save Files;
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
5. Create a RetroArch save state, advance both machines, load it, and verify
   volatile state rewinds together while battery progress remains intact.
6. Inspect the RetroArch log for a link watchdog, save-lock, or write error.

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

Reference documentation:

- [RetroArch command-line guide](https://docs.libretro.com/guides/cli-intro/)
- [RetroArch core installation guide](https://docs.libretro.com/guides/download-cores/)
- [Libretro BIOS placement guide](https://docs.libretro.com/library/bios/)
- [EmuDeck RetroArch guide](https://emudeck.github.io/emulators/steamos/retroarch/)
