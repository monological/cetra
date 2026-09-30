"""Synthesize the hall clock's tick and tock for apps/silent.

A longcase clock's beat is three sounds on top of one another: the pallet
striking a tooth of the escape wheel, a sharp click a couple of milliseconds
long; the steel of the escapement ringing after it, high and quickly gone;
and the case, a tall wooden box, resonating low under both. Each is made
here as noise or damped sines, so the files are deterministic and owe
nobody a licence. The tock is the other pallet: tuned a little lower and
struck a little softer, which is what makes a clock say tick-tock rather
than tick-tick.

Writes assets/audio/silent/tick.wav and tock.wav, 48 kHz mono 16-bit. Run
from anywhere; --plot PATH also draws each sound's waveform and spectrogram,
since nothing headless can listen to them:

    python3 apps/silent/tools/make_tick.py [--plot PATH]
"""

import argparse
import os
import sys
import wave

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OUT_DIR = os.path.join(ROOT, "assets", "audio", "silent")
RATE = 48000
LENGTH = 0.15  # seconds: the body's ring is gone well inside it

# (frequency Hz, amplitude, decay time constant s)
STEEL = [(3100.0, 0.35, 0.008), (4700.0, 0.25, 0.006), (6200.0, 0.15, 0.004)]
CASE = [(180.0, 0.5, 0.03), (420.0, 0.35, 0.025)]
CLICK_DECAY = 0.0012  # seconds
CLICK_CUT = 1500.0    # Hz: a high-pass, so the strike clicks rather than thuds
PEAK = 0.8            # of full scale, for the tick; the tock is scaled from it

# name: (noise seed, pitch factor, level)
SOUNDS = {"tick": (7, 1.0, 1.0), "tock": (11, 0.92, 0.85)}


def high_pass(x, cut):
    """One pole: enough to take the thud out of a click."""
    rc = 1.0 / (2.0 * np.pi * cut)
    alpha = rc / (rc + 1.0 / RATE)
    y = np.zeros_like(x)
    for n in range(1, x.size):
        y[n] = alpha * (y[n - 1] + x[n] - x[n - 1])
    return y


def modes(t, table, pitch, rng):
    out = np.zeros_like(t)
    for freq, amp, tau in table:
        phase = rng.uniform(0.0, 2.0 * np.pi)
        out += amp * np.sin(2.0 * np.pi * freq * pitch * t + phase) * np.exp(-t / tau)
    return out


def beat(seed, pitch, level):
    rng = np.random.default_rng(seed)
    t = np.arange(int(RATE * LENGTH)) / RATE
    click = high_pass(rng.standard_normal(t.size) * np.exp(-t / CLICK_DECAY), CLICK_CUT)
    sound = 0.5 * click + modes(t, STEEL, pitch, rng) + modes(t, CASE, pitch, rng)
    return sound / np.max(np.abs(sound)) * PEAK * level


def write_wav(path, samples):
    pcm = np.clip(samples * 32767.0, -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())
    print(path)


def plot(path, sounds):
    """Each sound as a waveform over a log-magnitude spectrogram to 12 kHz."""
    width, wave_h, spec_h = 900, 160, 220
    sheet = Image.new("RGB", (width, len(sounds) * (wave_h + spec_h)), (16, 16, 16))
    draw = ImageDraw.Draw(sheet)
    win, hop = 256, 24
    for i, (name, s) in enumerate(sounds.items()):
        top = i * (wave_h + spec_h)
        mid = top + wave_h // 2
        xs = np.linspace(0, s.size - 1, width).astype(int)
        pts = [(x, mid - s[j] * (wave_h / 2 - 8)) for x, j in enumerate(xs)]
        draw.line(pts, fill=(120, 220, 140))
        draw.text((6, top + 4), "%s  %.0f ms" % (name, 1000.0 * s.size / RATE), fill=(230, 230, 230))
        frames = [s[k:k + win] * np.hanning(win) for k in range(0, s.size - win, hop)]
        mag = np.abs(np.fft.rfft(np.array(frames), axis=1)).T
        bins = int(12000.0 / (RATE / 2) * mag.shape[0])
        db = 20.0 * np.log10(mag[:bins] + 1e-6)
        db = np.clip((db - db.max() + 70.0) / 70.0, 0.0, 1.0)
        img = Image.fromarray((np.flipud(db) * 255).astype(np.uint8)).resize(
            (width, spec_h), Image.Resampling.BILINEAR)
        sheet.paste(Image.merge("RGB", (img, img.point(lambda v: v // 2), img.point(lambda v: 255 - v // 2 if v else 0))),
                    (0, top + wave_h))
    sheet.save(path)
    print(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--plot", help="also draw each sound's waveform and spectrogram")
    args = parser.parse_args()
    os.makedirs(OUT_DIR, exist_ok=True)
    sounds = {name: beat(*spec) for name, spec in SOUNDS.items()}
    for name, samples in sounds.items():
        write_wav(os.path.join(OUT_DIR, name + ".wav"), samples)
    if args.plot:
        plot(args.plot, sounds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
