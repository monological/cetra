# Building

The build, the compiler flags, editor diagnostics, and using the engine from another project.

## Build System

```bash
./build.sh        # library and every app, Debug, into out/
./build.sh -r     # Release, into out/release/
./build.sh -c     # reconfigure from scratch first
```

`build.sh` is a thin wrapper over `CMakePresets.json`: the presets own the generator (Ninja), the
build type and the binary directory. Apps land in `out/bin/<app>` and load their assets relative
to the repository root. `--target <macos|linux|windows>` syncs the working tree to that OS's build
VM and builds there; `docs/build-vms.md` has the setup.

Every dependency is vendored under `cetra/src/ext/` and built from source as a static library
(README, "Dependencies").

## Compiler

- **Flags**: clang with `-Wall` on macOS and Linux, clang-cl with `/W4` on Windows, from one
  interface target (`cetra_warnings`) the library and every app share. Debug or Release comes from
  the preset.
- **The C dialect is `gnu11`.** `CMAKE_C_STANDARD 11` leaves `C_EXTENSIONS` at its default of ON,
  so GNU extensions are available.
- **`GL_SILENCE_DEPRECATION` is defined on macOS only**, through a `$<PLATFORM_ID:Darwin>`
  generator expression in the root `CMakeLists.txt`.

**`_POSIX_C_SOURCE=200809L` and `_DEFAULT_SOURCE` are defined on the `cetra` target on Linux only**,
through a `$<PLATFORM_ID:Linux>` generator expression in `cetra/CMakeLists.txt`. The scoping is the
thing to know:
- On macOS the define pins `__DARWIN_C_LEVEL` to the POSIX level and hides the Darwin extensions:
  BSD types like `u_int`, which `<sys/sysctl.h>` is declared in terms of, and constants like
  `_SC_NPROCESSORS_ONLN` stop being declared.
- Set globally, it breaks the JoltC build (`alloca` undeclared).
- The failure is an "undeclared identifier" compile error. Wrapping the call in `#if defined(...)`
  to silence it turns the error into a wrong answer at runtime.

`util.c` also defines `_DARWIN_C_SOURCE` before its first include, for `get_cpu_cores`, as a guard
against the define reaching macOS.

## Editor diagnostics

The compilation database lands in `out/`, which is neither place clangd looks (beside the file,
its ancestors, or a `build/` child), so a committed `.clangd` at the repository root points it
there. Without it clangd guesses flags, cannot find `cglm/cglm.h`, makes every `vec3` an unknown
type, and buries the real diagnostics under its error limit. If you see that, check `.clangd` is
still there before believing any of it. The build itself does not depend on it.

## Using cetra from another project

Add the engine with `add_subdirectory` on a git submodule, or with FetchContent; README, "Using
cetra in your project", has the CMakeLists. There is no install rule: the comment at the end of
`cetra/CMakeLists.txt` says what a working one would need.

Three things make the out-of-tree build work, and each is easy to undo by accident:
- **The output directories use `PROJECT_BINARY_DIR`**, which is `out/` here and the engine's own
  subdirectory of a parent's build. With `CMAKE_BINARY_DIR` every library and app lands in the
  parent's `lib/` and `bin/`. The draco include path is the exception and stays `CMAKE_BINARY_DIR`,
  because draco writes its generated header there.
- **`CETRA_BUILD_APPS` defaults to `PROJECT_IS_TOP_LEVEL`**, so a consumer does not build the
  example apps.
- **`shader_strings.h` is included only from `program.c`**, so a consumer's objects do not depend
  on the generated header (`docs/shaders.md`).

To check the path, build a throwaway consumer in a scratch directory: the README's CMakeLists with
`add_subdirectory` pointed at the checkout, plus a `main.c`. Then check that no `build/bin` or
`build/lib` appears, and that the generated header is absent from `ninja -t deps` for the
consumer's object. A cold build is about 670 steps.
