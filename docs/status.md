# Implementation status

Last updated: 2026-09-07.

## Verified

- The repository began as an initialized, commitless Git worktree containing only
  the authoritative handoff.
- SameBoy, mGBA PR #318, canonical mGBA, Libretro, and their license texts were
  inspected at the exact revisions recorded in `THIRD_PARTY.md`.
- GitHub's API and pull refs confirmed PR #318 is open and its head has not moved
  from the handoff's known `fa743c9` snapshot. GitHub reports merge conflicts.
- Docker can execute a `linux/amd64` Debian container on the ARM64 development host.
- Phase 1's minimal core built as an ELF64 x86-64 shared object with GCC 12.2.0.
  `ctest --test-dir build-linux-x86_64 --output-on-failure` passed its Libretro ABI
  smoke test (1/1), and `make symbols-linux-x86_64` confirmed that all mandatory
  ABI symbols are present and no non-`retro_*` symbol is publicly exported.
- The phase-1 artifact at `build-linux-x86_64/dualboy_libretro.so` was identified by
  `file` as `ELF 64-bit LSB shared object, x86-64`. This is only a core-load/ABI
  bootstrap artifact; it does not yet contain emulator integration.
- Phase 2's engine-neutral ownership ABI, Nintendo-logo/header checksum detector,
  family compatibility rules, native-pixel compositor, player-only modes, and
  coupled screen/controller swap are covered by `frontend_unit`. The strict Linux
  x86-64 build passed 2/2 CTest tests after these additions.

## In progress

- Phase 3: SameBoy dual-instance adapter, normal same-ROM loading, link stepping,
  input/video/audio plumbing, and independent persistence.

## Not yet claimed

No emulator load, emulated video, controller routing, link, audio, persistence,
save-state, sanitizer, real RetroArch, or Steam Deck behavior is claimed yet.
Entries move to Verified only with a recorded command and result.

## Environment facts

The development host is macOS ARM64 and has Docker with a Linux ARM64 daemon that
can emulate `linux/amd64`. Steam Deck hardware is not available. Native host builds
are useful diagnostics but cannot satisfy the Linux x86-64 acceptance criterion.
