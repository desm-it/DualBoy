# Third-party software

DualBoy uses exact Git submodule commits. A normal build must not fetch a branch
tip. Initialize the recorded objects with `git submodule update --init --recursive`.

## Pins verified 2026-09-07

| Component | Source and exact commit | License | Use and local changes |
| --- | --- | --- | --- |
| SameBoy | <https://github.com/LIJI32/SameBoy> at `213a12ce93d66b105a113debd9396306066a7cfc` | Expat (MIT-style), copyright Lior Halphon and contributors; see `third_party/sameboy/LICENSE` | The adapter compiles the internal `Core` API into DualBoy. SameBoy's proven two-instance behavior in `libretro/libretro.c` is a reference. The submodule is unmodified. |
| mGBA PR #318 snapshot | <https://github.com/libretro/mgba> at `fa743c965939f091350df094f57e639933bc17e3` | MPL-2.0; see `third_party/mgba/LICENSE` | This is the PR head, not a released mGBA revision. DualBoy links mGBA internal APIs; its adapted copy of the PR's SIO lockstep and all ownership code live under `src/engines/mgba/`. The submodule is unmodified. |
| Libretro API header | <https://github.com/libretro/libretro-common> at `f0173cc9c0d354c0a97ab4c4d075dc560ccc5988` | MIT notice embedded at the top of `include/libretro.h` (the grant applies to that header, not automatically to other files in the repository) | Only `include/libretro.h` is used as the canonical ABI header. The submodule is unmodified. |

The canonical mGBA upstream, <https://github.com/mgba-emu/mgba>, was separately
inspected at `aa6394c93ca24b0ff40dc9231bdb0533f415e0be`. It is not the build pin because
PR #318 was written and tested against the libretro fork snapshot above.

### mGBA PR status and base

GitHub reported PR <https://github.com/libretro/mgba/pull/318> as open, unmerged,
non-draft, and `mergeable_state: dirty` on 2026-09-07. Its head remained the pin
above; its base was `c758314a639aa0066e7b65a8341448181b73c804` and the fork's then-current
`master` was `e31759b24e7a4e3899285ff720d7b573ac328ae7`. Consequently, its multiplayer
files are not assumed to apply cleanly to current mGBA.

## License compatibility

DualBoy-authored source is licensed under MPL-2.0. mGBA source and any source file
derived from PR #318 stay under MPL-2.0. SameBoy remains under its Expat license,
which permits combination and redistribution when its notice is retained. The
Libretro API header retains its embedded MIT notice. No GPL-only emulator source
is used.

Binary distributors must satisfy MPL-2.0 source-availability and notice terms and
must include SameBoy's notice. This file is an engineering inventory, not legal
advice.

## Update procedure

1. Read upstream changelogs, licenses, and the complete diffs from the old pins.
2. Re-check PR #318's API compatibility, open/merge state, head, base, and license.
3. Create a dedicated branch and move one gitlink at a time with detached HEADs.
4. Keep submodules pristine (`git submodule foreach --recursive git status --short`).
5. Rebuild generated SameBoy boot data from the pinned assembly if it changed.
6. Run unit tests, paired engine tests, sanitizers, the Linux x86-64 container build,
   exported-symbol check, and RetroArch smoke test before recording a new pin.
7. Update this inventory and `docs/status.md` in the same commit as each gitlink.
