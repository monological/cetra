"""Fetch the sounds apps/silent plays, and shape each for its job.

Every sound is cut from a CC0 recording on Freesound (https://freesound.org),
and RECORDINGS below says which, by whom and under what licence. A preview
MP3 needs no account, and nothing here needs better. Two recipes:

THE CLOCK'S BEAT. The clock's two pallets strike differently, so its beats
alternate loud and soft; one of each is cut, from the cleanest pair in the
recording, and those are the tick and the tock. The recording is close and
dry, and a longcase clock is heard through a tall wooden box, which does four
things to a tick, each a stage here and each tunable in CASES:
  - the air in the trunk rings at its own lengths -- about 90, 390 and 690 Hz
    for a 1.9 x 0.44 x 0.25 m case -- so each tick leaves a short hum;
  - the thin panels resonate low and hollow, a few hundred hertz;
  - the tick reflects off the inside walls a few milliseconds apart, which is
    the boxed-in colour;
  - wood soaks up the top, so the whole of it is darker.

A LOOP, for a sound that never stops -- a fridge's hum, a tube's buzz, the
wind. The steadiest stretch of the recording is taken, so a loop never
repeats a car going by, and its tail is crossfaded into its head at equal
power so it goes round with no seam. A loop can also be MUFFLED into the
layer heard through a wall: low-passed and quieter, filtered round its own
period so the seam survives. Every loop is levelled to LEVEL_RMS, short of
clipping, so the gains in the app are the only statement of how loud
anything is.

Writes the beat as WAV and the loops as FLAC, which is gapless and half a
WAV's size, under assets/audio/silent/, and GENERATES apps/silent/src/door_takes.h,
where in each of the haunting's door takes the door hits its stop -- so never
edit that header; rerun this. With --audition DIR it writes every
CASES preset and every CANDIDATES recording there instead -- a loop repeated
over thirty seconds, so its seam can be heard -- to choose by ear. Needs
ffmpeg. Downloads are cached under out/polyhaven_cache, which is a build directory's and starts
empty in a fresh checkout, so --only names the jobs to fetch and make, and nothing else is
downloaded or written.

    python3 apps/silent/tools/fetch_sounds.py [--audition DIR] [--only JOB ...]
"""

import argparse
import os
import shutil
import subprocess
import sys

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as signal

from fetch_textures import CACHE_DIR, ROOT, fetch

OUT_DIR = os.path.join(ROOT, "assets", "audio", "silent")
RATE = 48000

# Freesound id -> who made it, and its preview. Every one is CC0 1.0.
RECORDINGS = {
    125968: ("Grandfather clock.wav", "Ryding", "125/125968_981397"),
    635565: ("fridge refrigerator old junky nearby heard from down short hall", "kyles",
             "635/635565_612689"),
    353797: ("Humming sound effect | from fridge", "Ironlink15", "353/353797_3015261"),
    573936: ("Fridge hum whirl stops loud clunk bang", "TRP", "573/573936_97550"),
    676841: ("Noisy old fluorescent light", "Brokkolix", "676/676841_11690710"),
    125064: ("Faulty Fluorescent Light Starter & Hum.wav", "EverydaySounds",
             "125/125064_2273054"),
    273625: ("Fluorescent Ballast Buzz - Room Tone", "Sauron974", "273/273625_152878"),
    830440: ("Fluorescent Light Buzzing", "JoelMcDaniel", "830/830440_17706853"),
    502879: ("Outdoors_Night_Windy_01.wav", "MrFossy", "502/502879_129727"),
    386823: ("Breezy night outdoors", "Clubadub", "386/386823_1984539"),
    843000: ("Winchester Wind ambience", "Paddywack0", "843/843000_1339853"),
    # The cat (spec 13.17).
    110011: ("cat meow", "tuberatanka", "110/110011_1537422"),
    412017: ("cat meow short", "skymary", "412/412017_3652520"),
    479272: ("New Cat Meow 1", "steffcaffrey", "479/479272_1844073"),
    528197: ("Cat meow 1", "fthgurdy", "528/528197_3302313"),
    455493: ("Cat Meow.aif", "Wrenasmir", "455/455493_1138747"),
    66518: ("meow9.wav", "freemaster2", "66/66518_334103"),
    850104: ("Cat trills shortly (calm/quiet)", "CatPhony", "850/850104_18444603"),
    668825: ("Cat trill 4", "MBPL", "668/668825_14585550"),
    200336: ("Cat trilling", "jsbarrett", "200/200336_3159560"),
    485952: ("Cat Hissing", "aunrea", "485/485952_7932944"),
    146963: ("catHisses1.wav", "Zabuhailo", "146/146963_2580450"),
    819958: ("Cat hissing", "rickismyname", "819/819958_16011171"),
    532225: ("cat hiss.wav", "patchytherat", "532/532225_5911297"),
    455495: ("Cat Growl into Hiss.aif", "Wrenasmir", "455/455495_1138747"),
    658429: ("Animal Footsteps", "IENBA", "658/658429_9616576"),
    338352: ("Cat Running Stairs", "PhilllChabbb", "338/338352_4205952"),
    569255: ("Claws animal (foley)", "Nakhas", "569/569255_717697"),
    803297: ("cat_land_floor", "superEGsonic", "803/803297_663245"),
    584442: ("Bag Drop Soft Surface 1", "BenjaminNelan", "584/584442_1196020"),
    191957: ("pillow-throw01.flac", "Aiyumi", "191/191957_2326086"),
    553962: ("Cat-purr.wav", "clareboots", "553/553962_11756852"),
    463790: ("Cat Purr.wav", "shyguy014", "463/463790_7805928"),
    656500: ("Cat purr", "druulian", "656/656500_6497685"),
    575933: ("Cat Purr", "RazzleDizzle", "575/575933_1824398"),
    765159: ("TV static", "MieckevanHoek", "765/765159_15764256"),
    # The basement's drip (spec 13.31).
    815419: ("AAD WATER DRIPPING INTO WATER", "AwenAudio", "815/815419_4940561"),
    546279: ("Single drip - dripping", "Mega-X-stream", "546/546279_4937681"),
    792932: ("Slow Single Water Drop Splash", "qubodup", "792/792932_71257"),
    22438: ("Drip.wav", "Lunardrive", "22/22438_120830"),
    # The lake and the cabin (spec 13.41): water lapping at a shore, a fire in a hearth, and a
    # small room's own quiet.
    518467: ("Small Waves Lapping on Lake Ontario Close Perspective", "robotjay",
             "518/518467_476277"),
    568819: ("200906 Water waves lapping, lake, gentle, close", "TRP", "568/568819_97550"),
    464801: ("Tiny waves lapping #3.m4a", "guyburns", "464/464801_5454234"),
    614299: ("Gentle waves on a lake", "TheFlyFishingFilmmaker", "614/614299_6501596"),
    549208: ("Fireplace.wav", "OwennewO", "549/549208_11244040"),
    718202: ("Fireplace woodstove with the lid open", "HullMusic", "718/718202_9307732"),
    512685: ("Fireplace Crackles", "WavJunction.com", "512/512685_9514571"),
    414298: ("ASTRONOMER FIREPLACE AMBI.wav", "schulmancreative", "414/414298_3718658"),
    452516: ("room tone small log cabin quiet with fire in wood stove metal clinks.flac", "kyles",
             "452/452516_612689"),
    744447: ("Soft Room Tone", "callmethefoo", "744/744447_14793871"),
    # The player's footsteps (spec 13.47), four candidates a surface.
    539222: ("walking on a squeaky floor", "mrsorbias", "539/539222_3606949"),
    635058: ("Walking Footsteps on Wood", "sillygrizzlies", "635/635058_5756735"),
    362777: ("Walking_Wood.mp3", "taure", "362/362777_2940947"),
    198962: ("footsteps on wood", "Mydo1", "198/198962_3138963"),
    170506: ("Footsteps, Tile, Male Sneakers, Slow Pace.wav", "SpliceSound", "170/170506_1480854"),
    565530: ("Kitchen Footsteps.WAV", "carsonbailie", "565/565530_7323818"),
    50725: ("Footsteps on Tiles.wav", "RutgerMuller", "50/50725_179538"),
    655367: ("rubber shoes on tiles footsteps.wav", "21100495", "655/655367_13723333"),
    148466: ("FOLEY_Footsteps_Carpet_001.wav", "conleec", "148/148466_221578"),
    545540: ("light footsteps on carpet walk.wav", "rsellick", "545/545540_6233742"),
    164100: ("Walking Footsteps on Carpet.aif", "Abolla", "164/164100_2054457"),
    463644: ("footsteps boots carpet.wav", "alec_mackay", "463/463644_6551032"),
    546827: ("Footsteps Leather Concrete", "Kinoton", "546/546827_2247456"),
    272260: ("concrete_14a_lightshoes_walk.wav", "sturmankin", "272/272260_4181170"),
    545543: ("light footsteps on concrete walking.wav", "rsellick", "545/545543_6233742"),
    459964: ("Footsteps on concrete", "florianreichelt", "459/459964_6253486"),
    205817: ("footsteps pavement street", "Yuval", "205/205817_770707"),
    539556: ("walking small street wet asphalt.wav", "Federico_Casazza", "539/539556_2549389"),
    711434: ("Steps in the city | Raining", "csigusz_foxoup", "711/711434_7724386"),
    553238: ("Walking through puddle", "NachtmahrTV", "553/553238_5620304"),
    352870: ("Footsteps Dirt Gravel", "PotatokingXII", "352/352870_4187409"),
    505831: ("Footsteps outdoors gravel.wav", "jedg", "505/505831_4024739"),
    419164: ("03-Man walking gravel.wav", "14GPanskaHonc_Petr", "419/419164_8353567"),
    556002: ("Footsteps_Mountain_Boots_Gravel_Mono.wav", "Nox_Sound", "556/556002_9250976"),
    685286: ("Footsteps grass.wav", "emilgasi", "685/685286_12974819"),
    505833: ("Footsteps grass.wav", "jedg", "505/505833_4024739"),
    377323: ("steps_dry_grass.wav", "Mystikuum", "377/377323_1643758"),
    389454: ("Footsteps_On_Dirt.wav", "lzmraul", "389/389454_6060491"),
    670305: ("steps on wet field.wav", "bruno.auzet", "670/670305_11519060"),
    760861: ("Walking through long grass and brush on wet ground wearing boots male",
             "Whats_The_Frequency_Kennett", "760/760861_8254017"),
    231317: ("Footsteps-Mud&Grass", "jonccox", "231/231317_3261532"),
    231850: ("Walk Wet", "alegemaate", "231/231850_2531187"),
    218305: ("01-20 Footsteps, sneakers on dry leaves.wav", "SpliceSound", "218/218305_1480854"),
    524496: ("Footsteps in Forest.wav", "vero.marengere", "524/524496_6605732"),
    398267: ("Walking on dry leaves and twigs.wav", "gabytoledosci", "398/398267_7618848"),
    474113: ("Footsteps forest", "Juno24", "474/474113_8872754"),
    543930: ("walking through thick wet mud 2.wav", "walthamstow_walker", "543/543930_12143735"),
    670850: ("rubber boots in mud.wav", "bruno.auzet", "670/670850_11519060"),
    455057: ("footsteps-mud.wav", "audioninja001", "455/455057_2699264"),
    446257: ("Walking In Mud", "lukiacostello", "446/446257_932203"),
    553863: ("deep water footsteps.wav", "bruno.auzet", "553/553863_11519060"),
    342932: ("Wading in Shallow Water.wav", "ryansitz", "342/342932_5800259"),
    583756: ("wet footsteps.mp3", "Bia12", "583/583756_4614795"),
    369580: ("Slow Water Footsteps", "Adielees9", "369/369580_4800355"),
    # The haunting (spec 13.52), chosen by ear: the basement door shaken in its frame, its knob
    # tried, a door slammed, and the bathroom door creaking ajar.
    444704: ("RattlingLockedDoor.mp3", "shelbyshark", "444/444704_8644110"),
    727791: ("doorknob rattle", "DrFahrts", "727/727791_10249017"),
    571797: ("Door slamming hard", "NachtmahrTV", "571/571797_5620304"),
    426228: ("squeaky door", "derjuli", "426/426228_2573247"),
}
PREVIEW = "https://cdn.freesound.org/previews/%s-hq.mp3"
CLOCK = 125968

# Each kind of loop: how long a stretch it takes, and whether it keeps its
# stereo -- a bed heard from everywhere does; a source placed in the room is
# mono, since the engine places it.
JOBS = {
    "fridge": {"seconds": 12.0, "stereo": False},
    "tube": {"seconds": 6.0, "stereo": False},
    "wind": {"seconds": 30.0, "stereo": True},
    "purr": {"seconds": 4.0, "stereo": False},
    # A television tuned to nothing (spec 13.30): its own hiss.
    "static": {"seconds": 8.0, "stereo": False},
    # The lake's edge, the cabin's hearth and its room (spec 13.41), each placed in the world.
    "lapping": {"seconds": 20.0, "stereo": False},
    "hearth": {"seconds": 12.0, "stereo": False},
    "room": {"seconds": 20.0, "stereo": False},
}

# What --audition renders for each job, to be chosen by ear.
CANDIDATES = {
    "fridge": [635565, 353797, 573936],
    "tube": [676841, 125064, 273625, 830440],
    "wind": [502879, 386823, 843000],
    "purr": [553962, 463790, 656500, 575933],
    "static": [765159],
    "lapping": [518467, 568819, 464801, 614299],
    "hearth": [549208, 718202, 512685, 414298],
    "room": [452516, 744447],
}

# What the game plays, chosen by ear from the candidates: file name -> (job,
# recording).
LOOPS = {
    "fridge_hum": ("fridge", 635565),
    "tube_buzz": ("tube", 273625),
    "wind_outside": ("wind", 386823),
    "cat_purr": ("purr", 656500),
    # An old set on a channel with no signal, chosen by ear.
    "tv_static": ("static", 765159),
    # The lake's edge, the cabin's hearth and its room (spec 13.41), chosen by ear.
    "lake_lapping": ("lapping", 568819),
    "hearth_fire": ("hearth", 414298),
    "cabin_room": ("room", 452516),
}

# A ONESHOT, for a sound that happens once -- a meow, a footfall, a landing. The recording's
# events are found by their envelope, each cut from just before its onset to a tail after it
# falls away, faded, high-passed to take off handling rumble, and levelled like a loop by its
# loud part. Each kind: the longest an event may be,
# the tail kept after it, the high-pass, and how many events a recording may give -- and
# optionally `join`, the gap under which two loud runs are one event (60 ms unless it says).
#
# The player's footsteps (spec 13.47) are a kind a surface, step_<surface>, so a surface's set is
# fetched or auditioned by its name, and the files are step_<surface>_<n>.wav. A shoe on a floor
# or the ground is short; a wade, or a foot in mud, is a slosh and the foot drawn out after it.
STEP = {"longest": 0.45, "tail": 0.1, "highpass": 70.0, "per": 6}
WADE = {"longest": 0.9, "tail": 0.15, "highpass": 60.0, "per": 6}
WALK_GAP = 0.55  # seconds between the steps of an audition's walk, a walking pace
ONESHOT_JOBS = {
    "meow": {"longest": 1.6, "tail": 0.12, "highpass": 150.0, "per": 3},
    "trill": {"longest": 1.0, "tail": 0.08, "highpass": 120.0, "per": 3},
    "hiss": {"longest": 2.0, "tail": 0.1, "highpass": 200.0, "per": 2},
    "paw": {"longest": 0.14, "tail": 0.03, "highpass": 80.0, "per": 8},
    "land": {"longest": 0.5, "tail": 0.08, "highpass": 60.0, "per": 2},
    "drip": {"longest": 0.5, "tail": 0.25, "highpass": 250.0, "per": 4},
    "step_wood": STEP,
    "step_tile": STEP,
    "step_cloth": STEP,
    "step_concrete": STEP,
    "step_pavement_wet": STEP,
    "step_gravel": STEP,
    "step_grass": STEP,
    "step_grass_wet": STEP,
    "step_forest": STEP,
    "step_mud": WADE,
    "step_water": WADE,
    # The haunting's door (spec 13.52). A try at a locked door is a burst of clacks a tenth of a
    # second or two apart, which a 60 ms join would cut into single clicks: one try is one take.
    "rattle": {"longest": 2.0, "tail": 0.15, "highpass": 70.0, "per": 4, "join": 0.3},
    "knob": {"longest": 2.0, "tail": 0.12, "highpass": 90.0, "per": 8, "join": 0.3},
    # A slam keeps its low thump and the frame ringing after it.
    "slam": {"longest": 1.6, "tail": 0.6, "highpass": 35.0, "per": 2},
    # The fridge's compressor stopping: the whirl dying and the clunk.
    "clunk": {"longest": 2.0, "tail": 0.4, "highpass": 35.0, "per": 3, "join": 0.2},
}
ONESHOT_CANDIDATES = {
    "meow": [110011, 412017, 479272, 528197, 455493, 66518],
    "trill": [850104, 668825, 200336],
    "hiss": [485952, 146963, 819958, 532225, 455495],
    "paw": [658429, 338352, 569255],
    "land": [803297, 584442, 191957],
    "drip": [815419, 546279, 792932, 22438],
    "step_wood": [539222, 635058, 362777, 198962],
    "step_tile": [170506, 565530, 50725, 655367],
    "step_cloth": [148466, 545540, 164100, 463644],
    "step_concrete": [546827, 272260, 545543, 459964],
    "step_pavement_wet": [205817, 539556, 711434, 553238],
    "step_gravel": [352870, 505831, 419164, 556002],
    "step_grass": [685286, 505833, 377323, 389454],
    "step_grass_wet": [670305, 760861, 231317, 231850],
    "step_forest": [218305, 524496, 398267, 474113],
    "step_mud": [543930, 670850, 455057, 446257],
    "step_water": [553863, 342932, 583756, 369580],
    "rattle": [444704],
    "knob": [727791],
    "slam": [571797],
    "clunk": [573936],
}
# The player's footsteps (spec 13.47), chosen by ear from each candidate's walk: surface ->
# recording. Its takes are the walk's, in the walk's order, as step_<surface>_1 to _6 -- the
# count footsteps.h's FOOTSTEP_TAKES reads.
FOOTSTEP_TAKES = 6
FOOTSTEPS = {
    "wood": 362777,
    "tile": 170506,
    "cloth": 545540,
    "concrete": 272260,
    "pavement_wet": 205817,
    "gravel": 352870,
    "grass": 685286,
    "grass_wet": 760861,
    "forest": 524496,
    "mud": 543930,
    "water": 369580,
}
# What the game plays: file name -> (job, recording, which event, by the order --audition
# numbers them). Chosen by length and source, not yet by ear: the meows are one cat's three
# rather than three cats', the paws four soft falls from the start of one recording.
ONESHOTS = {
    "cat_meow_1": ("meow", 455493, 0),
    "cat_meow_2": ("meow", 455493, 1),
    "cat_meow_3": ("meow", 455493, 2),
    "cat_trill": ("trill", 850104, 0),
    "cat_hiss": ("hiss", 146963, 0),
    "cat_paw_1": ("paw", 658429, 0),
    "cat_paw_2": ("paw", 658429, 2),
    "cat_paw_3": ("paw", 658429, 3),
    "cat_paw_4": ("paw", 658429, 5),
    "cat_land_hard": ("land", 803297, 0),
    "cat_land_soft": ("land", 584442, 0),
    # The tap over the basement's laundry tub (spec 13.31), dripping into the water standing in
    # it: chosen by ear.
    "basement_drip": ("drip", 815419, 3),
    **{"step_%s_%d" % (surface, k + 1): ("step_" + surface, sound, k)
       for surface, sound in FOOTSTEPS.items() for k in range(FOOTSTEP_TAKES)},
    # The haunting (spec 13.52), chosen by ear: the basement door shaken in its frame, four tries
    # at its knob for the game to pick among, a door slammed, and the fridge's compressor stopping.
    "door_rattle": ("rattle", 444704, 1),
    "door_knob_1": ("knob", 727791, 0),
    "door_knob_2": ("knob", 727791, 1),
    "door_knob_3": ("knob", 727791, 2),
    "door_knob_4": ("knob", 727791, 3),
    "door_slam": ("slam", 571797, 0),
    "fridge_clunk": ("clunk", 573936, 0),
}

# The door takes the haunting moves the basement door's leaf with, and the header that says where
# in each the door hits its stop: a kind -> its files, in the order the game numbers them.
DOOR_TAKES = {
    "RATTLE": ["door_rattle"],
    "KNOB": ["door_knob_1", "door_knob_2", "door_knob_3", "door_knob_4"],
}
DOOR_TAKE_HITS = 24  # the most hits a take keeps, its loudest
DOOR_HEADER = os.path.join(ROOT, "apps", "silent", "src", "door_takes.h")
ONESHOT_LEAD = 0.01  # seconds kept before an event's onset

# A one-shot cut by hand where no envelope would find it -- one stretch of a longer take, chosen
# by ear: file name -> (job, recording, from, to in seconds, high-pass Hz). Shaped and levelled
# as a found event is.
CUTS = {
    # The bathroom door drifting ajar (spec 13.52): one slow squeal of the hinge.
    "door_creak": ("creak", 426228, 1.0, 4.0, 100.0),
}

# The layers heard through the house's walls: file name -> (loop, low-pass
# Hz, level). A wall passes the low end and little of the rest.
MUFFLES = {
    "wind_inside": ("wind_outside", 600.0, 0.5),
}

# Every loop and every one-shot is levelled alike, so the volumes in the C files are the only
# statement of loudness: its loud part to LEVEL_RMS, about -20 dBFS, unless its peaks would pass
# LEVEL_PEAK first -- a gusty wind comes out quieter.
LEVEL_RMS = 0.1
LEVEL_PEAK = 0.9
CROSSFADE = 0.75  # seconds of tail laid over the head at the loop's seam

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


def decode(sound, stereo=False):
    """A recording as floats at RATE: mono, or (samples, 2) for stereo."""
    mp3 = os.path.join(CACHE_DIR, "freesound_%d.mp3" % sound)
    wav = os.path.join(CACHE_DIR, "freesound_%d%s.wav" % (sound, "_stereo" if stereo else ""))
    if not os.path.exists(mp3):
        fetch(PREVIEW % RECORDINGS[sound][2], "freesound_%d.mp3" % sound)
    if not os.path.exists(wav):
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", mp3, "-ac",
                        "2" if stereo else "1", "-ar", str(RATE), "-c:a", "pcm_f32le", wav],
                       check=True)
    rate, x = wavfile.read(wav)
    assert rate == RATE
    return x.astype(np.float64)


def credit(sound):
    title, by, _ = RECORDINGS[sound]
    return "%s by %s, https://freesound.org/s/%d/ (CC0 1.0)" % (title, by, sound)


def beats(x):
    """Every beat's onset in samples, and how loud it is."""
    env = envelope(x, 60.0)
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


def shape(y, fade_in, fade_out, highpass):
    """A cut sound faded in over `fade_in` seconds, straight, and out over its last `fade_out`
    samples on a raised cosine, then high-passed at `highpass` Hz."""
    y = y * np.minimum(1.0, np.arange(y.size) / (fade_in * RATE))
    y[-fade_out:] *= 0.5 * (1.0 + np.cos(np.linspace(0.0, np.pi, fade_out)))
    return signal.filtfilt(*signal.butter(2, highpass / (RATE / 2), "high"), y)


def level(y, measured):
    """y scaled so `measured`, the part of it its loudness is taken from, sits at LEVEL_RMS --
    unless y's peaks would pass LEVEL_PEAK first."""
    rms, peak = np.sqrt(np.mean(measured ** 2)), np.max(np.abs(y))
    return y * min(LEVEL_RMS / max(rms, 1e-9), LEVEL_PEAK / max(peak, 1e-9))


def cut(x, at):
    start = at - int(PRE * RATE)
    return shape(x[start:start + int(LENGTH * RATE)], 0.002, int(FADE * RATE), 40.0)


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


def steadiest(x, seconds):
    """Where the stretch of `seconds` starts whose loudness wavers least,
    measured in 50 ms steps -- so a passing car, a cough or a fridge cutting
    out rules a stretch out."""
    mono = x if x.ndim == 1 else x.mean(axis=1)
    step = int(0.05 * RATE)
    n = mono.size // step
    rms = np.sqrt(np.mean(mono[:n * step].reshape(n, step) ** 2, axis=1))
    width = int(round(seconds * RATE / step))
    best, score = 0, np.inf
    for i in range(max(n - width, 1)):
        w = rms[i:i + width]
        s = np.std(w) / max(np.mean(w), 1e-9)
        if s < score:
            best, score = i, s
    return best * step


def periodic(y, f):
    """f applied to a loop as though it went round forever: over three turns,
    keeping the middle one, so a filter's start-up never lands on the seam."""
    n = y.shape[0]
    return f(np.concatenate([y, y, y]))[n:2 * n]


def loop(x, seconds):
    """`seconds` of x that repeat with no seam, levelled. The
    stretch runs a crossfade's length past the loop, and that overrun is laid
    over the loop's head at equal power, so the end flows into the start."""
    fade = int(CROSSFADE * RATE)
    length = min(int(seconds * RATE), x.shape[0] - fade)
    start = steadiest(x, (length + fade) / RATE)
    y = x[start:start + length + fade]
    t = np.linspace(0.0, 0.5 * np.pi, fade)
    if y.ndim == 2:
        t = t[:, None]
    out = y[:length].copy()
    out[:fade] = y[length:] * np.cos(t) + y[:fade] * np.sin(t)
    out = periodic(out, lambda z: signal.filtfilt(*signal.butter(2, 30.0 / (RATE / 2), "high"),
                                                  z, axis=0))
    return level(out, out)


def muffle(y, cut, gain):
    """A loop heard through a wall: its top taken off above `cut` Hz, and
    `gain` of its level."""
    low = periodic(y, lambda z: signal.filtfilt(*signal.butter(2, cut / (RATE / 2)), z, axis=0))
    return gain * low


def write_flac(path, y):
    pcm = os.path.join(CACHE_DIR, "fetch_sounds_pcm.wav")
    write(pcm, y)
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", pcm, "-c:a", "flac",
                    path], check=True)
    print(path)


def repeat(y, seconds):
    """A loop played round for `seconds`, to hear its seam."""
    turns = int(np.ceil(seconds * RATE / y.shape[0]))
    return np.concatenate([y] * turns)[:int(seconds * RATE)]


def envelope(x, hz=30.0):
    return signal.filtfilt(*signal.butter(2, hz / (RATE / 2)), np.abs(x))


def events(x, job):
    """The recording's events for `job`, cleanest first, as (start, end) samples: each a run of
    the envelope above a sixth of the way from the recording's floor to its loudest, runs closer
    than the job's join joined, from just before the onset to the job's tail after it falls away,
    and no longer than the job allows. Cleanest is loudest over the floor."""
    j = ONESHOT_JOBS[job]
    join = int(j.get("join", 0.06) * RATE)
    env = envelope(x)
    floor = np.percentile(env, 20)
    above = env > floor + (env.max() - floor) / 6.0
    edges = np.diff(above.astype(np.int8))
    starts = list(np.where(edges == 1)[0] + 1)
    ends = list(np.where(edges == -1)[0] + 1)
    if above[0]:
        starts.insert(0, 0)
    if above[-1]:
        ends.append(x.size)
    runs = []
    for s, e in zip(starts, ends):
        if runs and s - runs[-1][1] < join:
            runs[-1][1] = e
        else:
            runs.append([s, e])
    found = []
    for s, e in runs:
        if e - s < int(0.01 * RATE):
            continue
        lead = max(0, s - int(ONESHOT_LEAD * RATE))
        end = min(x.size, e + int(j["tail"] * RATE), lead + int(j["longest"] * RATE))
        found.append((env[s:e].max() / max(floor, 1e-9), lead, end))
    found.sort(key=lambda f: -f[0])
    return [(s, e) for _, s, e in found[:j["per"]]]


def oneshot(x, span, job, highpass=None):
    """One event cut out, faded in over 3 ms and out over its last 40, high-passed (at the job's
    unless given), and levelled by its loud part."""
    s, e = span
    y = shape(x[s:e], 0.003, max(1, min(int(0.04 * RATE), (e - s) // 3)),
              ONESHOT_JOBS[job]["highpass"] if highpass is None else highpass)
    env = envelope(y, 60.0)
    return level(y, y[env > 0.25 * env.max()])


def take(name):
    """A ONESHOTS row's take."""
    job, sound, k = ONESHOTS[name]
    x = decode(sound)
    return oneshot(x, events(x, job)[k], job)


def hand_cut(name):
    """A CUTS row's stretch, shaped as a one-shot."""
    job, sound, start, end, highpass = CUTS[name]
    return oneshot(decode(sound), (int(start * RATE), int(end * RATE)), job, highpass)


def hits(y):
    """Where in a take the door hits its stop: every clack, in seconds from the take's start, with
    its loudness as a share of the take's loudest -- the peaks of a fast envelope, at least 50 ms
    apart and standing a tenth of the loudest above their surroundings."""
    env = envelope(y, 120.0)
    top = env.max()
    peaks, props = signal.find_peaks(env, height=0.2 * top, distance=int(0.05 * RATE),
                                     prominence=0.1 * top)
    strength = props["peak_heights"] / top
    keep = np.sort(np.argsort(-strength)[:DOOR_TAKE_HITS])
    return peaks[keep] / RATE, strength[keep]


def write_door_takes(takes):
    """door_takes.h: each door take's file, length and hits, as `takes` (file name -> its samples)
    has them, laid out as clang-format lays it so the commit hook leaves the file as written."""
    def f(v):
        return "%.4ff" % v

    lines = [
        "// Generated by apps/silent/tools/fetch_sounds.py -- do not edit; rerun it.",
        "// Included by haunt.c alone.",
        "#ifndef _SILENT_DOOR_TAKES_H_",
        "#define _SILENT_DOOR_TAKES_H_",
        "",
        "#define DOOR_TAKE_HITS %d" % DOOR_TAKE_HITS,
        "",
        "// A take at the basement door: its file under assets/audio/silent, its length in seconds,",
        "// and where in it the door hits its stop -- seconds from its start, and loudness as a share",
        "// of its loudest hit.",
        "typedef struct {",
        "    const char* file;",
        "    float seconds;",
        "    int hits;",
        "    float at[DOOR_TAKE_HITS];",
        "    float strength[DOOR_TAKE_HITS];",
        "} DoorTake;",
    ]
    for kind, names in DOOR_TAKES.items():
        lines += ["", "#define DOOR_%s_TAKES %d" % (kind, len(names)),
                  "static const DoorTake DOOR_%s[DOOR_%s_TAKES] = {" % (kind, kind)]
        for name in names:
            y = takes[name]
            at, strength = hits(y)
            lines += ["    {.file = \"%s.wav\"," % name,
                      "     .seconds = %s," % f(y.size / RATE),
                      "     .hits = %d," % at.size,
                      "     .at = {%s}," % ", ".join(f(v) for v in at),
                      "     .strength = {%s}}," % ", ".join(f(v) for v in strength)]
        lines += ["};"]
    lines += ["", "#endif // _SILENT_DOOR_TAKES_H_", ""]
    with open(DOOR_HEADER, "w") as out:
        out.write("\n".join(lines))
    if shutil.which("clang-format"):
        subprocess.run(["clang-format", "-i", DOOR_HEADER], check=True)
    print(os.path.relpath(DOOR_HEADER, ROOT))


def walk(takes):
    """Takes one after another WALK_GAP apart, in the order they are numbered: a footstep set as a
    walk on it sounds."""
    gap = int(WALK_GAP * RATE)
    out = np.zeros(gap * (len(takes) - 1) + max(t.size for t in takes))
    for k, t in enumerate(takes):
        out[k * gap:k * gap + t.size] += t
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--audition",
                        help="write every case preset and candidate loop here, to choose by ear")
    parser.add_argument("--only", nargs="+", metavar="JOB",
                        choices=["clock", *JOBS, *ONESHOT_JOBS,
                                 *sorted({cut[0] for cut in CUTS.values()})],
                        help="only these jobs -- clock, or a loop's or a one-shot's -- downloading "
                             "and writing nothing for the rest, whose files stay as they are")
    args = parser.parse_args()

    def wanted(job):
        return not args.only or job in args.only

    os.makedirs(CACHE_DIR, exist_ok=True)
    os.makedirs(OUT_DIR, exist_ok=True)
    if wanted("clock"):
        x = decode(CLOCK)
        pair = cleanest_pair(x)
        print("%s: beats at %.3f and %.3f s" % (credit(CLOCK), pair[0] / RATE, pair[1] / RATE))
    if args.audition:
        os.makedirs(args.audition, exist_ok=True)
        if wanted("clock"):
            for name, preset in CASES.items():
                t, k = make(x, pair, preset)
                write(os.path.join(args.audition, "clock_%s_10s.wav" % name), run(t, k))
        for job, sounds in CANDIDATES.items():
            if not wanted(job):
                continue
            for sound in sounds:
                y = loop(decode(sound, JOBS[job]["stereo"]), JOBS[job]["seconds"])
                write(os.path.join(args.audition, "%s_%d_%s_30s.wav" % (
                    job, sound, RECORDINGS[sound][1])), repeat(y, 30.0))
                print("%-6s %s" % (job, credit(sound)))
        for job, sounds in ONESHOT_CANDIDATES.items():
            if not wanted(job):
                continue
            for n, sound in enumerate(sounds):
                x = decode(sound)
                spans = events(x, job)
                takes = [oneshot(x, span, job) for span in spans]
                if job.startswith("step_"):
                    # A footstep is heard as a walk: one file a recording, not one a step, in a
                    # folder a surface, numbered in the candidates' order so one can be named.
                    folder = os.path.join(args.audition, job[len("step_"):])
                    os.makedirs(folder, exist_ok=True)
                    write(os.path.join(folder, "%d_%d_%s.wav" % (
                        n + 1, sound, RECORDINGS[sound][1])), walk(takes))
                    lengths = ", ".join("%.2f" % (y.size / RATE) for y in takes)
                    print("%-17s %d %d: %d steps, %s s" % (job, n + 1, sound, len(takes), lengths))
                    continue
                for k, (span, y) in enumerate(zip(spans, takes)):
                    write(os.path.join(args.audition, "%s_%d_%s_%d.wav" % (
                        job, sound, RECORDINGS[sound][1], k)), y)
                    print("%-6s %d event %d: %.2f s at %.2f s" % (
                        job, sound, k, y.size / RATE, span[0] / RATE))
        for name, (job, sound, start, end, _) in CUTS.items():
            if wanted(job):
                write(os.path.join(args.audition, name + ".wav"), hand_cut(name))
                print("%-6s %d, %.2f to %.2f s" % (job, sound, start, end))
        print(args.audition)
        return 0

    if wanted("clock"):
        tick, tock = make(x, pair, CASES[CASE])
        write(os.path.join(OUT_DIR, "tick.wav"), tick)
        write(os.path.join(OUT_DIR, "tock.wav"), tock)
        print(os.path.join(OUT_DIR, "tick.wav"), os.path.join(OUT_DIR, "tock.wav"))
    loops = {}
    for name, (job, sound) in LOOPS.items():
        if not wanted(job):
            continue
        print("%s: %s" % (name, credit(sound)))
        loops[name] = loop(decode(sound, JOBS[job]["stereo"]), JOBS[job]["seconds"])
        write_flac(os.path.join(OUT_DIR, name + ".flac"), loops[name])
    for name, (source, cut, gain) in MUFFLES.items():
        if source in loops:
            write_flac(os.path.join(OUT_DIR, name + ".flac"), muffle(loops[source], cut, gain))
    for name, (job, sound, k) in ONESHOTS.items():
        if not wanted(job):
            continue
        y = take(name)
        path = os.path.join(OUT_DIR, name + ".wav")
        write(path, y)
        print("%s: %s, event %d, %.2f s" % (path, credit(sound), k, y.size / RATE))
    door = [name for names in DOOR_TAKES.values() for name in names]
    if any(wanted(ONESHOTS[name][0]) for name in door):
        write_door_takes({name: take(name) for name in door})
    for name, (job, sound, start, end, _) in CUTS.items():
        if not wanted(job):
            continue
        y = hand_cut(name)
        path = os.path.join(OUT_DIR, name + ".wav")
        write(path, y)
        print("%s: %s, %.2f to %.2f s" % (path, credit(sound), start, end))
    return 0


if __name__ == "__main__":
    sys.exit(main())
