# Third-party software

DualBoy uses exact Git submodule commits. A normal build must not fetch a branch
tip. Initialize the recorded objects with `git submodule update --init --recursive`.

## Pins verified 2026-09-08

| Component | Source and exact commit | License | Use and local changes |
| --- | --- | --- | --- |
| SameBoy | <https://github.com/LIJI32/SameBoy> at `213a12ce93d66b105a113debd9396306066a7cfc` | Expat (MIT-style), copyright Lior Halphon and contributors; see `third_party/sameboy/LICENSE` | The adapter compiles the internal `Core` API into DualBoy. SameBoy's proven two-instance behavior in `libretro/libretro.c` is a reference. The submodule is unmodified. |
| mGBA PR #318 snapshot | <https://github.com/libretro/mgba> at `fa743c965939f091350df094f57e639933bc17e3` | MPL-2.0; see `third_party/mgba/LICENSE` | This is the PR head, not a released mGBA revision. DualBoy links mGBA internal APIs; its adapted copy of the PR's SIO lockstep and all ownership code live under `src/engines/mgba/`. The submodule is unmodified. |
| melonDS | <https://github.com/melonDS-emu/melonDS> at `906e9ebb27da8c6a715cd7abab4abfe8a8d29427` (tree `60c6724f8695bdbe5e21c4367b18293f7f811fc2`) | GPL-3.0-or-later; see `third_party/melonDS/LICENSE` and the license declaration in `third_party/melonDS/README.md` | DualBoy builds upstream's static `core` with the regular OpenGL renderer enabled and the desktop frontend, GDB stub, JIT, release LTO, and embedded build metadata disabled. Because upstream does not put `src/net/LocalMP.cpp` or its generated GLAD loader in `core`, those exact untouched sources are compiled into separate internal archives. The byte-clean submodule remains pinned; the build substitutes one generated, hash-verified adaptation of `GPU2D_OpenGL.cpp`, described below. All other platform and pair adaptation is under `src/engines/melonds/`. |
| Libretro API header | <https://github.com/libretro/libretro-common> at `f0173cc9c0d354c0a97ab4c4d075dc560ccc5988` | MIT notice embedded at the top of `include/libretro.h` (the grant applies to that header, not automatically to other files in the repository) | Only `include/libretro.h` is used as the canonical ABI header. The submodule is unmodified. |

The melonDS gitlink matched both upstream `HEAD` and `refs/heads/master` when
rechecked on 2026-09-08. That observation records the audit point; updates must
still select and review an exact commit rather than following a branch.

The pinned `GPU2D_OpenGL.cpp` allocates two CPU vertex arrays per
`GLRenderer2D` but does not release them. DualBoy's
`src/engines/melonds/adapt_gpu2d_opengl.cmake` verifies the normalized source as
SHA-256 `3f77171742446c39a9d10ce9a38af2e0910fedbcbe34116fa9ff702cccb75eb3`,
then generates a build-tree copy that initializes and deletes those two arrays.
The original source is not compiled, the submodule stays unchanged, and the
generated copy retains melonDS's GPL-3.0-or-later header. A source distribution
must include the pin, transformer, and build instructions. Remove this boundary
only after auditing an upstream pin containing an equivalent lifetime fix.

The canonical mGBA upstream, <https://github.com/mgba-emu/mgba>, was separately
inspected at `aa6394c93ca24b0ff40dc9231bdb0533f415e0be`. It is not the build pin because
PR #318 was written and tested against the libretro fork snapshot above.

### mGBA PR status and base

GitHub reported PR <https://github.com/libretro/mgba/pull/318> as open, unmerged,
non-draft, and `mergeable_state: dirty` on 2026-09-08. Its head remained the pin
above; its base was `c758314a639aa0066e7b65a8341448181b73c804` and the fork's then-current
`master` was `e31759b24e7a4e3899285ff720d7b573ac328ae7`. Consequently, its multiplayer
files are not assumed to apply cleanly to current mGBA.

The GPL-3.0-or-later
[`JesseTG/melonds-ds`](https://github.com/JesseTG/melonds-ds) frontend was
inspected at `bc4e4b67d2d470d7c682810a1e892cafd6f9082b` only as an integration-pattern
reference. It is not a production dependency, and none of its Libretro
entrypoint is compiled or copied into DualBoy.

## License compatibility

DualBoy-authored source files remain under MPL-2.0 and do not carry the MPL
"Incompatible With Secondary Licenses" notice. mGBA source and files derived
from PR #318 stay under MPL-2.0. SameBoy remains under its Expat license, which
permits combination and redistribution when its notice is retained. The
Libretro API header retains its embedded MIT notice.

melonDS is GPL-3.0-or-later. Because it is statically linked into
`dualboy_libretro`, the combined executable work and a binary distribution
containing it must be conveyed under GPLv3-compatible terms and with complete
corresponding source/build material as applicable. This does not erase the
file-level MPL-2.0, Expat, or MIT grants on their respective sources. The selected
melonDS core also compiles its bundled Teakra (MIT), FatFs (BSD-like one-clause),
FreeBIOS replacement (BSD-2-Clause-style), blip-buf (LGPL-2.1-or-later),
xxHash (BSD-2-Clause), Steve Reid SHA-1 (public domain), and tiny-AES-c (public
domain/Unlicense) sources. Their notices are preserved in the submodule and
installed with binary packages; see `NOTICE`.

The selected OpenGL build also compiles melonDS's GLAD 0.1.36 generated
`glad.c`/`glad.h` unchanged. GLAD's official license FAQ documents generated
output as available under Public Domain, WTFPL, or CC0 terms and cautions that
Khronos-specification-derived portions may be Apache-2.0; the generated files
themselves do not select one exclusive term. Binary packages install a
provenance notice and the full Apache-2.0 text. The included `khrplatform.h`
carries its own Khronos permissive notice, retained in source and installed as
a separate file.

Binary distributors must include the installed notices and license texts and
satisfy GPLv3 source-availability requirements in addition to the component
notices. This file is an engineering inventory, not legal advice.

## Update procedure

1. Read upstream changelogs, licenses, and the complete diffs from the old pins.
2. Re-check PR #318's API compatibility, open/merge state, head, base, and license.
3. For melonDS, verify the proposed commit and tree, review its root license plus
   every bundled source license selected by `core`, and re-check whether
   `LocalMP.cpp` is part of that target. Re-audit the `GPU2D_OpenGL.cpp`
   adaptation and remove or update it only when the pinned source changes.
4. Create a dedicated branch and move one gitlink at a time with detached HEADs.
5. Keep submodules pristine (`git submodule foreach --recursive git status --short`)
   and put all integration changes outside `third_party/`.
6. Rebuild generated SameBoy boot data from the pinned assembly if it changed.
7. Reconfigure from an empty build tree so cached upstream feature selections
   cannot hide new defaults, then run paired engine tests, sanitizers, the Linux
   x86-64 container build, exported-symbol check, install-tree check, and a real
   RetroArch smoke test before recording a new pin.
8. Update this inventory, installed license list, and `docs/status.md` in the same
   commit as each gitlink.
