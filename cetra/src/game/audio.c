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

#define AUDIO_NOISE_AMPLITUDE 0.25
#define AUDIO_NOISE_SEED      0x7a1c

// What a Sound plays from: a file miniaudio decodes, or a source the Sound holds itself.
typedef enum SoundKind { SOUND_FILE, SOUND_TONE, SOUND_NOISE } SoundKind;

struct Sound {
    ma_sound sound;
    SoundKind kind;
    union {
        ma_waveform waveform; // SOUND_TONE
        ma_noise noise;       // SOUND_NOISE
    };
    bool continuous; // tone: no auto-stop, so it plays until stopped
    ma_uint64 beep_frames;
    AudioBus bus;       // what a voice copied from it is routed through
    AudioSystem* audio; // borrowed; the tone stop-time and free_sound reach the engine here
};

// One pooled voice: a copy of a decoded sound, playing once and reaped at its end.
typedef struct AudioVoice {
    ma_sound sound;
    bool live;
    uint32_t follow;  // the entity it rides, by id, or 0
    vec3 offset;      // in that entity's frame
    uint64_t started; // the order voices were started in, for stealing the oldest
} AudioVoice;

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
    AudioVoice voices[AUDIO_VOICE_MAX];
    uint64_t voices_started;
};

// The AUDIO_SOURCE component payload: up to AUDIO_SOURCE_MAX Sounds, each placed every frame
// at its own offset in the entity's frame.
typedef struct AudioSource {
    Sound* sounds[AUDIO_SOURCE_MAX]; // owned; released with the component
    vec3 offsets[AUDIO_SOURCE_MAX];
    int count;
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

static void sound_source_uninit(Sound* s) {
    if (s->kind == SOUND_TONE)
        ma_waveform_uninit(&s->waveform);
    else if (s->kind == SOUND_NOISE)
        ma_noise_uninit(&s->noise, NULL);
}

static void sound_destroy(Sound* s) {
    if (!s)
        return;
    ma_sound_uninit(&s->sound);
    sound_source_uninit(s);
    free(s);
}

// The playable sound over the source `s` already holds, tracked for teardown. 2D until
// positioned: a beep or a bed should not attenuate with the listener. On failure `s` and its
// source are released.
static Sound* sound_over_source(AudioSystem* audio, Sound* s, AudioBus bus, const char* what) {
    ma_data_source* source =
        s->kind == SOUND_TONE ? (ma_data_source*)&s->waveform : (ma_data_source*)&s->noise;
    s->bus = bus;
    if (ma_sound_init_from_data_source(&audio->engine, source, MA_SOUND_FLAG_NO_SPATIALIZATION,
                                       group_for(audio, bus), &s->sound) != MA_SUCCESS) {
        log_error("audio: %s sound init failed", what);
        sound_source_uninit(s);
        free(s);
        return NULL;
    }
    if (!track_sound(audio, s)) {
        sound_destroy(s);
        return NULL;
    }
    return s;
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
    for (int i = 0; i < AUDIO_VOICE_MAX; i++)
        if (audio->voices[i].live)
            ma_sound_uninit(&audio->voices[i].sound);
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

// A free voice, or the oldest playing one stopped and handed over.
static AudioVoice* take_voice(AudioSystem* audio) {
    AudioVoice* oldest = NULL;
    for (int i = 0; i < AUDIO_VOICE_MAX; i++) {
        AudioVoice* v = &audio->voices[i];
        if (!v->live)
            return v;
        if (!oldest || v->started < oldest->started)
            oldest = v;
    }
    ma_sound_uninit(&oldest->sound);
    oldest->live = false;
    return oldest;
}

static void place_voice(AudioVoice* v, struct Entity* e) {
    vec3 p;
    glm_vec3_copy(v->offset, p);
    if (e) {
        mat4 m = GLM_MAT4_IDENTITY_INIT;
        entity_get_transform_matrix(e, m);
        glm_mat4_mulv3(m, v->offset, 1.0f, p);
    }
    ma_sound_set_position(&v->sound, p[0], p[1], p[2]);
}

// Start a voice whose sound is initialised: its gain, rate and place from the desc.
static void start_voice(AudioSystem* audio, AudioVoice* v, const AudioVoiceDesc* d) {
    v->live = true;
    v->started = ++audio->voices_started;
    v->follow = d && d->follow ? d->follow->id : 0;
    glm_vec3_zero(v->offset);
    if (d)
        glm_vec3_copy((float*)d->position, v->offset);
    ma_sound_set_volume(&v->sound, d && d->volume > 0.0f ? d->volume : 1.0f);
    ma_sound_set_pitch(&v->sound, d && d->pitch > 0.0f ? d->pitch : 1.0f);
    if (d && !d->flat)
        place_voice(v, (struct Entity*)d->follow);
    ma_sound_start(&v->sound);
}

bool audio_play_voice(AudioSystem* audio, const Sound* prototype, const AudioVoiceDesc* desc) {
    if (!audio || !prototype)
        return false;
    if (prototype->kind != SOUND_FILE) {
        log_error("audio_play_voice: a voice copies a sound from a file; this one is generated");
        return false;
    }
    AudioVoice* v = take_voice(audio);
    const ma_uint32 flags = desc && !desc->flat ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_copy(&audio->engine, &prototype->sound, flags,
                           group_for(audio, prototype->bus), &v->sound) != MA_SUCCESS) {
        log_error("audio_play_voice: could not copy the sound");
        return false;
    }
    start_voice(audio, v, desc);
    return true;
}

void audio_play_oneshot(AudioSystem* audio, const char* path, AudioBus bus) {
    if (!audio || !path)
        return;
    AudioVoice* v = take_voice(audio);
    if (ma_sound_init_from_file(&audio->engine, path,
                                MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                group_for(audio, bus), NULL, &v->sound) != MA_SUCCESS) {
        log_error("audio: could not play %s", path);
        return;
    }
    start_voice(audio, v, &(AudioVoiceDesc){.flat = true});
}

Sound* audio_play_music(AudioSystem* audio, const char* path, bool loop) {
    if (!audio || !path)
        return NULL;
    Sound* s = calloc(1, sizeof(Sound));
    if (!s)
        return NULL;
    s->audio = audio;
    s->bus = AUDIO_BUS_MUSIC;
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
    s->bus = bus;
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
    s->kind = SOUND_TONE;
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
    return sound_over_source(audio, s, bus, "tone");
}

Sound* audio_sound_from_noise(AudioSystem* audio, AudioNoise colour, AudioBus bus) {
    if (!audio)
        return NULL;
    Sound* s = calloc(1, sizeof(Sound));
    if (!s)
        return NULL;
    s->audio = audio;
    s->kind = SOUND_NOISE;
    const ma_noise_type type = colour == AUDIO_NOISE_PINK    ? ma_noise_type_pink
                               : colour == AUDIO_NOISE_BROWN ? ma_noise_type_brownian
                                                             : ma_noise_type_white;
    // Mono, so a positioned bed spatializes to stereo, as a tone does.
    ma_noise_config nc =
        ma_noise_config_init(ma_format_f32, 1, type, AUDIO_NOISE_SEED, AUDIO_NOISE_AMPLITUDE);
    if (ma_noise_init(&nc, NULL, &s->noise) != MA_SUCCESS) {
        log_error("audio: noise init failed");
        free(s);
        return NULL;
    }
    return sound_over_source(audio, s, bus, "noise");
}

void audio_sound_play(Sound* sound) {
    if (!sound)
        return;
    ma_sound_seek_to_pcm_frame(&sound->sound, 0);
    if (sound->kind == SOUND_TONE && !sound->continuous) {
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
    if (sound->kind == SOUND_TONE)
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
    if (!src)
        return;
    mat4 m = GLM_MAT4_IDENTITY_INIT;
    entity_get_transform_matrix(entity, m);
    for (int i = 0; i < src->count; i++) {
        vec3 p;
        glm_mat4_mulv3(m, src->offsets[i], 1.0f, p);
        ma_sound_set_position(&src->sounds[i]->sound, p[0], p[1], p[2]);
    }
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
    // Voices are reaped here, on the main thread, rather than from miniaudio's end callback,
    // which runs on the mixing thread and may not uninit the sound it is called for. A voice
    // whose entity has gone stays where it last was.
    for (int i = 0; i < AUDIO_VOICE_MAX; i++) {
        AudioVoice* v = &audio->voices[i];
        if (!v->live)
            continue;
        if (ma_sound_at_end(&v->sound)) {
            ma_sound_uninit(&v->sound);
            v->live = false;
            continue;
        }
        Entity* e = v->follow && em ? find_entity_by_id(em, v->follow) : NULL;
        if (e)
            place_voice(v, e);
    }
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
    for (int i = 0; i < src->count; i++)
        free_sound(src->sounds[i]);
    free(src);
}

Sound* entity_add_audio_source_at(struct Entity* entity, Sound* sound, const vec3 offset) {
    if (!entity || !sound || !offset)
        return NULL;
    AudioSource* src = (AudioSource*)entity_get_component(entity, COMPONENT_AUDIO_SOURCE);
    if (!src) {
        src = calloc(1, sizeof(AudioSource));
        if (!src)
            return NULL;
        entity_add_component(entity, COMPONENT_AUDIO_SOURCE, src);
        entity_set_component_free(entity, COMPONENT_AUDIO_SOURCE, audio_source_free);
    }
    if (src->count >= AUDIO_SOURCE_MAX) {
        log_error("Entity '%s' already holds %d sounds", entity->name, AUDIO_SOURCE_MAX);
        return NULL;
    }
    src->sounds[src->count] = sound;
    glm_vec3_copy((float*)offset, src->offsets[src->count]);
    src->count++;
    mat4 m = GLM_MAT4_IDENTITY_INIT;
    vec3 p;
    entity_get_transform_matrix(entity, m);
    glm_mat4_mulv3(m, (float*)offset, 1.0f, p);
    audio_sound_set_position(sound, p);
    return sound;
}

Sound* entity_add_audio_source(struct Entity* entity, Sound* sound) {
    return entity_add_audio_source_at(entity, sound, (vec3){0.0f, 0.0f, 0.0f});
}

int entity_audio_source_count(struct Entity* entity) {
    AudioSource* src =
        entity ? (AudioSource*)entity_get_component(entity, COMPONENT_AUDIO_SOURCE) : NULL;
    return src ? src->count : 0;
}

Sound* entity_get_audio_source(struct Entity* entity, int index) {
    AudioSource* src =
        entity ? (AudioSource*)entity_get_component(entity, COMPONENT_AUDIO_SOURCE) : NULL;
    return src && index >= 0 && index < src->count ? src->sounds[index] : NULL;
}

void entity_remove_audio_source(struct Entity* entity, Sound* sound) {
    AudioSource* src =
        entity ? (AudioSource*)entity_get_component(entity, COMPONENT_AUDIO_SOURCE) : NULL;
    if (!src || !sound)
        return;
    for (int i = 0; i < src->count; i++) {
        if (src->sounds[i] != sound)
            continue;
        free_sound(sound);
        for (int j = i + 1; j < src->count; j++) {
            src->sounds[j - 1] = src->sounds[j];
            glm_vec3_copy(src->offsets[j], src->offsets[j - 1]);
        }
        src->count--;
        return;
    }
}
