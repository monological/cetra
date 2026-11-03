# Building

The build, the compiler flags, and using the engine from another project.

## Build System

```bash
./build.sh        # Build library + all apps (CMake + Ninja)
```

The build script runs CMake to configure and Ninja to build. Output goes to `out/`
(`out/bin/<app>`).

**Using the engine from another project** (spec 11.105): `add_subdirectory` on a git submodule,
or FetchContent. `README.md`, "Using cetra in your project", has the CMakeLists. There is no
install; the comment at the end of `cetra/CMakeLists.txt` says why the old install rule was
deleted. Three lines make the out-of-tree build work, and each could be mistaken for its
neighbour: the output directories use `PROJECT_BINARY_DIR`, which is `out/` in this tree and the
engine's own subdirectory of a parent's build (the draco include path stays `CMAKE_BINARY_DIR`,
because draco writes there); `CETRA_BUILD_APPS` defaults to `PROJECT_IS_TOP_LEVEL`; and
`shader_strings.h` is included only from `program.c`. To check the path, build a throwaway
consumer in a scratch directory: the README's CMakeLists with `add_subdirectory` pointed at the
checkout, plus a `main.c`. Then look for `build/bin` or `build/lib` appearing, and for the
generated header in `ninja -t deps` for the consumer's object. On this Mac: 669 steps, 36 s cold.

**clangd / editor diagnostics:** the compilation database lands in `out/`, which is
neither place clangd looks (beside the file, its ancestors, or a `build/` child), so a
committed `.clangd` at the repo root points it there. Without it clangd guesses flags,
fails to find `cglm/cglm.h`, turns every `vec3` into an unknown type, and buries the real
diagnostics under an error-limit cutoff — a wall of red that says nothing about the code.
If you see that, check `.clangd` survived before believing any of it. The BUILD is
unaffected either way and always was.

**Compiler:** clang with `-Wall -g -std=gnu11 -pthread`, plus `-DGL_SILENCE_DEPRECATION` **on
macOS only** (`CMakeLists.txt:239`, a `PLATFORM_ID:Darwin` generator expression — the same
scoping trick as the define below, and for the same reason).
`CMAKE_C_STANDARD 11` leaves `C_EXTENSIONS` at its default of ON, so the emitted
flag is `gnu11`, not `c11` -- GNU extensions are available.

The `cetra` target compiles with **`-D_POSIX_C_SOURCE=200809L` and `-D_DEFAULT_SOURCE` on LINUX
ONLY**, through a `$<$<PLATFORM_ID:Linux>:...>` generator expression
(`cetra/CMakeLists.txt:109-111`). The scoping is the thing to know, because the define does real
damage where it is not wanted: on macOS it pins `__DARWIN_C_LEVEL` to the POSIX level and hides
the Darwin extensions -- BSD types like `u_int` (which `<sys/sysctl.h>` is declared in terms of)
and constants like `_SC_NPROCESSORS_ONLN` stop being declared at all. That is why it is not set
there. `util.c` still defines `_DARWIN_C_SOURCE` before its first include for `get_cpu_cores`, but
that is now belt-and-braces against the define coming back rather than a fix for anything the
build currently does. Note the failure mode is an "undeclared identifier" compile error -- and
wrapping the call in `#if defined(...)` turns that error into a silently wrong answer at runtime.
