// The single translation unit that emits miniaudio's implementation (the
// stb-in-import.c pattern). Everything above the device is here; nothing else
// in the tree includes miniaudio.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING // playback only -- we decode, never write
#include "../ext/miniaudio.h"

#include "audio.h"
#include "entity.h"
#include "../util.h"
#include "../ext/log.h"

#include <stdlib.h>

#define AUDIO_OFFLINE_SAMPLE_RATE 48000
#define AUDIO_OFFLINE_CHANNELS    2
#define AUDIO_TONE_AMPLITUDE      0.25
#define AUDIO_TONE_BEEP_SECONDS   0.18

struct Sound {
    ma_sound sound;
    ma_waveform waveform; // backs a tone; unused for a file
    bool is_tone;
    bool continuous; // tone: no auto-stop, so it plays until stopped
    ma_uint64 beep_frames;
    AudioSystem* audio; // borrowed; the tone stop-time and free_sound reach the engine here
};

struct AudioSystem {
    ma_engine engine;
    bool no_device;                         // headless: rendered offline, no device
    ma_sound_group groups[AUDIO_BUS_COUNT]; // MASTER slot unused; MUSIC/SFX/UI are groups
    // What each bus was last set to. Kept because miniaudio is write-only here
    // -- there is no ma_engine_get_volume -- and a settings slider needs a value
    // to open at. Seeded to unity at creation rather than left at calloc's zero,
    // which would mean silence.
    float volumes[AUDIO_BUS_COUNT];
    Sound** sounds; // every held Sound, for teardown
    size_t sound_count;
    size_t sound_cap;
};

// The AUDIO_SOURCE component payload: a Sound placed at its entity every frame.
typedef struct AudioSource {
    Sound* sound; // owned; released with the component (the Sound knows its system)
} AudioSource;

// MASTER routes to the engine endpoint (NULL group); the rest to their group.
static ma_sound_group* group_for(AudioSystem* audio, AudioBus bus) {
    if (bus > AUDIO_BUS_MASTER && bus < AUDIO_BUS_COUNT)
        return &audio->groups[bus];
    return NULL;
}

static bool track_sound(AudioSystem* audio, Sound* s) {
    if (!grow_array((void**)&audio->sounds, &audio->sound_cap, audio->sound_count + 1,
                    sizeof(Sound*), 8))
        return false;
    audio->sounds[audio->sound_count++] = s;
    return true;
}

static void sound_destroy(Sound* s) {
    if (!s)
        return;
    ma_sound_uninit(&s->sound);
    if (s->is_tone)
        ma_waveform_uninit(&s->waveform);
    free(s);
}

AudioSystem* create_audio_system(bool headless) {
    AudioSystem* audio = calloc(1, sizeof(AudioSystem));
    if (!audio)
        return NULL;

    audio->no_device = headless;
    for (int bus = AUDIO_BUS_MASTER; bus < AUDIO_BUS_COUNT; bus++)
        audio->volumes[bus] = 1.0f;

    ma_engine_config cfg = ma_engine_config_init();
    if (audio->no_device) {
        cfg.noDevice = MA_TRUE;
        cfg.channels = AUDIO_OFFLINE_CHANNELS; // must be set with no device
        cfg.sampleRate = AUDIO_OFFLINE_SAMPLE_RATE;
    }

    if (ma_engine_init(&cfg, &audio->engine) != MA_SUCCESS) {
        log_error("audio: engine init failed");
        free(audio);
        return NULL;
    }

    for (int bus = AUDIO_BUS_MASTER + 1; bus < AUDIO_BUS_COUNT; bus++) {
        if (ma_sound_group_init(&audio->engine, 0, NULL, &audio->groups[bus]) != MA_SUCCESS) {
            log_error("audio: sound group %d init failed", bus);
            for (int done = AUDIO_BUS_MASTER + 1; done < bus; done++)
                ma_sound_group_uninit(&audio->groups[done]);
            ma_engine_uninit(&audio->engine);
            free(audio);
            return NULL;
        }
    }

    log_info("audio: %s, %u ch @ %u Hz", audio->no_device ? "offline (no device)" : "device",
             ma_engine_get_channels(&audio->engine), ma_engine_get_sample_rate(&audio->engine));
    return audio;
}

void free_audio_system(AudioSystem* audio) {
    if (!audio)
        return;
    for (size_t i = 0; i < audio->sound_count; i++)
        sound_destroy(audio->sounds[i]);
    free(audio->sounds);
    for (int bus = AUDIO_BUS_MASTER + 1; bus < AUDIO_BUS_COUNT; bus++)
        ma_sound_group_uninit(&audio->groups[bus]);
    ma_engine_uninit(&audio->engine);
    free(audio);
}

float audio_get_bus_volume(const AudioSystem* audio, AudioBus bus) {
    if (!audio || bus < AUDIO_BUS_MASTER || bus >= AUDIO_BUS_COUNT)
        return 1.0f;
    return audio->volumes[bus];
}

void audio_set_bus_volume(AudioSystem* audio, AudioBus bus, float volume) {
    // The bus is checked HERE and not only in group_for, which is where the
    // check used to be enough: recording the value writes into a fixed array
    // before that call is reached, so an out-of-range bus wrote past it.
    if (!audio || bus < AUDIO_BUS_MASTER || bus >= AUDIO_BUS_COUNT)
        return;
    audio->volumes[bus] = volume;
    if (bus == AUDIO_BUS_MASTER) {
        ma_engine_set_volume(&audio->engine, volume);
        return;
    }
    ma_sound_group* group = group_for(audio, bus);
    if (group)
        ma_sound_group_set_volume(group, volume);
}

void audio_play_oneshot(AudioSystem* audio, const char* path, AudioBus bus) {
    if (!audio || !path)
        return;
    if (ma_engine_play_sound(&audio->engine, path, group_for(audio, bus)) != MA_SUCCESS)
        log_error("audio: could not play %s", path);
}

Sound* audio_play_music(AudioSystem* audio, const char* path, bool loop) {
    if (!audio || !path)
        return NULL;
    Sound* s = calloc(1, sizeof(Sound));
    if (!s)
        return NULL;
    s->audio = audio;
    ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_file(&audio->engine, path, flags, group_for(audio, AUDIO_BUS_MUSIC),
                                NULL, &s->sound) != MA_SUCCESS) {
        log_error("audio: could not open music %s", path);
        free(s);
        return NULL;
    }
    ma_sound_set_looping(&s->sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_start(&s->sound);
    if (!track_sound(audio, s)) {
        sound_destroy(s);
        return NULL;
    }
    return s;
}

Sound* audio_sound_from_file(AudioSystem* audio, const char* path, AudioBus bus) {
    if (!audio || !path)
        return NULL;
    Sound* s = calloc(1, sizeof(Sound));
    if (!s)
        return NULL;
    s->audio = audio;
    if (ma_sound_init_from_file(&audio->engine, path, MA_SOUND_FLAG_DECODE, group_for(audio, bus),
                                NULL, &s->sound) != MA_SUCCESS) {
        log_error("audio: could not load %s", path);
        free(s);
        return NULL;
    }
    if (!track_sound(audio, s)) {
        sound_destroy(s);
        return NULL;
    }
    return s;
}

Sound* audio_sound_from_tone(AudioSystem* audio, float hz, AudioBus bus) {
    if (!audio)
        return NULL;
    Sound* s = calloc(1, sizeof(Sound));
    if (!s)
        return NULL;
    s->audio = audio;
    s->is_tone = true;
    ma_uint32 rate = ma_engine_get_sample_rate(&audio->engine);
    s->beep_frames = (ma_uint64)(AUDIO_TONE_BEEP_SECONDS * rate);

    // Mono, so a positioned tone spatializes to stereo.
    ma_waveform_config wc = ma_waveform_config_init(ma_format_f32, 1, rate, ma_waveform_type_sine,
                                                    AUDIO_TONE_AMPLITUDE, hz);
    if (ma_waveform_init(&wc, &s->waveform) != MA_SUCCESS) {
        log_error("audio: tone init failed");
        free(s);
        return NULL;
    }
    // 2D until positioned: a beep should not attenuate with the listener.
    if (ma_sound_init_from_data_source(&audio->engine, &s->waveform,
                                       MA_SOUND_FLAG_NO_SPATIALIZATION, group_for(audio, bus),
                                       &s->sound) != MA_SUCCESS) {
        log_error("audio: tone sound init failed");
        ma_waveform_uninit(&s->waveform);
        free(s);
        return NULL;
    }
    if (!track_sound(audio, s)) {
        sound_destroy(s);
        return NULL;
    }
    return s;
}

void audio_sound_play(Sound* sound) {
    if (!sound)
        return;
    ma_sound_seek_to_pcm_frame(&sound->sound, 0);
    if (sound->is_tone && !sound->continuous) {
        ma_uint64 now = ma_engine_get_time_in_pcm_frames(&sound->audio->engine);
        ma_sound_set_stop_time_in_pcm_frames(&sound->sound, now + sound->beep_frames);
    }
    ma_sound_start(&sound->sound);
}

void audio_sound_stop(Sound* sound) {
    if (sound)
        ma_sound_stop(&sound->sound);
}

void audio_sound_set_looping(Sound* sound, bool loop) {
    if (!sound)
        return;
    if (sound->is_tone)
        sound->continuous = loop; // an endless waveform needs no auto-stop to loop
    else
        ma_sound_set_looping(&sound->sound, loop ? MA_TRUE : MA_FALSE);
}

void audio_sound_set_volume(Sound* sound, float volume) {
    if (sound)
        ma_sound_set_volume(&sound->sound, volume);
}

void audio_sound_set_position(Sound* sound, vec3 world_pos) {
    if (!sound)
        return;
    ma_sound_set_spatialization_enabled(&sound->sound, MA_TRUE);
    ma_sound_set_position(&sound->sound, world_pos[0], world_pos[1], world_pos[2]);
}

void free_sound(Sound* sound) {
    if (!sound)
        return;
    AudioSystem* audio = sound->audio;
    for (size_t i = 0; i < audio->sound_count; i++) {
        if (audio->sounds[i] == sound) {
            audio->sounds[i] = audio->sounds[--audio->sound_count];
            break;
        }
    }
    sound_destroy(sound);
}

static void audio_sync_source_cb(Entity* entity, void* user_data) {
    (void)user_data;
    AudioSource* src = (AudioSource*)entity_get_component(entity, COMPONENT_AUDIO_SOURCE);
    if (src && src->sound)
        ma_sound_set_position(&src->sound->sound, entity->position[0], entity->position[1],
                              entity->position[2]);
}

void audio_system_update(AudioSystem* audio, struct EntityManager* em, vec3 listener_pos,
                         vec3 forward, vec3 up) {
    if (!audio)
        return;
    ma_engine_listener_set_position(&audio->engine, 0, listener_pos[0], listener_pos[1],
                                    listener_pos[2]);
    ma_engine_listener_set_direction(&audio->engine, 0, forward[0], forward[1], forward[2]);
    ma_engine_listener_set_world_up(&audio->engine, 0, up[0], up[1], up[2]);
    if (em)
        entity_manager_foreach_with(em, COMPONENT_BIT(COMPONENT_AUDIO_SOURCE), audio_sync_source_cb,
                                    NULL);
}

size_t audio_system_read_pcm(AudioSystem* audio, float* out, size_t frames) {
    if (!audio || !audio->no_device || !out)
        return 0;
    ma_uint64 read = 0;
    ma_engine_read_pcm_frames(&audio->engine, out, frames, &read);
    return (size_t)read;
}

static void audio_source_free(void* data) {
    AudioSource* src = (AudioSource*)data;
    if (!src)
        return;
    if (src->sound)
        free_sound(src->sound);
    free(src);
}

Sound* entity_add_audio_source(struct Entity* entity, Sound* sound) {
    if (!entity || !sound)
        return NULL;
    audio_sound_set_position(sound, entity->position);
    AudioSource* src = calloc(1, sizeof(AudioSource));
    if (!src)
        return NULL;
    src->sound = sound;
    entity_add_component(entity, COMPONENT_AUDIO_SOURCE, src);
    entity_set_component_free(entity, COMPONENT_AUDIO_SOURCE, audio_source_free);
    return sound;
}

Sound* entity_get_audio_source(struct Entity* entity) {
    if (!entity)
        return NULL;
    AudioSource* src = (AudioSource*)entity_get_component(entity, COMPONENT_AUDIO_SOURCE);
    return src ? src->sound : NULL;
}
