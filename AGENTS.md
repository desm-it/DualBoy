# DualBoy agent guide

DualBoy is one Libretro frontend that owns exactly two emulator instances. GB/GBC
uses SameBoy; GBA uses mGBA plus the cooperative SIO lockstep implementation from
libretro/mgba PR #318. Never load another Libretro core from this core.

## Layout and ownership

- `src/libretro/`: the only exported Libretro ABI and frontend callbacks.
- `src/frontend/`: content detection/loading, composition, persistence, options,
  playlists, and paired-state container code. It must not depend on engine-private
  structs.
- `src/engines/{sameboy,mgba}/`: adapters that exclusively own engine objects and
  translate the common pair interface.
- `third_party/`: pinned Git submodules; do not edit them. Put adaptations in `src/`.
- `tests/`: unit/integration harnesses and source-generated legal test ROMs.

All partial-load cleanup paths must be idempotent. ROM bytes are immutable and may
be shared; SaveRAM, RTC, video, audio, state, and scheduler data may not be shared.
Only machine 0 audio is emitted in the MVP. Port 0 maps to left/top machine 0 and
port 1 to right/bottom machine 1 unless the single swap option changes both.

## Commands

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
make linux-x86_64
make test-linux-x86_64
```

The `linux-x86_64` targets run in the pinned container described in
`docs/architecture.md`; a host build is useful but is not Linux validation. Run
`make format-check`, `make test`, and sanitizers before release commits when those
targets are available.

## Constraints and status

- Root/DualBoy source is MPL-2.0. Preserve SameBoy's Expat and Libretro header's
  MIT notices; mGBA and PR-derived files remain MPL-2.0.
- Update dependency pins only with the audit procedure in `THIRD_PARTY.md`.
- Do not use commercial ROMs. Test content must be generated from source or have
  an explicit redistributable license.
- `docs/status.md` is the source of truth for verified versus inferred behavior.
- Current status: phases 1-2 are buildable; the SameBoy adapter is in progress. No
  emulator functionality is claimed until recorded in status with exact test
  output.
