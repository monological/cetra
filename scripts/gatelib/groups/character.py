"""The character group: a character's capsule changed while it stands (crouch, stand, feet; spec
13.48).
"""

import re

from gatelib.probes import _gametest_probe, _probe_verdicts


CHAR_FEET_TOL = 1e-4    # m

_CHARACTER_PROBE = re.compile(r"^character (\S+) (\S+) (\S+) (.*)$", re.M)


def _character_probe(case):
    """{(label, key): [floats]} from one gametest --character-probe run, or None. Real
    controllers in a physics world, stepped as a game steps them, and no window."""
    return _gametest_probe("--character-probe", _CHARACTER_PROBE, case)


def run_character_gate(workdir):
    """A character's capsule changed while it stands (spec 13.48), stepped in a physics world.

    The probe makes capsules of silent's size, 1.7 m standing and 1.15 crouched, on a floor with
    a beam across it whose underside is 1.4 m up, and walks them at it; and it shrinks and grows
    one on the floor clear of the beam. A crouch shortens the capsule from the top and leaves its
    foot where it was, so the controller's position never moves for it. The probe prints its
    fixture -- the capsule's radius, padding and two heights, and the beam's faces -- which the
    arms read rather than restate.

      character-crouch-under   walked at the beam, the standing capsule stops with its centre a
                               radius and the padding short of the near face, and the crouched
                               one comes out past the far face by more than a radius.
      character-stand-refused  as the crouched capsule's middle passes under the beam's, standing
                               is refused and it stays crouched; past the beam it stands.
      character-feet-kept      walking on open floor through a crouch and a stand, the position
                               and the ground under it hold to a tenth of a millimetre and the
                               body is never off the ground: the shorter capsule's foot is where
                               the taller one's was.
    """
    failed = []
    verdict = _probe_verdicts(_character_probe, failed)

    def under(r):
        radius, padding, _, _ = r[("capsule", "size")]
        face, back = r[("beam", "z")]
        standing = r[("standing", "reach")][0]
        crouched = r[("crouched", "reach")][0]
        stop = face - radius - padding
        ok = (int(r[("crouch", "set")][0]) == 1 and abs(standing - stop) < 0.01
              and crouched > back + radius)
        return ok, (f"standing stopped at z {standing:.4f} (want {stop:.4f}, the near face less a "
                    f"radius and the padding); crouched reached {crouched:.4f} (want past "
                    f"{back + radius:.4f})")

    def refused(r):
        _, _, stand, crouch = r[("capsule", "size")]
        under_stood, under_half, at = r[("under", "stood")]
        out_stood, out_half = r[("out", "stood")]
        ok = (int(under_stood) == 0 and abs(under_half - crouch) < 1e-6
              and int(out_stood) == 1 and abs(out_half - stand) < 1e-6)
        return ok, (f"under the beam at z {at:.3f} standing was "
                    f"{'allowed' if under_stood else 'refused'}, half-height {under_half:.3f}; "
                    f"past it {'allowed' if out_stood else 'refused'}, half-height "
                    f"{out_half:.3f} (want refused at {crouch:g}, allowed at {stand:g})")

    def feet(r):
        legs = ("stand", "crouch", "rise")
        sets = all(int(r[(leg, "set")][0]) == 1 for leg in legs)
        ys = {leg: r[(leg, "y")] for leg in legs}
        drift = max(abs(ys[leg][0] - ys["stand"][0]) for leg in legs)
        ground = max(abs(ys[leg][1] - ys["stand"][1]) for leg in legs)
        air = int(r[("air", "steps")][0])
        ok = sets and air == 0 and drift < CHAR_FEET_TOL and ground < CHAR_FEET_TOL
        return ok, (f"through a crouch and a stand the centre moved {drift * 1000:.4f} mm and the "
                    f"ground under it {ground * 1000:.4f} mm (want under "
                    f"{CHAR_FEET_TOL * 1000:g}); off the ground {air} step(s) (want 0); every "
                    f"change {'made' if sets else 'refused'}")

    ok, said = verdict("character-crouch-under", "beam", under)
    print(f"  character-crouch-under {'PASS' if ok else 'FAIL'}  {said}")
    ok, said = verdict("character-stand-refused", "beam", refused)
    print(f"  character-stand-refused {'PASS' if ok else 'FAIL'}  {said}")
    ok, said = verdict("character-feet-kept", "floor", feet)
    print(f"  character-feet-kept {'PASS' if ok else 'FAIL'}  {said}")
    return failed
