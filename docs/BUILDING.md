# Building ER-301 Sound Computer

## Layout

The Rack SDK is external to the repository. The default layout is:

```text
GitHub/
├── Rack-SDK/
└── VCVRackER-301/
```

Set `RACK_DIR` if your SDK lives elsewhere. The repository root is the Rack plugin directory.

On Windows, use MSYS2's **MinGW 64-bit (`MINGW64`)** shell. Do not build from the default MSYS or UCRT64 shell.

## Target selection

Rack's `arch.mk` supplies the target name. `third-party.mk` maps it to a pinned
dependency prefix:

| Rack target | ER-301 platform | Third-party prefix |
| --- | --- | --- |
| `mac-arm64` | `darwin` | `macos-arm64` |
| `mac-x64` | `darwin` | `macos-x86_64` |
| `win-x64` | `windows` | `windows-x64` |
| `lin-x64` | `linux` | `linux-x64` |

The upstream ER-301 build is invoked with `PROFILE=release`. Generated ER-301 objects
are isolated by target under `er-301-native/release/<target>/`. The plugin build
propagates Rack-compatible macOS deployment floors (10.9 on Intel and 11.0 on Apple
Silicon), selects the pinned Windows SWIG executable, and makes the complete ER-301
host archive a prerequisite of the final Rack plugin so interrupted builds resume
safely.

## Commands

From the repository root:

```sh
make install         # prepare dependencies if needed, build/package/install
make                  # prepare dependencies if needed, then build the plugin
make dist             # prepare dependencies if needed, then create ./dist/*.vcvplugin
make platform-info    # show the selected Rack/ER-301/dependency target
make clean            # remove plugin and target-specific ER-301 build output
make binary-audit     # prepare if needed, then reject accidental runtime deps
```

A fresh checkout does not require a separate dependency command. The public build
entry points prepare the selected platform before the ER-301 engine and final plugin
link. Generated `sdl2-config` metadata is evaluated only when the link step needs it.

Useful regression checks:

```sh
make audio-test
make multi-instance-test
make runtime-check
```

`package-test` additionally requires a native ER-301 package archive supplied as
`PLUCK_PKG=/path/to/package.pkg`.

## Dependencies

Pinned source snapshots live under `third-party/src/`. On macOS and Linux, the selected
`third-party/install/<platform>/` prefix is generated automatically the first time a
public build target needs it, then audited and reused. This first preparation can take a while on slower machines. Vendor/CMake output is captured
during this automatic bootstrap so the normal `make install` transcript stays concise;
if preparation fails, the tail of the full log is printed and the log path is retained.

Windows carries its validated static `windows-x64` prefix plus the pinned SWIG runtime
under `third-party/tools/`, and `make install` audits those inputs before compiling.

Release packages must not acquire runtime dependencies on SDL2, SDL2_ttf, FFTW, or
FreeType from the build machine; run `make binary-audit` on each release target to
verify this explicitly. The explicit `third-party-*-rebuild` targets remain available
for maintainer diagnostics and intentionally show the full vendor build output.

Compiler warnings originating in the Festivamente/VCV integration should be fixed.
Warnings from the upstream ER-301 source or generated/vendor compatibility code are
reviewed separately and are not grounds for modifying upstream merely to make a log
visually quiet. The Intel host build suppresses only the extremely repetitive Clang
`gnu_inline` warning emitted by the bundled neon2sse compatibility header.

The Rack SDK itself is not vendored because it is supplied by VCV Rack's build
environment and changes with Rack.
