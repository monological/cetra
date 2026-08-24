# The build VMs

`./build.sh --target linux|windows` syncs the working tree to a VM and builds it there, so any of
the three platforms can be built from any host. This is how that side is set up, and why each piece
is needed.

## What they are for

Primarily: **does this still compile and link on that platform**. But `gates.py` and `goldens.py`
both run on all three now, and the gate suite travels better than its reputation -- most arms assert
an analytic property (a penumbra width, a shadow term, a ratio between two framings, whether two
runs agree) rather than comparing a stored image, and those are renderer-independent by
construction. `ao-active` reads 28.5% of frame on Apple's driver and 28.5% under llvmpipe, three
significant figures apart on two unrelated rasterizers.

**What makes that work is framebuffer normalisation, and it is worth understanding before reading
any number off these machines.** Every sample coordinate and every stored golden was measured
against a framebuffer TWICE the requested `-W/-H`, because that is what a HiDPI context hands back.
Both scripts now measure the display scale at startup and scale the REQUEST so every platform lands
on that same buffer -- a HiDPI machine multiplies by one and is byte-identical to before, a 1x
machine asks for double. `gates.py` prints which case it is on every run.

Note this was never only a portability question: **a Mac on a 1x external monitor** hit exactly the
same wall, which is why the scale is measured rather than assumed from the OS.

Two things still do not transfer, and should not be read as regressions:

- **Timing arms** are meaningless under a software rasterizer.
- **Goldens compare real pixels now** that dimensions match, so genuine rasterizer differences show
  up as differences. That is the honest outcome, and the corpus must **not** be re-baked off macOS --
  it is the macOS reference, and re-baking it from llvmpipe would destroy it.

**Do not reach for "the rasterizer is different" as an explanation.** Before the framebuffer
normalisation, three shadow arms failed on Linux (`grazing` 0.8364 against a 0.95 floor,
`pillar-pcf` 0.2569 and `pillar-msm` 0.2445 against 0.02) and that was the obvious reading. It was
wrong: all three sample the frame at fixed positions, were reading a half-size buffer, and now pass
at 1.0000, 0.0000 and 0.0095. A wrong-sized frame produces plausible-looking wrong numbers, not
obviously broken ones -- which is exactly how it survives being looked at.

### Tooling both scripts need

They shell out to **`magick`**, the ImageMagick 7 entry point. Windows gets a real version 7 from
`winget install --id ImageMagick.ImageMagick` -- run `winget source reset --force` first if it
answers 404, which is a stale index rather than a missing package. Ubuntu ships version 6, which has
`identify` / `compare` / `convert` as separate binaries and no `magick` at all, so the call dies with
`FileNotFoundError` before any comparison happens. Put this on `PATH` as `magick`:

```sh
#!/bin/sh
case "$1" in
    identify|compare|convert|mogrify|montage|composite|stream)
        tool="$1"; shift; exec "$tool" "$@" ;;
    *)
        exec convert "$@" ;;    # bare "magick in out" is v7 for v6's convert
esac
```

Windows needs one more thing, and without it the run dies inside a `subprocess` reader thread rather
than in anything recognisable: **invoke Python as `python -X utf8`**. `text=True` decodes child
output in the locale encoding, which is cp1252 there, and the render app opens by printing its
`┏┓┏┳┓` banner. UTF-8 mode is a per-invocation switch and needs no change to either script.

Rendering matters on these machines for the reason the next section exists.

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
sudo apt-get install -y xvfb mesa-utils gdb imagemagick
```

Run anything that opens a window under a virtual framebuffer:

```bash
xvfb-run -s '-screen 0 1280x1024x24' ./out/bin/render -m assets/c64.fbx -x -f 2 -S /tmp/f.ppm
```

The gate suite wants a **bigger virtual screen than that**, because on a 1x display it asks for
double the nominal size to land on the framebuffer the arms were calibrated against, and its largest
render is 2400x1500. A screen that cannot hold the window clips it:

```bash
xvfb-run -s '-screen 0 2560x2048x24' python3 scripts/gates.py
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

## Running the suites

Each platform's invocation differs only in what has to be wrapped around it:

```bash
# macOS
python3 scripts/gates.py

# Linux -- a virtual framebuffer, big enough for the doubled request
xvfb-run -s '-screen 0 2560x2048x24' python3 scripts/gates.py

# Windows -- UTF-8 mode, and the preset does not build to out/bin
python -X utf8 scripts/gates.py --bin-dir out/windows-debug/bin
```

The first line of output names the binary directory and the second the framebuffer scale. Both are
part of any result read off this machine, which is why they are printed rather than assumed.

Expect the two guests to be **slow**: on a 1x display the suite asks for double the nominal size,
which is four times the pixels, and llvmpipe draws every one of them on the CPU.
