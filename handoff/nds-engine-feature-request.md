# Codex handoff: add two-instance Nintendo DS local multiplayer

## Mission

Extend DualBoy with Nintendo DS support by embedding an **untouched, pinned upstream melonDS engine** and running exactly two local NDS instances inside the existing Libretro core.

Implement the feature autonomously when this handoff is given to Codex: inspect the current repository first, update tests and documentation with the code, run every applicable validation gate, fix failures, and report observed results honestly. Do not stop at a plan or scaffold. Do not claim game, RetroArch, or Steam Deck compatibility without executing the corresponding test.

This document defines requested scope and decisions. `AGENTS.md`, `THIRD_PARTY.md`, `docs/architecture.md`, `docs/saves.md`, and `docs/status.md` remain authoritative for current repository conventions and verified status.

## Product outcome

One `dualboy_libretro.so` must support:

- GB/GBC through the existing SameBoy adapter.
- GBA through the existing mGBA adapter.
- NDS through a new melonDS adapter.
- Exactly two NDS consoles in one RetroArch session.
- The two NDS consoles connected through melonDS same-process local wireless when link mode is active.
- Both NDS consoles visible in the existing combined DualBoy framebuffer and independently controlled through Libretro ports 0 and 1.

Nintendo DS “link” means emulated DS local wireless, not Internet/WFC emulation and not RetroArch netplay.

## Fixed scope decisions

### Required

- Use upstream [`melonDS-emu/melonDS`](https://github.com/melonDS-emu/melonDS) directly as a pinned Git submodule under `third_party/`.
- Do not modify, patch, or fork melonDS. All integration code belongs in DualBoy-authored files.
- Build melonDS as an internal static engine. Never load or nest a melonDS Libretro core.
- Create exactly two `melonDS::NDS` instances with independent mutable state.
- Use upstream melonDS `LocalMP` unchanged. It may retain its internal 16-instance capacity; DualBoy must register only instance IDs 0 and 1.
- Support normal one-ROM loading by loading the same immutable `.nds` ROM into both consoles.
- Extend the existing two-content subsystem and M3U paths to accept exactly two `.nds` ROMs.
- Reject mixed NDS/non-NDS pairs clearly.
- Use header/content detection rather than trusting only `.nds` filename extensions.
- Give each console independent cartridge SaveRAM and writable firmware state.
- Give the two consoles unique wireless identities/MAC addresses.
- Render each NDS console with a fixed top-screen-over-bottom-screen layout.
- Feed each resulting `256x384` machine frame into the existing DualBoy outer compositor. Side-by-side therefore produces `512x384` output.
- Preserve the existing outer display modes where they remain generic and inexpensive: side by side, consoles top/bottom, Player 1 only, Player 2 only, and coupled player/screen swap. Do not add new NDS-specific advanced layouts.
- Map Libretro port 0 to NDS instance 0 and port 1 to NDS instance 1.
- Support independent DS touch input. Poll successive `RETRO_DEVICE_POINTER` indices for multitouch, route each contact by the displayed bottom-screen rectangle, transform it to local DS coordinates, and allow one active touch per emulated DS. Provide an independent controller/analog stylus fallback for each player.
- Emit machine 0 audio only, matching the existing DualBoy MVP policy.
- Keep link enable/disable dynamically changeable without reloading content.
- Permit fast-forward whenever NDS local multiplayer transport is inactive. Dynamically inhibit fast-forward only while both NDS instances are actively joined to the enabled `LocalMP` transport, and release the override when either instance leaves or link is disabled.
- Treat rewind and runahead as unsupported for NDS sessions and document that users must disable them in the RetroArch/DualBoy core override.
- Manual savestates may serialize both NDS machine states, but preserving an active wireless session is explicitly not required. Loading a state may reset or break the local wireless connection; document that savestates must not be used during multiplayer.
- Build and validate Linux x86-64 for RetroArch/Steam Deck as the primary target.

### Explicit non-goals

Do not spend implementation time on:

- Enlarged touchscreen layouts.
- Hybrid primary-screen layouts.
- Swapping an individual DS’s top and bottom screens.
- Nintendo DS Download Play or cartridge-less guest boot.
- Nintendo Wi-Fi Connection, Internet emulation, LAN netplay, or real-DS connectivity.
- DSi or DSiWare.
- DS Slot-2 or GBA connectivity.
- More than two NDS instances.
- Perfect serialization of in-flight `LocalMP` queues, waits, or wireless sessions.
- Rewind or runahead compatibility.
- Audio mixing or machine 1 audio.
- OpenGL/hardware rendering in the initial implementation.
- Broad exposure of standalone melonDS frontend options.

## Upstream integration boundary

At analysis time, upstream melonDS commit `906e9ebb27da8c6a715cd7abab4abfe8a8d29427` was inspected. Re-check upstream state and choose an exact audited revision before implementation; do not silently assume this snapshot remains correct.

Upstream melonDS already provides:

- The static CMake target named `core`.
- Multiple independent `melonDS::NDS` objects.
- `thread_local melonDS::NDS::Current`.
- Software rendering.
- ARM interpreter/JIT support.
- Cartridge handling and save memory.
- Machine savestates.
- Built-in BIOS/firmware replacements.
- Wi-Fi hardware emulation.
- `src/net/LocalMP.cpp` and `LocalMP.h`.

Configure the dependency without its desktop frontend and with the smallest feature set needed for this request. Expected initial direction:

```cmake
set(BUILD_QT_SDL OFF)
set(ENABLE_GDBSTUB OFF)
set(ENABLE_OGLRENDERER OFF)
add_subdirectory(third_party/melonDS)
target_link_libraries(dualboy_libretro PRIVATE core)
```

`LocalMP.cpp` is not part of melonDS’s base `core` target at the inspected revision. It may be compiled unchanged as an additional source of the DualBoy target. This is permitted; editing that source is not.

The newer [`JesseTG/melonds-ds`](https://github.com/JesseTG/melonds-ds) Libretro core was inspected at commit `bc4e4b67d2d470d7c682810a1e892cafd6f9082b`. Use it as a reference for consuming an untouched melonDS dependency and for `Platform.h`, console construction, software rendering, input, SaveRAM, firmware, and Libretro integration patterns. Do not embed its Libretro entrypoint or create a nested core. Avoid adding it as a production dependency unless a concrete need is established.

## Suggested DualBoy-owned modules

Keep all adaptation outside `third_party/melonDS`, for example:

```text
src/engines/melonds/
├── melonds_adapter.cpp
├── melonds_adapter.h
├── melonds_platform.cpp
├── melonds_workers.cpp
├── melonds_input.cpp
├── melonds_video.cpp
└── melonds_persistence.cpp
```

Exact file boundaries may change after inspecting current code, but maintain the existing rule that engine-private objects do not leak into `src/frontend/`.

Expose the adapter through the existing `dualboy_engine_ops` boundary. Extend that boundary minimally where DS requirements prove the existing button-only input structure insufficient. Prefer one structured per-machine input value containing buttons, touch state and coordinates over adding many unrelated callbacks.

## Runtime design

### Instance ownership

The melonDS pair adapter owns:

- Two `melonDS::NDS` objects.
- Two per-instance frontend/userdata contexts.
- One upstream `melonDS::LocalMP` transport.
- Two persistent worker threads.
- Start/end frame barriers and shutdown/error state.
- Two independent cartridge-save shadows/managers.
- Two independent firmware objects or writable overlays.
- Per-machine composed `256x384` video buffers.

ROM bytes may be shared only when immutable. Save data, firmware writes, video, audio, savestates, worker state and wireless identity must not be shared.

### Platform callbacks

Implement the melonDS `Platform.h` contract in DualBoy-owned C++ files. Reuse/adapt proven melonDS DS frontend patterns where license-compatible, retaining required attribution. Required areas include logging, files, save-write callbacks, mutexes, semaphores, threads, time/sleep, stop/error behavior, and multiplayer callbacks.

Each NDS receives userdata equivalent to:

```cpp
struct dualboy_melonds_instance_context {
    dualboy_melonds_pair *pair;
    unsigned instance;
};
```

DualBoy’s `Platform::MP_*` functions derive instance 0 or 1 from this userdata and forward to the pair-owned upstream `LocalMP` object.

### Frame scheduling

Do not run linked instances sequentially on one thread. `LocalMP` performs blocking/timed packet waits, so one console can wait for the peer that has not run yet.

Use two persistent workers. For each `retro_run()`:

1. Poll both controllers and all pointer contacts once.
2. Apply per-machine input.
3. Release both workers to call one `NDS::RunFrame()` concurrently.
4. Allow both to communicate through the shared `LocalMP` object.
5. Wait for both workers at a completion barrier.
6. Read and compose top/bottom software framebuffers for each machine.
7. Pass the two machine frames through the existing outer compositor.
8. Emit machine 0 audio.

Teardown after partial initialization must remain idempotent. A stopped, sleeping, failed, or partially loaded instance must not deadlock the Libretro thread or its peer.

### Dynamic link and fast-forward policy

Retain the existing `dualboy_link` live core option and adapt its label/description so it is not always called a cable for NDS.

Track whether each emulated NDS has called `Platform::MP_Begin()` or `MP_End()`. When the user changes link state:

- Link disabled: disconnect/no-op the transport while both consoles continue independently; fast-forward remains available.
- Link enabled: register only currently active NDS instances, and register a later instance when it calls `MP_Begin()`.
- Both instances joined: use `RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE` to force normal speed and inhibit the frontend fast-forward toggle.
- Either instance leaves, or link is disabled: clear the override so frontend-controlled fast-forward works again.

Enabling link before entering a multiplayer lobby is the supported workflow. Disabling link during a game session intentionally disconnects it. Do not add packet-activity timeout heuristics unless actual testing demonstrates that `MP_Begin`/`MP_End` is insufficient.

Both NDS consoles must continue advancing together when unlinked; frontend fast-forward should accelerate the complete two-console core, not one machine independently.

### Rewind, runahead and manual states

Libretro does not provide a clean way for a core to distinguish ordinary manual savestate calls from rewind/runahead serialization calls. Do not build a complex workaround.

- Mark/document rewind and runahead unsupported for NDS.
- Add exact RetroArch core-override guidance:

```ini
rewind_enable = "false"
run_ahead_enabled = "false"
```

- Keep manual two-machine NDS savestates only if they can be implemented safely without pretending that `LocalMP` state is complete.
- On NDS state load, reset/reconstruct transport synchronization as needed to avoid stale queues or blocked workers.
- Clearly warn that state load during multiplayer disconnects or invalidates the session and is unsupported.
- Do not regress the complete linked savestate behavior of the existing SameBoy and mGBA engines.

## Video and touch details

One native DS screen is `256x192`. The required fixed per-machine arrangement is:

```text
256x192 top screen
256x192 bottom/touch screen
--------------------------
256x384 machine frame
```

The existing outer compositor then gives:

- Side by side: `512x384`.
- Consoles top/bottom, if retained: `256x768`.
- Single player: `256x384`.

Replace the current fixed `480x320` maximum allocation with a safe maximum or dynamic allocation that accommodates these geometries without regressing GB/GBC/GBA.

For multitouch:

1. Query `RETRO_DEVICE_POINTER` indices successively until `PRESSED` is false.
2. Use normalized coordinates relative to the emitted game image.
3. Determine whether the contact lies inside machine 0 or machine 1’s displayed bottom-screen rectangle.
4. Apply the inverse compositor/layout transform.
5. Clamp to local DS coordinates `0..255` by `0..191`.
6. Deliver at most one contact to each NDS instance; deterministic first-contact-wins behavior is acceptable if multiple contacts land on one emulated single-touch screen.
7. Verify on a physical Steam Deck whether the active RetroArch/EmuDeck Linux input driver preserves multiple pointer indices. Do not infer this solely from API support.

## Content, saves and firmware

- Add NDS platform/header detection and engine-family selection.
- Normal load duplicates one ROM into independent consoles.
- Subsystem and M3U load exactly two NDS ROMs.
- No empty second slot or Download Play flow is required.
- Preserve existing stable, non-clobbering save naming. Same-ROM NDS sessions need separate canonical files such as `<stem>.srm` and `<stem>.srm.2`; different-ROM sessions retain one save identity per ROM.
- Respect actual melonDS cartridge save lengths and write notifications.
- Test save independence across complete adapter destruction and recreation.
- Use independent writable firmware state and distinct MAC addresses. A common immutable base may be copied.
- Do not bundle copyrighted Nintendo BIOS or firmware. Built-in replacements are the default; optional user-supplied files may be supported only if straightforward and clearly documented.

## Licensing

melonDS is GPL-3.0-or-later. Statically linking it changes the combined DualBoy distribution obligations.

- Preserve existing MPL-2.0 notices on DualBoy-authored files unless there is a documented reason to relicense them.
- Document that the combined binary/distribution is governed by GPLv3-compatible terms.
- Update `LICENSE`, `README.md`, `THIRD_PARTY.md`, core metadata and packaged notices as required after a real license audit.
- Record the exact melonDS source URL, commit, license, build selection, absence of local modifications, and update procedure.
- Do not copy code from melonDS DS or other projects without retaining its license and attribution.

## Testing strategy

Follow the repository’s existing generated/legal-test-content policy. Never add, download, or commit commercial ROMs or copyrighted BIOS/firmware dumps.

Add automated coverage for at least:

- NDS header detection and mixed-family rejection.
- Normal same-ROM and two-ROM content ownership.
- Exactly two live NDS objects with distinct userdata and mutable state.
- Partial-load and repeated load/unload cleanup.
- Independent controller/button routing.
- Multi-pointer routing into the correct machine’s bottom screen.
- DS video composition and all retained outer geometries.
- Distinct save buffers, names, flushes and destroy/recreate reload.
- Unique firmware/wireless identities.
- `LocalMP` instance registration limited to IDs 0 and 1.
- Dynamic link enable/disable without content reload.
- Fast-forward override activation only when both NDS instances are joined, and release when inactive.
- Two concurrent frame workers without deadlock under timeout, stop and failure paths.
- Two-machine state round trip if NDS manual states are retained, with documented/reset link behavior.
- Existing SameBoy and mGBA suites remain green.

Use source-built or explicitly redistributable NDS homebrew/test software for an actual local-wireless test. If no suitable legal test ROM can be produced or located, keep transport-level and synthetic tests but record retail/local-wireless compatibility as unverified. Do not substitute an inferred result.

## Validation gates

At minimum run and record:

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
make test
make linux-x86_64
make test-linux-x86_64
make asan-linux-native
make symbols-linux-x86_64
```

Adapt sanitizer/build targets if the C++ dependency exposes real integration issues, but do not weaken existing warnings or silently drop tests. Preserve a single exported Libretro ABI and verify no melonDS or nested Libretro entrypoints leak into the shared object.

A host build is diagnostic, not Linux x86-64 validation. A desktop RetroArch run is not Steam Deck validation. Record each boundary separately in `docs/status.md`.

## Acceptance criteria

The request is complete only when real evidence establishes:

1. Untouched melonDS is pinned as a submodule and its selected sources build reproducibly into `dualboy_libretro.so`.
2. Existing GB/GBC/GBA behavior and tests remain green.
3. One `.nds` ROM creates two independent visible NDS consoles.
4. Two `.nds` ROMs work through the existing subsystem and M3U paths.
5. Both consoles render top-over-bottom, with the pair side by side by default.
6. Ports 0 and 1 independently control instances 0 and 1.
7. Two simultaneous Libretro pointer contacts are routed to separate DS touchscreens in automated tests; physical Steam Deck behavior is reported only after hardware validation.
8. Upstream `LocalMP` connects exactly the two internal instances without nested Libretro cores or external networking.
9. Link can be enabled and disabled at runtime without reloading content.
10. Fast-forward remains available while the transport is inactive, is dynamically inhibited while both instances are joined, and is restored afterward.
11. Cartridge saves remain independent and survive full engine destruction/recreation.
12. Rewind, runahead and multiplayer-savestate limitations are explicit and do not regress existing platforms.
13. The strict Linux x86-64, native sanitizer and symbol checks pass, or blockers are documented with exact failing commands and output.
14. `README.md`, `THIRD_PARTY.md`, `docs/architecture.md`, `docs/saves.md`, `docs/steam-deck.md`, `docs/status.md`, and `AGENTS.md` accurately describe the implemented state and license impact.

## Implementation order

Keep the tree buildable and commit cohesive changes. A sensible order is:

1. Audit/pin melonDS, update licensing metadata, and integrate a software-only static build.
2. Add NDS platform detection and a minimal single-instance adapter smoke test.
3. Add the second instance, worker lifecycle and fixed per-machine top/bottom composition.
4. Extend the engine input contract, two-controller routing and multitouch transforms.
5. Integrate pair-owned upstream `LocalMP` and dynamic link transitions.
6. Add dynamic fast-forward inhibition tied to both instances being joined.
7. Add independent SaveRAM and firmware/wireless identities.
8. Add basic two-machine manual states only if safe; explicitly reset/abandon wireless-session restoration.
9. Extend normal/subsystem/M3U Libretro paths and ABI tests.
10. Run strict builds, sanitizers, symbol checks, real RetroArch tests, and physical Steam Deck validation where available.
11. Update all handoff/status documentation with exact results and remaining unverified claims.

## Required Codex final report

Report concisely:

- Architecture implemented and any deviations from this handoff.
- Exact melonDS revision and license treatment.
- Files added or changed, including whether anything under `third_party/melonDS` differs from upstream.
- Exact commands executed and their actual results.
- Artifact paths and architecture.
- Acceptance criteria verified versus unverified.
- Actual local-wireless test content and provenance, if used.
- RetroArch and Steam Deck results, kept separate.
- Known failures, compatibility gaps and technical debt.
- Final Git status and commits created.
- The next smallest concrete task if anything remains incomplete.

Do not report success for behavior that was only code-reviewed, mocked, inferred, or compiled. Engine build, two-instance execution, touch routing, local wireless exchange, save persistence, RetroArch loading and Steam Deck behavior are separate verification claims.
