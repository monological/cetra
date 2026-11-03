# tree

Procedural tree generator with ImGui sliders, on a domed island in a sea with a seabed under it, at sunset, walkable in first person (`--player`); `--no-water` for dry land. Specs 11.32, 11.35, 11.36. Since 13.38 every preset is in the panel's Preset combo, and each form has its own sliders -- the recursive ones for broadleaf and dead, crown, whorls and sprays for spruce, fir and snag; a conifer takes no season and drops no leaves

Headless: yes (but NOT frame-deterministic on the orbit path: floor is 9k-31k px depending on framing, see `docs/verification.md`)
