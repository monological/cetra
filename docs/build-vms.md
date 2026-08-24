# The build VMs

`./build.sh --target linux|windows` syncs the working tree to a VM and builds it there, so any of
the three platforms can be built from any host. This is how that side is set up, and why each piece
is needed.

## What they are for, and what they are not

They exist to answer **does this still compile and link on that platform**. They are not a second
place to run `gates.py` or `goldens.py`: both are macOS-calibrated instruments, the goldens are
committed PNGs baked from a macOS debug build, and the software rasterizers below are a different
renderer entirely. A green build here plus a green suite on macOS is the coverage; a suite run here
would produce numbers that compare nothing.

Rendering still matters on them, though, for the reason the next section exists.

## A crash on these machines is not necessarily a VM problem

Neither guest has a GPU (the KVM host does; nothing is passed through), so for a long time both
"failed to run" and that was written off as missing graphics. It was hiding a real bug in the
engine, on every x86-64 machine in the world:

Jolt's `USE_AVX2` and friends default ON and inject `-mavx2` into the **global** compiler flags, so
they reach every C file, not just Jolt's. `__AVX__` then makes cglm's `mat4` `CGLM_ALIGN(32)` with
`_mm256_store_ps` copies, while `Engine`, `Scene` and every node come from `calloc`, which
guarantees 16 on glibc. The result is a SIGSEGV in `glm_mat4_identity` inside `create_engine` —
before any GL call, which is what made it look like a graphics failure. See the ISA `foreach` in the
root `CMakeLists.txt`.

The lesson worth keeping: **make the VM able to render before concluding that it cannot.** Installing
a software rasterizer is what turned "no GPU" into a backtrace.

## Addressing

The three hosts are `cetra-host`, `cetra-linux` and `cetra-win`, defined in `~/.ssh/config` and
deliberately not in the repo — the addresses and the jump through the KVM host are local
configuration. `build.sh` reads the alias names from `CETRA_MACOS_SSH` / `CETRA_LINUX_SSH` /
`CETRA_WIN_SSH`, so CI can point them elsewhere without editing anything.

The sync drops `.git`, so the remote tree has no history to derive a version from and
`CMakeLists.txt` refuses to configure. `build.sh` writes a `VERSION` file onto the remote after the
transfer, which is the escape hatch that file's own error message names.

## Linux (`cetra-linux`)

The guest has **no display adapter at all** — `lspci` lists none, so there is no `/dev/dri`, no DRM
module and no display server. Mesa's software rasterizer covers it:

```bash
sudo apt-get install -y xvfb mesa-utils gdb
```

Run anything that opens a window under a virtual framebuffer:

```bash
xvfb-run -s '-screen 0 1280x1024x24' ./out/bin/render -m assets/c64.fbx -x -f 2 -S /tmp/f.ppm
```

`llvmpipe` reports **GL 4.5 core**, comfortably above the 4.1 the engine asks for; check with
`xvfb-run glxinfo -B`. `mesa-utils` is only for that check, `gdb` only for backtraces — but both earn
their place, since a crash here is otherwise a bare exit code with an empty log (stderr is
block-buffered into a file and dies unflushed with the process).

The alternative to software GL is giving the guest `virtio-gpu`, or passing the host's Intel UHD 770
through. Neither is set up; llvmpipe is enough to prove a binary runs.

## Windows (`cetra-win`)

The guest has the **Microsoft Basic Display Adapter**, whose driver exposes **OpenGL 1.1**, so a 4.1
core context request fails outright — cleanly, with `Failed to create GLFW window`. Mesa3D's
drop-in replacement fixes it:

```powershell
curl.exe -sL -o C:/Users/dev/mesa.7z https://github.com/pal1000/mesa-dist-win/releases/download/26.2.0/mesa3d-26.2.0-release-msvc.7z
mkdir C:/Users/dev/mesa
tar.exe -xf C:/Users/dev/mesa.7z -C C:/Users/dev/mesa
cd C:/Users/dev/mesa; cmd /c systemwidedeploy.cmd 1
```

Four things there are not obvious:

- **`tar.exe` reads the `.7z`.** Windows ships bsdtar/libarchive, which handles 7-Zip, so no archiver
  needs installing — worth knowing because `winget install 7zip.7zip` currently fails with a 404 on
  this machine.
- **`release-msvc`, not mingw**, to match the clang-cl toolchain the Windows preset uses.
- **`systemwidedeploy.cmd` takes an argument** and any argument turns on its non-interactive
  "botmode"; `1` is desktop GL. Without one it opens a menu and blocks.
- **System-wide, not per-app, and that is the point.** Per-app deployment means copying DLLs next to
  the executable — into `out/`, which `--clean` wipes — and it needs **three** of them
  (`opengl32.dll`, `libgallium_wgl.dll`, `dxil.dll`). `opengl32.dll` alone is a 136 KB stub that
  loads the others, and missing them is exit `0xC0000135`, which says only "a DLL was not found".

After a system-wide deploy no environment variable is needed; `GALLIUM_DRIVER=llvmpipe` is only for
forcing the software path when a real adapter is also present.

## Checking a VM is working

Build and render one frame. `render` needs a model, so `assets/c64.fbx` is the cheap one:

```bash
./build.sh --target linux      # or --target windows
```

then on the guest, run `render -m assets/c64.fbx -x -f 2 -S <path>` (under `xvfb-run` on Linux) and
confirm the exit code is 0 and the file is non-empty. A frame of the right size can still be black,
so read a few bytes if the answer matters — a real one spans most of the 0-255 range.
