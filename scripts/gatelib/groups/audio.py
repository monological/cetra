"""The audio group: audio (offline PCM through the spatializer, spec 12.0)."""

import math
import struct
import re
import os

from gatelib.harness import GAMETEST
from gatelib.probes import _gametest_probe_text


# "audio <case> <label> rms <l> <r>" from gametest --audio-probe. This IS the four-field
# shape -- the literal rms sits in the key slot -- so the grammar is not what keeps it
# separate. What does is the return type: this group's eleven index sites read {label:
# (l, r)}, where _gametest_probe yields {(label, key): [floats]}. Folding it in is a
# real simplification and costs re-indexing those eleven; spec 12.5 recorded it rather
# than doing it, since nothing else in that group was being changed.
_AUDIO_PROBE = re.compile(r"^audio (\w+) (\w+) rms (-?[\d.]+) (-?[\d.]+)$", re.M)
# "audio <case> <label> rms <x> bright <y>": a level and a brightness, which the noise and the
# pitch cases print.
_AUDIO_BRIGHT = re.compile(r"^audio (\w+) (\w+) rms (-?[\d.]+) bright (-?[\d.]+)$", re.M)


def _audio_bright(text, case):
    """{label: (rms, bright)} from the rows of `case` in an --audio-probe run's output."""
    return {label: (float(rms), float(bright))
            for c, label, rms, bright in _AUDIO_BRIGHT.findall(text) if c == case}


# A voice's pitch (spec 13.47): the probe plays the sine at 1, unset, and at this.
AUDIO_PITCH_HIGH = 1.06
# The zone arms (spec 13.33): the link the probe sets, how near a level must come to what a
# link, a path or the fade predicts, and the fade's time constant and the probe's window, which
# between them predict every window as the listener steps into the tone's zone.
AUDIO_ZONE_THROUGH = 0.25
AUDIO_ZONE_TOL = 0.01
AUDIO_ZONE_FADE = 0.4      # seconds: audio.h's AUDIO_ZONE_FADE
AUDIO_PROBE_SECONDS = 0.2  # gametest's AUDIO_PROBE_WINDOW at the offline 48 kHz


def _audio_probe_run(case, extra=None):
    """{label: (rms_l, rms_r)} from one gametest --audio-probe run, or None if it
    failed or measured nothing. The probe is a headless offline render (miniaudio's
    noDevice mode), so its PCM is a pure function of the frames pulled -- no device,
    deterministic, and asset-free but for the decode case's synthesized WAV."""
    text, code = _gametest_probe_text("--audio-probe", case, extra=extra)
    out = {label: (float(l), float(rr)) for c, label, l, rr in _AUDIO_PROBE.findall(text)
           if c == case}
    if code != 0 or not out:
        return None
    return out


def _write_sine_wav(path, hz=440.0, seconds=0.5, rate=48000, amp=0.5):
    """A mono 16-bit sine WAV, so the decode arm exercises the file path with no
    committed binary."""
    import wave, struct, math
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(
            struct.pack("<h", int(amp * 32767 * math.sin(2 * math.pi * hz * n / rate)))
            for n in range(int(seconds * rate))))


def run_audio_gate(workdir):
    """The audio path above the device seam (spec 12.0), driven by miniaudio's
    offline noDevice engine so the mixed PCM is deterministic and needs no device.
    No sound is played to a speaker: the probe renders frames and measures them.

      audio-onset    a tone is silent before it is played and energetic after -- an
                     always-on or never-on bug fails.
      audio-pan      a source to the right is louder in the right channel and to the
                     left louder in the left -- a mono or centred mix fails. A ratio
                     each way, so a swapped channel fails too.
      audio-distance the same tone is louder near than far, and still audible far --
                     no attenuation, or a mute past a cutoff, fails.
      audio-master   the master bus at 1 passes energy and at 0 passes silence -- a
                     volume that does not route fails.
      audio-decode   a synthesized WAV loaded from a file decodes to energy -- the
                     file path with no committed asset.
      audio-noise    each colour of procedural noise (spec 13.9) is loud, and the
                     energy of its sample-to-sample difference against its own falls
                     white > pink > brown, with white's near sqrt 2, which is what a
                     flat spectrum gives -- colours swapped or collapsed to one fail.
      audio-zone     a tone in another zone through a link of 0.25 is a quarter of
                     itself with no zones, and a sound decoded from a file and never
                     placed is whole either way, being 2D -- spatialized from the start
                     it would sit at the origin, in the tone's zone (spec 13.33)
      audio-zone-path with a direct link of 0.1 and a way round through a third zone
                     at 0.5 and 0.5, the tone is heard along the best path, 0.25
      audio-zone-start the first update lands a tone at 0.25 whatever its delta, and a
                     tone placed after it is at 0.25 from its first window
      audio-zone-fade the listener stepping into the tone's zone eases it up, each
                     window where AUDIO_ZONE_FADE puts it; a link opened lets it through
                     whole; a voice started in another zone is a quarter of itself
                     from its first window rather than fading down from whole
      audio-pitch    a voice of a sine at pitch 1.06 is 1.06 times as bright as at 1,
                     a sine's brightness being its frequency, and one with its pitch
                     unset is the same as at 1 to the digit (spec 13.47)
      audio-held-pitch a held tone re-pitched to 1.06 while it plays is 1.06 times as
                     bright as before (spec 13.56): audio_sound_set_pitch, what a motor's
                     loop follows its speed by
      audio-load     inside a loading phase, a file that is not there and one that is no
                     audio are both refused, the phase still ends, and a good file loaded
                     beside them plays (spec 13.56) -- miniaudio released the phase's fence
                     twice for a load that failed, and its end asserted

    Falsified by hand at 13.33: audio-zone fails with the zone's gain never applied and
    with a file sound spatialized from the moment it is made, audio-zone-path with the
    direct link taken for the path (0.1), audio-zone-start with the first update easing
    like any other and with a placed sound left unseeded, and audio-zone-fade with the
    easing removed (the first window whole), with the fade twice as fast, and with a
    voice left unseeded.
    """
    if not os.path.exists(GAMETEST):
        print("  audio        SKIP  (gametest not built)")
        return []
    failures = []

    def avg(t):
        return (t[0] + t[1]) / 2.0

    # --- audio-onset -----------------------------------------------------------
    d = _audio_probe_run("onset")
    if not d or "pre" not in d or "post" not in d:
        print("  audio-onset  FAIL  run failed or measured nothing")
        failures.append("audio-onset")
    else:
        pre, post = d["pre"], d["post"]
        ok = max(pre) < 0.001 and min(post) > 0.05
        print(f"  audio-onset  {'PASS' if ok else 'FAIL'}  silent before play (rms {max(pre):.4f}, "
              f"want ~0), energetic after (rms {min(post):.4f}, want > 0.05)")
        if not ok:
            failures.append("audio-onset")

    # --- audio-pan -------------------------------------------------------------
    d = _audio_probe_run("pan")
    if not d or "right" not in d or "left" not in d:
        print("  audio-pan    FAIL  run failed or measured nothing")
        failures.append("audio-pan")
    else:
        rl, rr = d["right"]
        ll, lr = d["left"]
        ok = rr > 3 * rl and ll > 3 * lr and rr > 0.005 and ll > 0.005
        print(f"  audio-pan    {'PASS' if ok else 'FAIL'}  source right -> R/L {rr:.4f}/{rl:.4f}, "
              f"source left -> L/R {ll:.4f}/{lr:.4f} (want the nearer channel dominant each way)")
        if not ok:
            failures.append("audio-pan")

    # --- audio-distance --------------------------------------------------------
    d = _audio_probe_run("distance")
    if not d or "near" not in d or "far" not in d:
        print("  audio-distance FAIL  run failed or measured nothing")
        failures.append("audio-distance")
    else:
        near, far = avg(d["near"]), avg(d["far"])
        ok = near > 3 * far and far > 0.0
        print(f"  audio-distance {'PASS' if ok else 'FAIL'}  near rms {near:.4f} vs far {far:.4f} "
              f"(want near > 3x far, far still audible)")
        if not ok:
            failures.append("audio-distance")

    # --- audio-master ----------------------------------------------------------
    d = _audio_probe_run("master")
    if not d or "on" not in d or "off" not in d:
        print("  audio-master FAIL  run failed or measured nothing")
        failures.append("audio-master")
    else:
        on, off = avg(d["on"]), avg(d["off"])
        ok = on > 0.05 and off < 0.001
        print(f"  audio-master {'PASS' if ok else 'FAIL'}  master 1 rms {on:.4f} (want > 0.05), "
              f"master 0 rms {off:.4f} (want ~0)")
        if not ok:
            failures.append("audio-master")

    # --- audio-decode ----------------------------------------------------------
    wav = os.path.join(workdir, "audio_probe.wav")
    _write_sine_wav(wav)
    d = _audio_probe_run("decode", ["--audio-file", wav])
    if not d or "result" not in d:
        print("  audio-decode FAIL  run failed or did not load the WAV")
        failures.append("audio-decode")
    else:
        energy = avg(d["result"])
        ok = energy > 0.05
        print(f"  audio-decode {'PASS' if ok else 'FAIL'}  decoded WAV rms {energy:.4f} "
              f"(want > 0.05)")
        if not ok:
            failures.append("audio-decode")

    # --- audio-noise -----------------------------------------------------------
    text, code = _gametest_probe_text("--audio-probe", "noise")
    noise = _audio_bright(text, "noise")
    order = [noise.get(c, (0.0, float("nan")))[1] for c in ("white", "pink", "brown")]
    ok = (code == 0 and len(noise) == 3 and all(r > 0.02 for r, _ in noise.values())
          and order[0] > order[1] > order[2] and abs(order[0] - math.sqrt(2.0)) < 0.1)
    detail = ", ".join(f"{c} rms {r:.4f} bright {b:.3f}" for c, (r, b) in noise.items())
    print(f"  audio-noise  {'PASS' if ok else 'FAIL'}  {detail or 'nothing measured'} (want each "
          f"loud, darker white > pink > brown, and white's near sqrt 2)")
    if not ok:
        failures.append("audio-noise")

    # --- audio-zone (spec 13.33) ----------------------------------------------
    d = _audio_probe_run("zone", ["--audio-file", wav])
    if not d or any(k not in d for k in ("flat_bare", "bare", "zoned", "flat_zoned")):
        print("  audio-zone   FAIL  run failed or measured nothing")
        failures.append("audio-zone")
    else:
        through = avg(d["zoned"]) / avg(d["bare"])
        flat = avg(d["flat_zoned"]) / avg(d["flat_bare"])
        ok = (abs(through - AUDIO_ZONE_THROUGH) <= AUDIO_ZONE_TOL
              and abs(flat - 1.0) <= AUDIO_ZONE_TOL)
        print(f"  audio-zone   {'PASS' if ok else 'FAIL'}  through a link of "
              f"{AUDIO_ZONE_THROUGH}: {through:.4f} of the tone with no zones (want "
              f"{AUDIO_ZONE_THROUGH} +- {AUDIO_ZONE_TOL}); a decoded sound never placed "
              f"{flat:.4f} (want 1)")
        if not ok:
            failures.append("audio-zone")

    # --- audio-zone-path --------------------------------------------------------
    d = _audio_probe_run("zone_path")
    if not d or "bare" not in d or "zoned" not in d:
        print("  audio-zone-path FAIL  run failed or measured nothing")
        failures.append("audio-zone-path")
    else:
        through = avg(d["zoned"]) / avg(d["bare"])
        ok = abs(through - AUDIO_ZONE_THROUGH) <= AUDIO_ZONE_TOL
        print(f"  audio-zone-path {'PASS' if ok else 'FAIL'}  {through:.4f} of the tone (want the "
              f"way round, {AUDIO_ZONE_THROUGH}, not the direct link's 0.1)")
        if not ok:
            failures.append("audio-zone-path")

    # --- audio-zone-start -------------------------------------------------------
    d = _audio_probe_run("zone_start")
    if not d or any(k not in d for k in ("bare", "first", "placed")):
        print("  audio-zone-start FAIL  run failed or measured nothing")
        failures.append("audio-zone-start")
    else:
        first = avg(d["first"]) / avg(d["bare"])
        placed = avg(d["placed"]) / avg(d["bare"])
        ok = (abs(first - AUDIO_ZONE_THROUGH) <= AUDIO_ZONE_TOL
              and abs(placed - AUDIO_ZONE_THROUGH) <= AUDIO_ZONE_TOL)
        print(f"  audio-zone-start {'PASS' if ok else 'FAIL'}  after the first update {first:.4f} "
              f"(want {AUDIO_ZONE_THROUGH}, not faded toward it); placed after it {placed:.4f} "
              f"(want {AUDIO_ZONE_THROUGH} from its first window)")
        if not ok:
            failures.append("audio-zone-start")

    # --- audio-zone-fade --------------------------------------------------------
    steps = [f"t{i}" for i in range(10)]
    d = _audio_probe_run("zone_fade", ["--audio-file", wav])
    if not d or any(k not in d for k in ["voice_bare", "bare", "from_a", "opened", "voice"] + steps):
        print("  audio-zone-fade FAIL  run failed or measured nothing")
        failures.append("audio-zone-fade")
    else:
        bare = avg(d["bare"])
        rise = [avg(d[s]) / bare for s in steps]
        # The gain each window holds, set by the update before it: what is left of the step
        # decays by exp(-window / fade) an update.
        want = [1.0 - (1.0 - AUDIO_ZONE_THROUGH)
                * math.exp(-(i + 1) * AUDIO_PROBE_SECONDS / AUDIO_ZONE_FADE) for i in range(10)]
        start = avg(d["from_a"]) / bare
        opened = avg(d["opened"]) / bare
        voice = avg(d["voice"]) / avg(d["voice_bare"])
        ok = (abs(start - AUDIO_ZONE_THROUGH) <= AUDIO_ZONE_TOL
              and all(abs(r - w) <= AUDIO_ZONE_TOL for r, w in zip(rise, want))
              and abs(opened - 1.0) <= AUDIO_ZONE_TOL
              and abs(voice - AUDIO_ZONE_THROUGH) <= AUDIO_ZONE_TOL)
        print(f"  audio-zone-fade {'PASS' if ok else 'FAIL'}  from the other zone {start:.4f}; "
              f"stepping in, a window at a time: {', '.join(f'{r:.3f}' for r in rise)} (want "
              f"{', '.join(f'{w:.3f}' for w in want)}, each +- {AUDIO_ZONE_TOL}); the link opened "
              f"{opened:.4f} (want 1); a voice started in the other zone {voice:.4f} in its first "
              f"window (want {AUDIO_ZONE_THROUGH})")
        if not ok:
            failures.append("audio-zone-fade")

    # --- audio-pitch ------------------------------------------------------------
    text, code = _gametest_probe_text("--audio-probe", "pitch", extra=["--audio-file", wav])
    rows = _audio_bright(text, "pitch")
    if code != 0 or any(k not in rows for k in ("unset", "one", "high")):
        print("  audio-pitch FAIL  run failed or measured nothing")
        failures.append("audio-pitch")
    else:
        ratio = rows["high"][1] / rows["one"][1]
        ok = (abs(ratio - AUDIO_PITCH_HIGH) < 0.005 and rows["unset"] == rows["one"]
              and rows["one"][0] > 0.05)
        print(f"  audio-pitch {'PASS' if ok else 'FAIL'}  a voice at pitch {AUDIO_PITCH_HIGH} is "
              f"{ratio:.4f} as bright as at 1 (want {AUDIO_PITCH_HIGH} +- 0.005, a sine's "
              f"brightness being its frequency); unset it is "
              f"{'the same to the digit' if rows['unset'] == rows['one'] else 'NOT the same'} as "
              f"at 1 ({rows['unset']} and {rows['one']})")
        if not ok:
            failures.append("audio-pitch")
        # A held sound re-pitched while it plays (spec 13.56), the motor's loop's way.
        held = ("held_one", "held_high")
        if any(k not in rows for k in held):
            print("  audio-held-pitch FAIL  measured nothing")
            failures.append("audio-held-pitch")
        else:
            ratio = rows["held_high"][1] / rows["held_one"][1]
            # The tone is quieter than the file, measured 0.044 at its 2 m: heard is what is asked.
            ok = abs(ratio - AUDIO_PITCH_HIGH) < 0.005 and rows["held_one"][0] > 0.02
            print(f"  audio-held-pitch {'PASS' if ok else 'FAIL'}  a held tone re-pitched to "
                  f"{AUDIO_PITCH_HIGH} as it plays is {ratio:.4f} as bright (want "
                  f"{AUDIO_PITCH_HIGH} +- 0.005)")
            if not ok:
                failures.append("audio-held-pitch")

    # --- audio-load -------------------------------------------------------------
    with open(wav + ".junk", "w") as f:
        f.write("<html><body>429 Too Many Requests</body></html>\n")
    text, code = _gametest_probe_text("--audio-probe", "load", extra=["--audio-file", wav])
    refused = re.search(r"^audio load refused missing (\d) junk (\d)$", text, re.M)
    good = [(float(l), float(rr)) for c, label, l, rr in _AUDIO_PROBE.findall(text)
            if c == "load" and label == "good"]
    energy = avg(good[0]) if good else 0.0
    ok = (code == 0 and refused is not None and refused.groups() == ("1", "1")
          and energy > 0.05)
    print(f"  audio-load {'PASS' if ok else 'FAIL'}  in a loading phase a missing file and a "
          f"file of no audio are "
          f"{'refused' if refused and refused.groups() == ('1', '1') else 'NOT both refused'}, "
          f"the phase ends ({'exit 0' if code == 0 else f'exit {code}'}), and the good one "
          f"plays at rms {energy:.4f} (want > 0.05)")
    if not ok:
        failures.append("audio-load")

    return failures
