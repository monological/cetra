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

#include <math.h>
#include <stdlib.h>
#include <string.h>

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
    bool streamed;      // read from the file as it plays, so nothing decoded to copy
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

#define AUDIO_ZONE_NAME 32
// A zone gain this close to its target is taken as there, so a settled sound is exactly it.
#define AUDIO_ZONE_LANDED 1e-4f

// The rooms and what passes between them (audio.h, "Zones").
typedef struct AudioZones {
    int count; // zones, the world included, so 1 when none was added
    char names[AUDIO_ZONE_MAX][AUDIO_ZONE_NAME];
    AABB boxes[AUDIO_ZONE_MAX][AUDIO_ZONE_BOXES];
    int box_counts[AUDIO_ZONE_MAX];
    int link_count;
    AudioZone link_ends[AUDIO_ZONE_LINKS][2];
    float link_through[AUDIO_ZONE_LINKS];
    // The best path's gain between every pair of zones: 1 from a zone to itself, 0 where no
    // chain of links joins two.
    float path[AUDIO_ZONE_MAX][AUDIO_ZONE_MAX];
} AudioZones;

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
    AudioZones zones;
    AudioZone listener_zone;     // where the listener was at the last update
    float heard[AUDIO_ZONE_MAX]; // each zone's best path to the listener's, eased
    bool updated;                // an update has run, so zone gains ease rather than land
};

// The AUDIO_SOURCE component payload: a Sound placed every frame at an offset in its entity's
// frame.
typedef struct AudioSource {
    Sound* sound; // owned; released with the component (the Sound knows its system)
    vec3 offset;
} AudioSource;

// Where a point in an entity's frame is in the world.
static void entity_point(const Entity* e, const vec3 local, vec3 out) {
    mat4 m = GLM_MAT4_IDENTITY_INIT;
    entity_get_transform_matrix(e, m);
    glm_mat4_mulv3(m, (float*)local, 1.0f, out);
}

// The best path between every pair of zones, by Floyd-Warshall over the max-product semiring.
// No `through` is above 1, so going round a cycle never improves a path and the closure is
// exact. Two links between one pair -- a wall and a door through it -- count as the better.
static void zones_solve(AudioZones* z) {
    const int n = z->count;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            z->path[i][j] = i == j ? 1.0f : 0.0f;
    for (int l = 0; l < z->link_count; l++) {
        const AudioZone a = z->link_ends[l][0], b = z->link_ends[l][1];
        const float t = z->link_through[l];
        if (t > z->path[a][b]) {
            z->path[a][b] = t;
            z->path[b][a] = t;
        }
    }
    for (int k = 0; k < n; k++)
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                const float via = z->path[i][k] * z->path[k][j];
                if (via > z->path[i][j])
                    z->path[i][j] = via;
            }
}

// The zone `p` is in: the last added whose boxes hold it, else the world.
static AudioZone zone_find(const AudioZones* zones, const vec3 p) {
    for (int z = zones->count - 1; z > (int)AUDIO_ZONE_WORLD; z--)
        for (int i = 0; i < zones->box_counts[z]; i++)
            if (aabb_dist_sq(&zones->boxes[z][i], p) == 0.0f)
                return (AudioZone)z;
    return AUDIO_ZONE_WORLD;
}

// A placed sound's zone gain eased by `k` toward what its zone lets through to the listener's, 1
// to land it there. The gain is the sound's output-bus volume, a stage the volume the app sets
// never touches -- the two multiply -- so the sound itself holds it and it is read back here.
static void zone_ease(const AudioSystem* audio, ma_sound* sound, float k) {
    const ma_vec3f at = ma_sound_get_position(sound);
    const AudioZone z = zone_find(&audio->zones, (vec3){at.x, at.y, at.z});
    const float target = audio->zones.path[audio->listener_zone][z];
    const float gain = ma_node_get_output_bus_volume(sound, 0);
    float eased = gain + (target - gain) * k;
    if (fabsf(target - eased) < AUDIO_ZONE_LANDED)
        eased = target;
    if (eased != gain)
        ma_node_set_output_bus_volume(sound, 0, eased);
}

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
    audio->zones.count = 1; // the world alone
    strcpy(audio->zones.names[AUDIO_ZONE_WORLD], "world");
    zones_solve(&audio->zones);
    audio->heard[AUDIO_ZONE_WORLD] = 1.0f; // the listener starts in the world, which hears itself

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
static void release_voice(AudioVoice* v) {
    ma_sound_uninit(&v->sound);
    v->live = false;
}

// A free voice; else one that has played out and is not yet reaped -- an offline render ends
// voices between updates -- else the oldest still playing, stopped and handed over.
static AudioVoice* take_voice(AudioSystem* audio) {
    AudioVoice *oldest = NULL, *ended = NULL;
    for (int i = 0; i < AUDIO_VOICE_MAX; i++) {
        AudioVoice* v = &audio->voices[i];
        if (!v->live)
            return v;
        if (!ended && ma_sound_at_end(&v->sound))
            ended = v;
        if (!oldest || v->started < oldest->started)
            oldest = v;
    }
    AudioVoice* v = ended ? ended : oldest;
    release_voice(v);
    return v;
}

static void place_voice(AudioVoice* v, const Entity* e) {
    vec3 p = {0.0f, 0.0f, 0.0f};
    entity_point(e, v->offset, p);
    ma_sound_set_position(&v->sound, p[0], p[1], p[2]);
}

bool audio_play_voice(AudioSystem* audio, const Sound* prototype, const AudioVoiceDesc* desc) {
    if (!audio || !prototype || !desc)
        return false;
    if (prototype->kind != SOUND_FILE || prototype->streamed) {
        log_error("audio_play_voice: a voice copies a sound decoded from a file; this one is %s",
                  prototype->streamed ? "streamed" : "generated");
        return false;
    }
    AudioVoice* v = take_voice(audio);
    if (ma_sound_init_copy(&audio->engine, &prototype->sound, 0, group_for(audio, prototype->bus),
                           &v->sound) != MA_SUCCESS) {
        log_error("audio_play_voice: could not copy the sound");
        return false;
    }
    v->live = true;
    v->started = ++audio->voices_started;
    v->follow = desc->follow ? desc->follow->id : 0;
    glm_vec3_copy((float*)desc->position, v->offset);
    ma_sound_set_volume(&v->sound, desc->volume > 0.0f ? desc->volume : 1.0f);
    place_voice(v, desc->follow);
    // Heard through its zone from the first sample, not faded in from whole.
    zone_ease(audio, &v->sound, 1.0f);
    ma_sound_start(&v->sound);
    return true;
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
    s->bus = AUDIO_BUS_MUSIC;
    s->streamed = true;
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
    // 2D until placed, as a tone is: spatialized from the start, it would sit at the world origin
    // and fall off with the listener's distance from there.
    const ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_file(&audio->engine, path, flags, group_for(audio, bus), NULL,
                                &s->sound) != MA_SUCCESS) {
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
    const bool placed = ma_sound_is_spatialization_enabled(&sound->sound);
    ma_sound_set_spatialization_enabled(&sound->sound, MA_TRUE);
    ma_sound_set_position(&sound->sound, world_pos[0], world_pos[1], world_pos[2]);
    // Heard through its zone from the moment it is placed, not faded in from whole.
    if (!placed)
        zone_ease(sound->audio, &sound->sound, 1.0f);
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
    if (!src || !src->sound)
        return;
    vec3 p = {0.0f, 0.0f, 0.0f};
    entity_point(entity, src->offset, p);
    ma_sound_set_position(&src->sound->sound, p[0], p[1], p[2]);
}

void audio_system_update(AudioSystem* audio, struct EntityManager* em, vec3 listener_pos,
                         vec3 forward, vec3 up, float dt) {
    if (!audio)
        return;
    ma_engine_listener_set_position(&audio->engine, 0, listener_pos[0], listener_pos[1],
                                    listener_pos[2]);
    ma_engine_listener_set_direction(&audio->engine, 0, forward[0], forward[1], forward[2]);
    ma_engine_listener_set_world_up(&audio->engine, 0, up[0], up[1], up[2]);
    if (em)
        entity_manager_foreach_with(em, COMPONENT_BIT(COMPONENT_AUDIO_SOURCE), audio_sync_source_cb,
                                    NULL);

    // Where the listener is now, and every zone's path to it, eased: what a sound in each is
    // heard with, for an app asking about one it does not place. The first update takes every
    // gain outright, since until it the listener was nowhere -- the world, by default -- and a
    // game starting indoors would hear its rooms fade up.
    audio->listener_zone = zone_find(&audio->zones, listener_pos);
    const float k = audio->updated ? 1.0f - expf(-fmaxf(dt, 0.0f) / AUDIO_ZONE_FADE) : 1.0f;
    for (int z = 0; z < audio->zones.count; z++)
        audio->heard[z] += (audio->zones.path[audio->listener_zone][z] - audio->heard[z]) * k;
    audio->updated = true;
    // A held sound is in the world, and so in a zone, once it is placed: every sound is made 2D
    // and placing one is what spatializes it.
    for (size_t i = 0; i < audio->sound_count; i++) {
        Sound* s = audio->sounds[i];
        if (ma_sound_is_spatialization_enabled(&s->sound))
            zone_ease(audio, &s->sound, k);
    }

    // Voices are reaped here, on the main thread, rather than from miniaudio's end callback,
    // which runs on the mixing thread and may not uninit the sound it is called for. A voice
    // whose entity has gone stays where it last was.
    for (int i = 0; i < AUDIO_VOICE_MAX; i++) {
        AudioVoice* v = &audio->voices[i];
        if (!v->live)
            continue;
        if (ma_sound_at_end(&v->sound)) {
            release_voice(v);
            continue;
        }
        const Entity* e = v->follow && em ? find_entity_by_id(em, v->follow) : NULL;
        if (e)
            place_voice(v, e);
        zone_ease(audio, &v->sound, k);
    }
}

AudioZone audio_zone_add(AudioSystem* audio, const AudioZoneDesc* desc) {
    if (!audio)
        return AUDIO_ZONE_NONE;
    AudioZones* zones = &audio->zones;
    const char* name = desc && desc->name ? desc->name : "unnamed";
    if (!desc || !desc->boxes || desc->box_count <= 0 || desc->box_count > AUDIO_ZONE_BOXES) {
        log_error("audio: zone '%s' wants 1 to %d boxes; not added", name, AUDIO_ZONE_BOXES);
        return AUDIO_ZONE_NONE;
    }
    if (zones->count >= AUDIO_ZONE_MAX) {
        log_error("audio: zone '%s' past the %d there is room for; not added", name,
                  AUDIO_ZONE_MAX - 1);
        return AUDIO_ZONE_NONE;
    }
    const AudioZone z = (AudioZone)zones->count++;
    strncpy(zones->names[z], name, AUDIO_ZONE_NAME - 1);
    memcpy(zones->boxes[z], desc->boxes, (size_t)desc->box_count * sizeof(AABB));
    zones->box_counts[z] = desc->box_count;
    zones_solve(zones);
    return z;
}

AudioZoneLink audio_zone_link(AudioSystem* audio, AudioZone a, AudioZone b, float through) {
    if (!audio)
        return AUDIO_ZONE_NO_LINK;
    AudioZones* zones = &audio->zones;
    if (a >= (AudioZone)zones->count || b >= (AudioZone)zones->count) {
        log_error("audio: a link with an end that is not a zone (%s); not linked",
                  a == AUDIO_ZONE_NONE || b == AUDIO_ZONE_NONE ? "one refused" : "out of range");
        return AUDIO_ZONE_NO_LINK;
    }
    if (a == b) {
        log_error("audio: a link from '%s' to itself; not linked", zones->names[a]);
        return AUDIO_ZONE_NO_LINK;
    }
    if (zones->link_count >= AUDIO_ZONE_LINKS) {
        log_error("audio: a link from '%s' to '%s' past the %d there is room for", zones->names[a],
                  zones->names[b], AUDIO_ZONE_LINKS);
        return AUDIO_ZONE_NO_LINK;
    }
    const AudioZoneLink l = (AudioZoneLink)zones->link_count++;
    zones->link_ends[l][0] = a;
    zones->link_ends[l][1] = b;
    zones->link_through[l] = glm_clamp(through, 0.0f, 1.0f);
    zones_solve(zones);
    return l;
}

void audio_zone_link_set(AudioSystem* audio, AudioZoneLink link, float through) {
    if (!audio || link == AUDIO_ZONE_NO_LINK)
        return;
    AudioZones* zones = &audio->zones;
    if (link >= (AudioZoneLink)zones->link_count) {
        log_error("audio: no link %u to set", link);
        return;
    }
    // Only a change is solved again: a door is set every frame and swings in few of them.
    const float t = glm_clamp(through, 0.0f, 1.0f);
    if (zones->link_through[link] != t) {
        zones->link_through[link] = t;
        zones_solve(zones);
    }
}

float audio_zone_heard(const AudioSystem* audio, AudioZone z) {
    return audio && z < (AudioZone)audio->zones.count ? audio->heard[z] : 1.0f;
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

Sound* entity_add_audio_source_at(struct Entity* entity, Sound* sound, const vec3 offset) {
    if (!entity || !sound || !offset)
        return NULL;
    if (entity_has_component(entity, COMPONENT_AUDIO_SOURCE)) {
        log_error("Entity '%s' already has an audio source", entity->name);
        return NULL;
    }
    AudioSource* src = calloc(1, sizeof(AudioSource));
    if (!src)
        return NULL;
    src->sound = sound;
    glm_vec3_copy((float*)offset, src->offset);
    vec3 p = {0.0f, 0.0f, 0.0f};
    entity_point(entity, offset, p);
    audio_sound_set_position(sound, p);
    entity_add_component(entity, COMPONENT_AUDIO_SOURCE, src);
    entity_set_component_free(entity, COMPONENT_AUDIO_SOURCE, audio_source_free);
    return sound;
}

Sound* entity_add_audio_source(struct Entity* entity, Sound* sound) {
    return entity_add_audio_source_at(entity, sound, (vec3){0.0f, 0.0f, 0.0f});
}

Sound* entity_get_audio_source(struct Entity* entity) {
    if (!entity)
        return NULL;
    AudioSource* src = (AudioSource*)entity_get_component(entity, COMPONENT_AUDIO_SOURCE);
    return src ? src->sound : NULL;
}
