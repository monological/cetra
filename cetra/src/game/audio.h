#ifndef _AUDIO_H_
#define _AUDIO_H_

/*
 * The game layer's audio (spec 12.0): one output device wrapping miniaudio's
 * high-level engine, 2D fire-and-forget SFX and music, and 3D positional sound
 * with the camera as the listener. A Game subsystem like the physics world --
 * built by the app, installed with game_set_audio_system, owned and freed by
 * the framework (after the entity manager, so the AUDIO_SOURCE components whose
 * sounds live in the engine tear down while the engine is still alive).
 *
 * The device is the seam: a windowed run opens the OS device and mixes on
 * miniaudio's own thread; a headless run opens NO device and renders offline
 * through audio_system_read_pcm, so the whole layer above the device is a pure
 * deterministic function of the frames pulled -- which is what the `audio` gate
 * group asserts on. The real device path (playback quality, other OSes) is owed
 * a listen; see docs/verification.md.
 */

#include <stdbool.h>
#include <stddef.h>
#include <cglm/types.h>

struct EntityManager;
struct Entity;

typedef struct AudioSystem AudioSystem; // owns the miniaudio engine device
typedef struct Sound Sound;             // one playable voice

// The mixer buses. MASTER is the engine's own gain; the other three are groups
// a game routes SFX, music and UI through so each has one volume.
typedef enum {
    AUDIO_BUS_MASTER = 0,
    AUDIO_BUS_MUSIC,
    AUDIO_BUS_SFX,
    AUDIO_BUS_UI,
    AUDIO_BUS_COUNT
} AudioBus;

// The colours of procedural noise, by their slope: white is flat, pink falls 3 dB an octave
// and brown 6 -- hiss, the patter of rain, and a rumble.
typedef enum { AUDIO_NOISE_WHITE, AUDIO_NOISE_PINK, AUDIO_NOISE_BROWN } AudioNoise;

// A headless game opens no device and renders offline; NULL on failure.
AudioSystem* create_audio_system(bool headless);
void free_audio_system(AudioSystem* audio);

// Linear gain, 1 = unity. MASTER scales the whole mix; the rest scale their bus.
void audio_set_bus_volume(AudioSystem* audio, AudioBus bus, float volume);
// What that bus was last set to, 1 until something sets it. miniaudio is
// write-only here -- there is no ma_engine_get_volume for the master -- so this
// reads a value the setter records rather than asking the mixer. A settings
// screen needs it: a slider with nothing to initialise from opens at whatever
// the widget happened to default to and silently rewrites the real volume the
// first time it is touched.
float audio_get_bus_volume(const AudioSystem* audio, AudioBus bus);

// Fire-and-forget 2D: miniaudio owns the voice and reaps it at the end.
void audio_play_oneshot(AudioSystem* audio, const char* path, AudioBus bus);

// The pool positional fire-and-forget voices come from. Full, a voice that has played out is
// taken, else the oldest still playing is stopped for the new one: a headless run that pulls
// no PCM never reaches a voice's end, so nothing would ever come free otherwise.
#define AUDIO_VOICE_MAX 32

// A voice played from a sound, as zero means the default named here.
typedef struct AudioVoiceDesc {
    vec3 position;               // world, or in `follow`'s frame when it follows an entity
    const struct Entity* follow; // carried by this entity while it lives, or NULL to stay put
    float volume;                // linear gain; 0 is 1
} AudioVoiceDesc;

// Play a positioned copy of a sound decoded from a file, on that sound's bus, once, and reap
// it at its end. Any number may overlap, and none decodes anything: the copies share the
// prototype's samples. The prototype keeps its own volume and position, which a voice does not
// take. False, logged, for a sound generated or streamed rather than decoded.
bool audio_play_voice(AudioSystem* audio, const Sound* prototype, const AudioVoiceDesc* desc);
// A held music voice, streamed and unspatialized; returned so a game can stop it.
Sound* audio_play_music(AudioSystem* audio, const char* path, bool loop);

// Held voices. from_file decodes WAV/MP3/FLAC; from_tone is a procedural sine,
// which needs no asset and is what the demo and the gate play. A tone is 2D
// (centred, no attenuation) until audio_sound_set_position turns spatialization
// on. Both return NULL on failure.
Sound* audio_sound_from_file(AudioSystem* audio, const char* path, AudioBus bus);
Sound* audio_sound_from_tone(AudioSystem* audio, float hz, AudioBus bus);
// Endless procedural noise, 2D until positioned like a tone, from a fixed seed so an offline
// render is the same every run: a bed -- rain, wind, a room's hum -- rather than an event.
Sound* audio_sound_from_noise(AudioSystem* audio, AudioNoise colour, AudioBus bus);
// Plays from the start, so calling it again re-triggers. A tone stops itself
// after a short beep unless set_looping made it continuous.
void audio_sound_play(Sound* sound);
void audio_sound_stop(Sound* sound);
// For a file: ma looping. For a tone: continuous (no auto-stop) when true.
void audio_sound_set_looping(Sound* sound, bool loop);
void audio_sound_set_volume(Sound* sound, float volume);
// Places the voice in the world and enables 3D attenuation and panning.
void audio_sound_set_position(Sound* sound, vec3 world_pos);
void free_sound(Sound* sound);

// Once per rendered frame: point the listener along the camera pose, push each AUDIO_SOURCE
// sound's position from its entity, carry the voices that follow one, and reap the voices that
// have played out (em may be NULL when there are no entities).
void audio_system_update(AudioSystem* audio, struct EntityManager* em, vec3 listener_pos,
                         vec3 forward, vec3 up);

// Headless (noDevice) only: render `frames` interleaved stereo frames (2 floats
// each) into `out`. Returns frames produced; 0 in device mode. The gate's readout.
size_t audio_system_read_pcm(AudioSystem* audio, float* out, size_t frames);

// The AUDIO_SOURCE entity component (spec 12.0): attach a Sound (from a file or
// a tone) to an entity, and its world position is pushed from the entity each
// frame -- at an offset in the entity's frame, since spec 13.17, or at its origin.
// Spatialization is turned on. The component takes ownership of the Sound and
// releases it on teardown. Returns the sound, or NULL on failure or when the
// entity has a source already.
Sound* entity_add_audio_source(struct Entity* entity, Sound* sound);
Sound* entity_add_audio_source_at(struct Entity* entity, Sound* sound, const vec3 offset);
Sound* entity_get_audio_source(struct Entity* entity);

#endif // _AUDIO_H_
