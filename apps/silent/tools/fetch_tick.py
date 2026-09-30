"""Fetch the hall clock's tick and tock for apps/silent, and put them in a case.

The beat is cut from a real grandfather clock: SOURCE below, a CC0 recording
on Freesound (https://freesound.org). Its preview MP3 needs no account and a
tick needs nothing better. The clock's two pallets strike differently, so its
beats alternate loud and soft; one of each is cut, from the cleanest pair in
the recording, and those are the tick and the tock.

Then the case. The recording is close and dry, and a longcase clock is heard
through a tall wooden box, which does four things to a tick, each a stage
here and each tunable in CASES:
  - the air in the trunk rings at its own lengths -- about 90, 390 and 690 Hz
    for a 1.9 x 0.44 x 0.25 m case -- so each tick leaves a short hum;
  - the thin panels resonate low and hollow, a few hundred hertz;
  - the tick reflects off the inside walls a few milliseconds apart, which is
    the boxed-in colour;
  - wood soaks up the top, so the whole of it is darker.

Writes assets/audio/silent/tick.wav and tock.wav from the CASE preset. With
--audition DIR it also writes every preset there, each as a tick, a tock and
a ten-second run at the game's one beat a second, to choose by ear. Needs
ffmpeg to decode the MP3. Downloads are cached under out/polyhaven_cache.

    python3 apps/silent/tools/fetch_tick.py [--audition DIR]
"""

import argparse
import os
import subprocess
import sys

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as signal

from fetch_textures import CACHE_DIR, ROOT, fetch

OUT_DIR = os.path.join(ROOT, "assets", "audio", "silent")
RATE = 48000

SOURCE = {
    "title": "Grandfather clock.wav",
    "by": "Ryding",
    "page": "https://freesound.org/people/Ryding/sounds/125968/",
    "licence": "CC0 1.0",
    "preview": "https://cdn.freesound.org/previews/125/125968_981397-hq.mp3",
}

PRE = 0.004    # seconds kept before a beat's onset
LENGTH = 0.45  # seconds kept in all: under the recording's 0.57 s between beats
FADE = 0.08    # seconds of fade at the end, into silence
PEAK = 0.8     # of full scale, for the tick; the tock keeps its own level under it

# The case presets. mix is how much of the case is heard against the dry
# tick; modes are (Hz, ring seconds, gain) for the air and the panels;
# reflections are (seconds, gain) off the inside walls; bright is where the
# wood takes the top off, in Hz.
CASES = {
    "dry": {"mix": 0.0},
    "light": {
        "mix": 0.35,
        "modes": [(90.0, 0.06, 0.8), (180.0, 0.05, 0.6), (390.0, 0.04, 0.5), (690.0, 0.03, 0.3)],
        "reflections": [(0.0015, 0.5), (0.0026, 0.4), (0.0029, -0.3), (0.0040, 0.25),
                        (0.0051, -0.18), (0.0077, 0.1)],
        "bright": 8000.0,
    },
    "heavy": {
        "mix": 0.6,
        "modes": [(90.0, 0.09, 1.0), (160.0, 0.08, 0.9), (240.0, 0.07, 0.7), (390.0, 0.06, 0.6),
                  (690.0, 0.04, 0.35)],
        "reflections": [(0.0015, 0.6), (0.0026, 0.5), (0.0029, -0.4), (0.0040, 0.35),
                        (0.0051, -0.28), (0.0063, 0.2), (0.0077, 0.15), (0.0102, 0.1)],
        "bright": 5000.0,
    },
}
CASE = "light"  # what the game plays


def decode():
    """The recording as mono floats at RATE."""
    mp3 = os.path.join(CACHE_DIR, "freesound_125968.mp3")
    wav = os.path.join(CACHE_DIR, "freesound_125968.wav")
    if not os.path.exists(mp3):
        fetch(SOURCE["preview"], "freesound_125968.mp3")
    if not os.path.exists(wav):
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", mp3, "-ac", "1",
                        "-ar", str(RATE), "-c:a", "pcm_f32le", wav], check=True)
    rate, x = wavfile.read(wav)
    assert rate == RATE
    return x.astype(np.float64)


def beats(x):
    """Every beat's onset in samples, and how loud it is."""
    env = signal.filtfilt(*signal.butter(2, 60.0 / (RATE / 2)), np.abs(x))
    peaks, props = signal.find_peaks(env, height=env.max() * 0.25, distance=int(0.4 * RATE))
    return peaks, props["peak_heights"], env


def cleanest_pair(x):
    """The loud beat and the soft one after it with the least noise under
    them, among pairs that keep the clock's own rhythm -- the recording has a
    stretch where something else was heard, and its beats come unevenly."""
    peaks, heights, env = beats(x)
    gaps = np.diff(peaks)
    beat = np.median(gaps)
    best, score = None, -1.0
    for i in range(len(peaks) - 2):
        if heights[i] < heights[i + 1]:
            continue  # a pair starts on the loud beat
        if abs(gaps[i] - beat) > 0.1 * beat or abs(gaps[i + 1] - beat) > 0.1 * beat:
            continue
        floor = np.median(env[peaks[i] - int(0.1 * RATE):peaks[i] - int(0.02 * RATE)])
        s = min(heights[i], heights[i + 1]) / max(floor, 1e-9)
        if s > score:
            best, score = (peaks[i], peaks[i + 1]), s
    return best


def onset(x, peak):
    """Where the beat starts: the first sample within 5 ms before its envelope
    peak that rises past a fifth of the peak's height."""
    lo = peak - int(0.005 * RATE)
    seg = np.abs(x[lo:peak + 1])
    return lo + int(np.argmax(seg > 0.2 * seg.max()))


def cut(x, at):
    start = at - int(PRE * RATE)
    y = x[start:start + int(LENGTH * RATE)].copy()
    y *= np.minimum(1.0, np.arange(y.size) / (0.002 * RATE))
    n = int(FADE * RATE)
    y[-n:] *= 0.5 * (1.0 + np.cos(np.linspace(0.0, np.pi, n)))
    return signal.filtfilt(*signal.butter(2, 40.0 / (RATE / 2), "high"), y)


def case(y, preset):
    """The tick heard through the clock's case: preset's mix of the dry tick
    and the same through the case's modes and inside reflections, darkened."""
    if preset["mix"] <= 0.0:
        return y
    wet = y.copy()
    for delay, gain in preset["reflections"]:
        n = int(round(delay * RATE))
        wet[n:] += gain * y[:-n]
    for hz, ring, gain in preset["modes"]:
        q = np.pi * hz * ring
        b, a = signal.iirpeak(hz, q, fs=RATE)
        wet += gain * signal.lfilter(b, a, y)
    wet = signal.lfilter(*signal.butter(2, preset["bright"] / (RATE / 2)), wet)
    wet *= np.max(np.abs(y)) / max(np.max(np.abs(wet)), 1e-9)
    return (1.0 - preset["mix"]) * y + preset["mix"] * wet


def write(path, y):
    pcm = np.clip(y * 32767.0, -32768, 32767).astype("<i2")
    wavfile.write(path, RATE, pcm)


def make(x, pair, preset):
    tick, tock = (case(cut(x, onset(x, p)), preset) for p in pair)
    scale = PEAK / np.max(np.abs(tick))
    return tick * scale, tock * scale


def run(tick, tock, seconds=10):
    """Ten seconds of the clock as the game plays it: a beat a second."""
    out = np.zeros(seconds * RATE + tick.size)
    for k in range(seconds):
        s = tock if k & 1 else tick
        out[k * RATE:k * RATE + s.size] += s
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--audition", help="also write every case preset here, to choose by ear")
    args = parser.parse_args()
    os.makedirs(CACHE_DIR, exist_ok=True)
    os.makedirs(OUT_DIR, exist_ok=True)
    x = decode()
    pair = cleanest_pair(x)
    print("%s by %s, %s (%s): beats at %.3f and %.3f s" % (
        SOURCE["title"], SOURCE["by"], SOURCE["page"], SOURCE["licence"], pair[0] / RATE,
        pair[1] / RATE))
    tick, tock = make(x, pair, CASES[CASE])
    write(os.path.join(OUT_DIR, "tick.wav"), tick)
    write(os.path.join(OUT_DIR, "tock.wav"), tock)
    print(os.path.join(OUT_DIR, "tick.wav"), os.path.join(OUT_DIR, "tock.wav"))
    if args.audition:
        os.makedirs(args.audition, exist_ok=True)
        for name, preset in CASES.items():
            t, k = make(x, pair, preset)
            write(os.path.join(args.audition, "%s_tick.wav" % name), t)
            write(os.path.join(args.audition, "%s_tock.wav" % name), k)
            write(os.path.join(args.audition, "%s_clock_10s.wav" % name), run(t, k))
        print(args.audition)
    return 0


if __name__ == "__main__":
    sys.exit(main())
