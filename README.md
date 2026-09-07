# DualBoy

DualBoy is an in-development Libretro core that runs two linked GB/GBC or GBA
machines in one RetroArch session. It directly embeds SameBoy and mGBA engine code;
it does not load other Libretro cores.

The authoritative requirements are in `handoff/codex-project-handoff.md`. Verified
functionality and current limitations are tracked in `docs/status.md`. Do not infer
emulator support from the presence of a build artifact until it is listed there.

## Bootstrap build

```sh
git submodule update --init --recursive
make linux-x86_64
make test-linux-x86_64
make symbols-linux-x86_64
```

This builds `build-linux-x86_64/dualboy_libretro.so` in a pinned x86-64 Linux
container. A conventional CMake/Ninja build also works when those tools are present:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

Installation, subsystem, persistence, and Steam Deck instructions will be expanded
as their implementation phases become verified.

## License

DualBoy-authored code is MPL-2.0. SameBoy is Expat-licensed, mGBA is MPL-2.0, and
the Libretro API header carries an MIT notice. See `LICENSE` and `THIRD_PARTY.md`.
