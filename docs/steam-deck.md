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

If conveying the binary to anyone else, use the CMake install tree for the
binary, notices, and license texts, and also accompany it with complete
corresponding source and build scripts through a GPLv3-compliant distribution
method. The install tree alone is not a redistributable source bundle. A
personal copy between your own machines does not replace those materials in a
redistributable package.

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

Connect and order the controllers in Steam's controller settings, then confirm
their logical port assignments in RetroArch. The Deck's built-in controls
commonly occupy Port 1, so the first two Bluetooth pads often appear as Ports 2
and 3, but this ordering is not guaranteed and must be verified in the frontend.
Set RetroArch's **Maximum Users** to at least the highest port that DualBoy will
select. DualBoy cannot identify Bluetooth devices by name or change their
physical order. Its live **Player 1 Controller** and **Player 2 Controller**
options only choose which of RetroArch's Ports 1 through 5 feeds each emulated
machine. Defaults are Port 1 and Port 2 respectively; selecting the same port for
both is supported when one controller should drive both machines.

Each NDS machine always shows its 256x192 top screen above its 256x192 bottom
touch screen. RetroArch pointer contacts are accepted only inside displayed
bottom-screen rectangles. If the active input driver exposes multiple pointer
indices, contacts on the two bottom screens can drive both consoles at once; this
must be verified on the installed Deck/input-driver combination. For an
independent controller stylus fallback, move that player's right stick to aim,
then hold R3 or the right trigger (R2) to press. R3 can reveal a stationary aim
point; R2 works only while the aiming cursor is visible. Moving the stick
displays a black-and-white aiming reticle on that player's bottom screen before
the touch is pressed. Player 1 has a cyan center and Player 2 an orange center;
the reticle hides after three seconds without meaningful movement. A direct
touchscreen contact takes priority for that player.

Useful core options are:

- **Display Mode = Dual**
- **Dual-screen Layout = Side by Side** for the Deck's landscape screen
- For local wireless: **Nintendo DS Renderer = Software** and **Local Link =
  Enabled**
- For experimental non-linked rendering: **Nintendo DS Renderer = OpenGL** and
  **Local Link = Disabled**
- **Player 1 Controller = Controller Port 1**
- **Player 2 Controller = Controller Port 2**
- **Swap Screens = Enabled** when you want to exchange only their placement
- **Audio Source = Player 1** or **Disabled**

For a Deck with two Bluetooth controllers, use the verified logical ports rather
than assuming connection order. For example, if RetroArch shows the Bluetooth
pads on Ports 2 and 3, assign those two ports to Player 1 and Player 2 in either
order.

Screen swap does not change controller assignments, and has no effect in a
player-only display mode. Changing a player-controller option takes effect live;
for NDS it also clears that player's old aiming cursor. GBA link
changes reset both machines, so configure the cable before beginning play. NDS
link changes dynamically: disabling it disconnects local wireless without a
content reload. Fast-forward is available until both NDS consoles join the
enabled transport, then DualBoy requests a frontend override to inhibit it; the
override is released when either console leaves or link is disabled. The MVP
emits only Player 1 audio. Enable **Local Link** before entering a multiplayer
lobby; toggling it off during play intentionally disconnects the session.

NDS OpenGL and Local Link are mutually exclusive. Choosing either one disables
the other live, and both may be disabled. OpenGL requires the Deck's
`libEGL.so.1` runtime, explicit surfaceless EGL support, and an OpenGL 3.2 driver.
DualBoy obtains an explicit surfaceless EGL display and creates a dedicated
two-context pair on its NDS worker. It does not request or replace a Libretro
hardware context, and final frames still use the ordinary CPU video callback.
The log reports the actual GL renderer and warns if it is a known software
rasterizer. This path has not yet passed the
physical Deck/RetroArch gate, and its serial GL execution plus CPU readback may
not be faster, so retain Software for the documented multiplayer procedure
until device validation and profiling are recorded.

NDS manual states, rewind, and runahead are unsupported because the two machine
states do not contain the pair-owned `LocalMP` queues. Save a per-core override
for DualBoy containing exactly:

```ini
rewind_enable = "false"
run_ahead_enabled = "false"
```

### Let either controller use the menu

RetroArch owns its menu and menu hotkeys; a Libretro core cannot take control of
them. This distinction is especially visible with the default **Pause Content
When Menu Is Active** setting: RetroArch stops calling DualBoy's `retro_run()`.
Even when that pause setting is disabled, RetroArch blocks Libretro input while
it runs the core behind the menu. A DualBoy core option therefore cannot make
Player 2 navigate a paused Quick Menu.

Configure this in RetroArch itself:

1. Open **Settings > Input > Menu Controls** and enable **All Users Control
   Menu**, then save the current RetroArch configuration.
2. If the existing menu toggle already opens the menu from every controller,
   keep it. Otherwise, open **Settings > Input > Hotkeys** and optionally set
   **Menu Toggle (Controller Combo)** to **Start + Select**.

With **All Users Control Menu** enabled, either configured controller can
navigate the menu and use the frontend's configured menu action to close it.
When selected, Start + Select lets either controller open or close the menu and
avoids L3 + R3, which collides with DualBoy's R3 controller stylus.

The repository includes the equivalent minimal fragment at
[`config/retroarch/dualboy-menu-controls.cfg`](../config/retroarch/dualboy-menu-controls.cfg).
The fragment changes only **All Users Control Menu**; it deliberately preserves
the user's existing menu-toggle binding. Do not replace `retroarch.cfg` with
this one-line fragment. Either merge its assignment into the active
configuration while RetroArch is closed, or append it for a particular launch:

```sh
retroarch --appendconfig=/absolute/path/to/dualboy-menu-controls.cfg \
  -L /absolute/path/to/dualboy_libretro.so /absolute/path/to/content.m3u
```

For users who choose the optional combo, the config value
`input_menu_toggle_gamepad_combo = "4"` means Start + Select in both RetroArch
1.22.2 and the primary source revision audited on 2026-09-08. Prefer the named UI
setting rather than carrying that numeric value into an unknown future version.

This frontend boundary and the two keys were verified against RetroArch primary
source commit
[`8039bc24666366ade168776ccffd303620fc26ed`](https://github.com/libretro/RetroArch/commit/8039bc24666366ade168776ccffd303620fc26ed):

- RetroArch defines **All Users Control Menu** and **Menu Toggle (Controller
  Combo)** as frontend settings in
  [`settings_def_input_haptics.h`](https://github.com/libretro/RetroArch/blob/8039bc24666366ade168776ccffd303620fc26ed/settings/settings_def_input_haptics.h#L7-L11)
  and
  [`settings_def_input_haptics.h`](https://github.com/libretro/RetroArch/blob/8039bc24666366ade168776ccffd303620fc26ed/settings/settings_def_input_haptics.h#L53-L57).
- Its input loop reads every configured controller and restricts menu input to
  User 1 only when `all_users_control_menu` is false in
  [`input_driver.c`](https://github.com/libretro/RetroArch/blob/8039bc24666366ade168776ccffd303620fc26ed/input/input_driver.c#L8150-L8285); the
  menu-toggle combination is then evaluated from those collected frontend bits
  in
  [`runloop.c`](https://github.com/libretro/RetroArch/blob/8039bc24666366ade168776ccffd303620fc26ed/runloop.c#L6165-L6174).
- The menu runloop calls the core only when content is allowed to keep running
  and explicitly blocks Libretro input in that case in
  [`runloop.c`](https://github.com/libretro/RetroArch/blob/8039bc24666366ade168776ccffd303620fc26ed/runloop.c#L5965-L6005).

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
2. Confirm both screens advance and two selected controller ports affect
   different machines. Exercise at least one selection from Ports 3 through 5,
   exchange the two **Player Controller** options, and verify the controls move
   to the intended players without changing screen positions.
3. Toggle **Swap Screens** and verify placement swaps while each controller
   continues operating the same machine.
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
- **Only one controller works:** check Steam controller order, every relevant
  RetroArch port assignment, RetroArch's **Maximum Users**, and DualBoy's two
  **Player Controller** options. The maximum-user value must be at least as high
  as the highest selected port.
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
- **`Nintendo DS frame workers timed out` appears:** the pair has been retired
  for safety and an in-game reset cannot recover it; unload and reload the
  content. Current builds bound the known repeated LocalMP host-poll wait and
  log only the first failed frame in that failure episode. Capture that first
  timeout and the transport messages immediately before it if it recurs; the
  absence of repeated errors does not by itself prove wireless is still working.

Reference documentation:

- [RetroArch command-line guide](https://docs.libretro.com/guides/cli-intro/)
- [RetroArch core installation guide](https://docs.libretro.com/guides/download-cores/)
- [Libretro BIOS placement guide](https://docs.libretro.com/library/bios/)
- [EmuDeck RetroArch guide](https://emudeck.github.io/emulators/steamos/retroarch/)
