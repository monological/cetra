# spores

A cordyceps spore room: a dark interior lit by one directional key, filled with pale green motes on
curl-noise turbulence, soft and lit through the shadows, contained by the room and pushed aside by
a glass sphere that wanders through them. It is the test bed for the general particle system
(`docs/game-framework.md`, Particle System): the system is attached to a scene node, and the engine
ticks and draws it. It runs on the game loop with no physics.

```bash
./out/bin/spores
./out/bin/spores -x -f 300 -S /tmp/spores.ppm
```

A drag orbits the camera. G shows the debug GUI, C the camera HUD, L the light gizmos.

Flags:
- `-x`, `-f N`, `-S PATH`, `--screenshot-every N`: the headless capture set.
- `--cpu`: the CPU sim backend in place of the GPU transform-feedback one.
- `--transform-probe N`: prints the previous and current pose of every named node every N frames.
  The sphere is driven by the game clock with no wall clock in it, which makes it the one rigid
  scene-graph mover in the apps that is also deterministic headless.
- `--taa`, `--headless-jitter`, `--msaa N`: the anti-aliasing switches.

It keeps four MSAA samples and no TAA, and that is a decision: the particle pass writes no motion
vector, so under TAA every mote reprojects through the geometry behind it and becomes a dash.
Giving the particle pass a motion vector is what would change that; the flags stay for that day.
