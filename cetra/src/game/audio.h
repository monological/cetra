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

struct Engine;
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

// noDevice iff engine->headless; NULL on failure.
AudioSystem* create_audio_system(const struct Engine* engine);
void free_audio_system(AudioSystem* audio);

// Linear gain, 1 = unity. MASTER scales the whole mix; the rest scale their bus.
void audio_set_bus_volume(AudioSystem* audio, AudioBus bus, float volume);

// Fire-and-forget 2D: miniaudio owns the voice and reaps it at the end.
void audio_play_oneshot(AudioSystem* audio, const char* path, AudioBus bus);
// A held music voice, streamed and unspatialized; returned so a game can stop it.
Sound* audio_play_music(AudioSystem* audio, const char* path, bool loop);

// Held voices. from_file decodes WAV/MP3/FLAC; from_tone is a procedural sine,
// which needs no asset and is what the demo and the gate play. A tone is 2D
// (centred, no attenuation) until audio_sound_set_position turns spatialization
// on. Both return NULL on failure.
Sound* audio_sound_from_file(AudioSystem* audio, const char* path, AudioBus bus);
Sound* audio_sound_from_tone(AudioSystem* audio, float hz, AudioBus bus);
// Plays from the start, so calling it again re-triggers. A tone stops itself
// after a short beep unless set_looping made it continuous.
void audio_sound_play(Sound* sound);
void audio_sound_stop(Sound* sound);
// For a file: ma looping. For a tone: continuous (no auto-stop) when true.
void audio_sound_set_looping(Sound* sound, bool loop);
void audio_sound_set_volume(Sound* sound, float volume);
// Places the voice in the world and enables 3D attenuation and panning.
void audio_sound_set_position(Sound* sound, vec3 world_pos);
void free_sound(AudioSystem* audio, Sound* sound);

// Once per rendered frame: point the listener along the camera pose, reap
// finished one-shots, and push each AUDIO_SOURCE component's position from its
// entity (em may be NULL when there are no entities).
void audio_system_update(AudioSystem* audio, struct EntityManager* em, vec3 listener_pos,
                         vec3 forward, vec3 up);

// Headless (noDevice) only: render `frames` interleaved stereo frames (2 floats
// each) into `out`. Returns frames produced; 0 in device mode. The gate's readout.
size_t audio_system_read_pcm(AudioSystem* audio, float* out, size_t frames);

// The AUDIO_SOURCE entity component (spec 12.0): attach a Sound (from a file or
// a tone) to an entity, and its world position is pushed from the entity each
// frame. Spatialization is turned on. The component takes ownership of the
// Sound and releases it on teardown. Returns the sound, or NULL on failure.
Sound* entity_add_audio_source(struct Entity* entity, AudioSystem* audio, Sound* sound);
Sound* entity_get_audio_source(struct Entity* entity);

#endif // _AUDIO_H_
