# DualBoy agent guide

DualBoy is one Libretro frontend that owns exactly two emulator instances. GB/GBC
uses SameBoy; GBA uses mGBA plus the cooperative SIO lockstep implementation from
libretro/mgba PR #318; NDS uses two melonDS objects plus upstream same-process
`LocalMP`. Never load another Libretro core from this core.

## Layout and ownership

- `src/libretro/`: the only exported Libretro ABI and frontend callbacks.
- `src/frontend/`: content detection/loading, M3U parsing, composition,
  persistence, options, session ownership, and paired-state container code. It
  must not depend on engine-private structs.
- `src/engines/{sameboy,mgba,melonds}/`: adapters that exclusively own engine
  objects and translate the common pair interface.
- `third_party/`: pinned Git submodules; do not edit them. Put adaptations in
  `src/`.
- `tests/`: unit/integration harnesses and source-generated legal test ROMs.
- `docs/status.md`: source of truth for verified claims and outstanding runtime
  validation.

All partial-load cleanup paths must be idempotent. ROM bytes are immutable and may
be shared; SaveRAM, RTC, video, audio, state, and scheduler data may not be shared.
Only machine 0 audio is emitted in the MVP. RetroArch port 0 maps to machine 0
and port 1 to machine 1. The screen-swap option changes only their presentation
order; it never changes controller routing.

Persistence ownership depends on engine and load mode; do not infer it from a
filename. Read `docs/saves.md` before changing memory IDs, save paths, extents,
or load/unload ordering. In particular, mGBA exposes a stable 128 KiB shadow while
the persisted extent remains unknown until mGBA detects a save device.

## Commands

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
make test
make linux-x86_64
make test-linux-x86_64
make asan-linux-x86_64
make asan-linux-native
make symbols-linux-x86_64
```

The `linux-x86_64` targets use the pinned container described in
`docs/architecture.md`; a host build is a useful diagnostic but is not Linux
validation. On an ARM host, use `asan-linux-native`: an x86-64 ASan process does
not have usable virtual-address-space semantics under this Docker emulation.
There is currently no `format-check` Make target. Run the strict container suite,
native sanitizer suite, and exported-symbol check before a release claim.

## Constraints and current status

- DualBoy-authored source is MPL-2.0. Preserve SameBoy's Expat notice and the
  Libretro header's MIT notice; mGBA and PR-derived files remain MPL-2.0.
  melonDS is GPL-3.0-or-later, so the statically linked combined binary and its
  distribution are governed by GPLv3-compatible terms; keep installed notices
  and `THIRD_PARTY.md` synchronized with the build.
- Update dependency pins only with the audit procedure in `THIRD_PARTY.md`.
- Do not use commercial ROMs. Test content must be generated from source or carry
  an explicit redistributable license.
- Do not claim real RetroArch, Steam Deck, or commercial-game validation until it
  is actually performed and recorded in `docs/status.md` with the
  command/device and result.
- GB/GBC/GBA and NDS support normal/subsystem/M3U loading, options, independent
  persistence, and Libretro ABI integration. Paired states remain available for
  GB/GBC/GBA; NDS manual states, rewind, and runahead are unsupported because
  `LocalMP` transport state is not serializable. Validation claims and remaining
  real-RetroArch and Steam Deck gates remain governed by `docs/status.md`.
- Preserve the documented SameBoy subsystem RTC caveat until a real frontend is
  shown to persist both custom RTC memory IDs independently.
