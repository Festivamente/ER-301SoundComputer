# Third-party build dependencies

ER-301 Sound Computer uses pinned third-party dependencies so release builds do not
change when a system package manager updates.

Pinned source trees currently include:

- SDL2 2.32.10
- SDL2_ttf 2.24.0 (including its pinned FreeType source)
- FFTW 3.3.11

`install/<platform>/` contains the minimal target prefix used by the plugin build:
headers, static libraries, and the relocatable `sdl2-config` helper where required.
Generated pkg-config/CMake/libtool metadata is deliberately omitted because it is not
used by this build and tends to embed machine-specific absolute paths.

Windows SWIG 4.4.1 is retained under `tools/swigwin-4.4.1/` with only the executable,
runtime library, and licensing/readme material needed to run it. macOS/Linux builds use
their normal host SWIG executable.

Third-party software retains its own licenses. See
`THIRD_PARTY_NOTICES.md`.

## Rebuilding the macOS static prefixes

The generated macOS static archives must use the same deployment floor as the
Rack target (macOS 11.0 for arm64, 10.9 for x86_64). From the repository root, run:

```sh
make third-party-macos-rebuild
```

The helper builds only from the pinned sources in `third-party/src/`, is
single-threaded by default, prefers Apple's standalone Command Line Tools over
the Xcode application bundle, and writes a small `.er301-build-meta` record into
the target prefix. Set `ER301_BUILD_JOBS=N` only on a machine where additional
compile load is acceptable.

## Rebuilding the Linux x86_64 static prefix

On the Linux x86_64 validation machine, run from the repository root:

```sh
make third-party-linux-rebuild
```

The helper builds SDL2, SDL2_ttf/FreeType, and FFTW only from the pinned source
trees, installs static/PIC artifacts into `install/linux-x64/`, retains the
relocatable `sdl2-config` needed for SDL2's normal Linux system-library flags,
and runs `make third-party-linux-audit` before reporting success. Release builds
refuse to build the ER-301 host archive until this audit passes, preventing an
accidental fallback to system SDL/FFTW/FreeType libraries.
