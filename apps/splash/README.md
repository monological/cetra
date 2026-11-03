# splash

A test of the SDF text renderer (`text.h`): "C E T R A" over "ENGINE", in Silkscreen at 128 px,
under the plasma text effect.

```bash
./out/bin/splash
```

Run it from the repository root, since it loads `apps/splash/assets/Silkscreen-Regular.ttf` by that
relative path. Escape quits. It takes no flags.

It does not use `engine_run`: it clears and swaps the default framebuffer in its own loop, so the
engine's scene target, post chain, anti-aliasing settings and screenshot path never reach it, and
it has no headless mode.
