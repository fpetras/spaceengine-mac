// Audio: the game's SDL2_mixer imports, implemented as a small mixer.
//
// The game links a fork of SDL_mixer that can play several music streams at
// once (it cross-fades between soundtrack pieces), which the stock library
// cannot do, so this is written directly: Ogg Vorbis decoding through
// libvorbisfile, mixing in float, output through an SDL3 audio stream.
//
//   chunk    a sound effect, decoded completely when loaded
//   channel  one of the slots a chunk plays in, with volume, loops and fades
//   music    a file decoded while it plays, with volume, loops, fades, seeking
//
// Volumes are 0..128 as in SDL_mixer. Everything shared with the audio thread
// is guarded by one mutex.
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vorbis/vorbisfile.h>

#include "host.h"
#include "paths.h"

enum { OUT_RATE = 44100, MAX_CHANNELS = 64, MAX_MUSIC = 16, MAX_VOLUME = 128 };
enum { NO_FADE, FADE_OUT, FADE_IN };

typedef struct { float *samples; long frames; } chunk_t;   // interleaved stereo at OUT_RATE

typedef struct {
    float gain;               // 0..1, multiplies the volume
    float step;               // per output frame while fading
    int state;
} fade_t;

typedef struct {
    chunk_t *chunk;
    long position;
    int loops;                // further repeats after this one; -1 for ever
    int volume;
    fade_t fade;
} channel_t;

typedef struct music {
    OggVorbis_File file;
    int source_rate, source_channels;
    SDL_AudioStream *convert;  // only when the file's rate is not OUT_RATE
    double duration;
    int playing, loops, volume, in_use;
    fade_t fade;
} music_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioStream *device;
static channel_t channels[MAX_CHANNELS];
static int channel_count = 8;
static music_t *musics[MAX_MUSIC];
static char last_error[256] = "";

static void set_error(const char *text) { snprintf(last_error, sizeof last_error, "%s", text); }
const char *host_audio_error(void) { return last_error; }

// --------------------------------------------------------------- decoding
// Reads up to `frames` stereo frames of the music at its own rate.
static long decode(music_t *m, float *out, long frames) {
    long done = 0;
    while (done < frames) {
        float **pcm;
        int section;
        long n = ov_read_float(&m->file, &pcm, (int)(frames - done), &section);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            out[2 * (done + i)] = pcm[0][i];
            out[2 * (done + i) + 1] = m->source_channels > 1 ? pcm[1][i] : pcm[0][i];
        }
        done += n;
    }
    return done;
}

// Reads `frames` stereo frames at OUT_RATE, looping as the music's loop count
// allows. Returns how many were produced; fewer means the music ended.
static long music_read(music_t *m, float *out, long frames) {
    long done = 0;
    while (done < frames) {
        long n;
        if (m->convert) {
            if (SDL_GetAudioStreamAvailable(m->convert) < (int)((frames - done) * 8)) {
                float block[4096 * 2];
                long got = decode(m, block, 4096);
                if (got > 0) SDL_PutAudioStreamData(m->convert, block, (int)(got * 8));
                else SDL_FlushAudioStream(m->convert);
            }
            int bytes = SDL_GetAudioStreamData(m->convert, out + 2 * done, (int)((frames - done) * 8));
            n = bytes > 0 ? bytes / 8 : 0;
        } else {
            n = decode(m, out + 2 * done, frames - done);
        }
        if (n > 0) { done += n; continue; }
        if (m->loops == 0) break;                 // finished
        if (m->loops > 0) m->loops--;
        ov_time_seek(&m->file, 0.0);
        if (m->convert) SDL_ClearAudioStream(m->convert);
    }
    return done;
}

// ---------------------------------------------------------------- mixing
static float fade_next(fade_t *f, int *stopped) {
    float g = f->gain;
    if (f->state == FADE_IN) { f->gain += f->step; if (f->gain >= 1.0f) { f->gain = 1.0f; f->state = NO_FADE; } }
    else if (f->state == FADE_OUT) { f->gain -= f->step; if (f->gain <= 0.0f) { f->gain = 0.0f; f->state = NO_FADE; *stopped = 1; } }
    return g;
}

static void SDLCALL mix(void *userdata, SDL_AudioStream *stream, int additional, int total) {
    (void)userdata; (void)total;
    static float out[4096 * 2], scratch[4096 * 2];
    while (additional > 0) {
        long frames = additional / 8 < 4096 ? additional / 8 : 4096;
        if (frames <= 0) break;
        memset(out, 0, (size_t)frames * 8);
        pthread_mutex_lock(&lock);
        for (int c = 0; c < channel_count; c++) {
            channel_t *ch = &channels[c];
            for (long i = 0; i < frames && ch->chunk; i++) {
                if (ch->position >= ch->chunk->frames) {
                    if (ch->loops == 0) { ch->chunk = NULL; break; }
                    if (ch->loops > 0) ch->loops--;
                    ch->position = 0;
                    if (ch->chunk->frames == 0) { ch->chunk = NULL; break; }
                }
                int stopped = 0;
                float g = fade_next(&ch->fade, &stopped) * (float)ch->volume / MAX_VOLUME;
                out[2 * i] += ch->chunk->samples[2 * ch->position] * g;
                out[2 * i + 1] += ch->chunk->samples[2 * ch->position + 1] * g;
                ch->position++;
                if (stopped) ch->chunk = NULL;
            }
        }
        for (int k = 0; k < MAX_MUSIC; k++) {
            music_t *m = musics[k];
            if (!m || !m->playing) continue;
            long got = music_read(m, scratch, frames);
            for (long i = 0; i < got; i++) {
                int stopped = 0;
                float g = fade_next(&m->fade, &stopped) * (float)m->volume / MAX_VOLUME;
                out[2 * i] += scratch[2 * i] * g;
                out[2 * i + 1] += scratch[2 * i + 1] * g;
                if (stopped) { m->playing = 0; break; }
            }
            if (got < frames) m->playing = 0;
        }
        pthread_mutex_unlock(&lock);
        for (long i = 0; i < frames * 2; i++) out[i] = out[i] > 1.0f ? 1.0f : out[i] < -1.0f ? -1.0f : out[i];
        SDL_PutAudioStreamData(stream, out, (int)(frames * 8));
        additional -= (int)(frames * 8);
    }
}

static int open_device(void) {
    if (device) return 0;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { set_error(SDL_GetError()); return -1; }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, OUT_RATE};
    device = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, mix, NULL);
    if (!device) { set_error(SDL_GetError()); return -1; }
    for (int c = 0; c < MAX_CHANNELS; c++) { channels[c].volume = MAX_VOLUME; channels[c].fade.gain = 1.0f; }
    SDL_ResumeAudioStreamDevice(device);
    return 0;
}

static void start_fade(fade_t *f, int state, int ms) {
    if (ms <= 0) { f->state = NO_FADE; f->gain = state == FADE_OUT ? 0.0f : 1.0f; return; }
    f->state = state;
    f->step = 1000.0f / ((float)ms * OUT_RATE);
    if (state == FADE_IN) f->gain = 0.0f;
}

// ------------------------------------------------------------- the device
IMPORT(SDL_GetNumAudioDevices) { RET(1); }
IMPORT(SDL_GetAudioDeviceName) { RET("Default output"); }
IMPORT(Mix_OpenAudio) { RET((uint32_t)open_device()); }
IMPORT(Mix_OpenAudioDevice) { RET((uint32_t)open_device()); }
IMPORT(Mix_CloseAudio) {
    (void)c;
    if (device) { SDL_DestroyAudioStream(device); device = NULL; }
}
IMPORT(Mix_AllocateChannels) {
    int n = (int32_t)ARG(0);
    pthread_mutex_lock(&lock);
    if (n >= 0) {
        if (n > MAX_CHANNELS) n = MAX_CHANNELS;
        for (int i = n; i < channel_count; i++) channels[i].chunk = NULL;
        channel_count = n;
    }
    n = channel_count;
    pthread_mutex_unlock(&lock);
    RET((uint32_t)n);
}
IMPORT(Mix_ReserveChannels) { RET((uint32_t)ARG(0)); }

// ----------------------------------------------------------------- chunks
// SDL_RWFromFile is only ever used to hand a file to Mix_LoadWAV_RW, so the
// "RWops" is just the host path.
IMPORT(SDL_RWFromFile) { RET(host_path(PTR(0), 0)); }

static chunk_t *load_chunk(const char *path) {
    OggVorbis_File file;
    if (ov_fopen(path, &file) != 0) { set_error("not an Ogg Vorbis file, or it cannot be opened"); return NULL; }
    vorbis_info *info = ov_info(&file, -1);
    long source_frames = (long)ov_pcm_total(&file, -1), have = 0;
    float *source = malloc((size_t)(source_frames + 1) * 8);
    for (;;) {
        float **pcm;
        int section;
        long n = ov_read_float(&file, &pcm, 4096, &section);
        if (n <= 0 || have + n > source_frames) break;
        for (long i = 0; i < n; i++) {
            source[2 * (have + i)] = pcm[0][i];
            source[2 * (have + i) + 1] = info->channels > 1 ? pcm[1][i] : pcm[0][i];
        }
        have += n;
    }
    chunk_t *chunk = calloc(1, sizeof *chunk);
    if (info->rate == OUT_RATE) {
        chunk->samples = source;
        chunk->frames = have;
    } else {                                   // resample once, at load time
        SDL_AudioSpec from = {SDL_AUDIO_F32, 2, (int)info->rate}, to = {SDL_AUDIO_F32, 2, OUT_RATE};
        Uint8 *converted = NULL;
        int bytes = 0;
        if (SDL_ConvertAudioSamples(&from, (const Uint8 *)source, (int)(have * 8), &to, &converted, &bytes)) {
            chunk->samples = malloc((size_t)bytes + 8);
            memcpy(chunk->samples, converted, (size_t)bytes);
            chunk->frames = bytes / 8;
            SDL_free(converted);
        }
        free(source);
    }
    ov_clear(&file);
    return chunk;
}
IMPORT(Mix_LoadWAV_RW) {
    char *path = PTR(0);
    chunk_t *chunk = path ? load_chunk(path) : NULL;
    if (ARG(1)) free(path);
    RET(chunk);
}
IMPORT(Mix_FreeChunk) {
    chunk_t *chunk = PTR(0);
    if (!chunk) return;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CHANNELS; i++) if (channels[i].chunk == chunk) channels[i].chunk = NULL;
    pthread_mutex_unlock(&lock);
    free(chunk->samples);
    free(chunk);
}

// --------------------------------------------------------------- channels
static int play(int channel, chunk_t *chunk, int loops, int fade_ms) {
    if (!chunk || !device) return -1;
    pthread_mutex_lock(&lock);
    if (channel < 0) for (int i = 0; i < channel_count && channel < 0; i++) if (!channels[i].chunk) channel = i;
    if (channel >= 0 && channel < channel_count) {
        channel_t *ch = &channels[channel];
        ch->chunk = chunk; ch->position = 0; ch->loops = loops;
        start_fade(&ch->fade, FADE_IN, fade_ms);
    } else channel = -1;
    pthread_mutex_unlock(&lock);
    return channel;
}
IMPORT(Mix_PlayChannelTimed) { RET((uint32_t)play((int32_t)ARG(0), PTR(1), (int32_t)ARG(2), 0)); }
IMPORT(Mix_FadeInChannelTimed) { RET((uint32_t)play((int32_t)ARG(0), PTR(1), (int32_t)ARG(2), (int32_t)ARG(3))); }
// Applies `body` to one channel, or to all of them when the number is -1.
#define FOR_CHANNELS(which, body)                                                          \
    for (int i = (which) < 0 ? 0 : (which); i < ((which) < 0 ? channel_count : (which) + 1) && i < MAX_CHANNELS; i++) { \
        channel_t *ch = &channels[i]; body                                                 \
    }
IMPORT(Mix_HaltChannel) {
    int which = (int32_t)ARG(0);
    pthread_mutex_lock(&lock);
    FOR_CHANNELS(which, ch->chunk = NULL;)
    pthread_mutex_unlock(&lock);
    RET(0);
}
IMPORT(Mix_FadeOutChannel) {
    int which = (int32_t)ARG(0), ms = (int32_t)ARG(1), n = 0;
    pthread_mutex_lock(&lock);
    FOR_CHANNELS(which, if (ch->chunk) { start_fade(&ch->fade, FADE_OUT, ms); if (ms <= 0) ch->chunk = NULL; n++; })
    pthread_mutex_unlock(&lock);
    RET((uint32_t)n);
}
IMPORT(Mix_Volume) {
    int which = (int32_t)ARG(0), volume = (int32_t)ARG(1), previous = 0, n = 0;
    pthread_mutex_lock(&lock);
    FOR_CHANNELS(which, previous += ch->volume; n++; if (volume >= 0) ch->volume = volume > MAX_VOLUME ? MAX_VOLUME : volume;)
    pthread_mutex_unlock(&lock);
    RET((uint32_t)(n ? previous / n : 0));
}
IMPORT(Mix_Playing) {
    int which = (int32_t)ARG(0), n = 0;
    pthread_mutex_lock(&lock);
    FOR_CHANNELS(which, n += ch->chunk != NULL;)
    pthread_mutex_unlock(&lock);
    RET((uint32_t)n);
}
IMPORT(Mix_FadingChannel) {
    int which = (int32_t)ARG(0), state = NO_FADE;
    pthread_mutex_lock(&lock);
    if (which >= 0 && which < channel_count && channels[which].chunk) state = channels[which].fade.state;
    pthread_mutex_unlock(&lock);
    RET((uint32_t)state);
}

// ------------------------------------------------------------------ music
IMPORT(Mix_LoadMUS) {
    char *path = host_path(PTR(0), 0);
    music_t *m = calloc(1, sizeof *m);
    int ok = ov_fopen(path, &m->file) == 0;
    free(path);
    if (!ok) { free(m); set_error("not an Ogg Vorbis file, or it cannot be opened"); RET(0); }
    vorbis_info *info = ov_info(&m->file, -1);
    m->source_rate = (int)info->rate;
    m->source_channels = info->channels;
    m->duration = ov_time_total(&m->file, -1);
    m->volume = MAX_VOLUME;
    m->fade.gain = 1.0f;
    if (m->source_rate != OUT_RATE) {
        SDL_AudioSpec from = {SDL_AUDIO_F32, 2, m->source_rate}, to = {SDL_AUDIO_F32, 2, OUT_RATE};
        m->convert = SDL_CreateAudioStream(&from, &to);
    }
    pthread_mutex_lock(&lock);
    for (int k = 0; k < MAX_MUSIC && !m->in_use; k++) if (!musics[k]) { musics[k] = m; m->in_use = 1; }
    pthread_mutex_unlock(&lock);
    if (!m->in_use) { ov_clear(&m->file); free(m); set_error("too many pieces of music open"); RET(0); }
    RET(m);
}
IMPORT(Mix_FreeMusic) {
    music_t *m = PTR(0);
    if (!m) return;
    pthread_mutex_lock(&lock);
    for (int k = 0; k < MAX_MUSIC; k++) if (musics[k] == m) musics[k] = NULL;
    pthread_mutex_unlock(&lock);
    if (m->convert) SDL_DestroyAudioStream(m->convert);
    ov_clear(&m->file);
    free(m);
}
static uint32_t music_start(music_t *m, int loops, int fade_ms, double position) {
    if (!m || !device) return (uint32_t)-1;
    pthread_mutex_lock(&lock);
    ov_time_seek(&m->file, position > 0 ? position : 0.0);
    if (m->convert) SDL_ClearAudioStream(m->convert);
    // SDL_mixer counts plays, the mixer above counts repeats after the first.
    m->loops = loops < 0 ? -1 : loops > 0 ? loops - 1 : 0;
    start_fade(&m->fade, FADE_IN, fade_ms);
    m->playing = 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
IMPORT(Mix_PlayMusicStream) { RET(music_start(PTR(0), (int32_t)ARG(1), 0, 0.0)); }
IMPORT(Mix_FadeInMusicStreamPos) { RET(music_start(PTR(0), (int32_t)ARG(1), (int32_t)ARG(2), c->x[3].f64[0])); }
IMPORT(Mix_HaltMusicStream) {
    music_t *m = PTR(0);
    pthread_mutex_lock(&lock);
    if (m) m->playing = 0;
    pthread_mutex_unlock(&lock);
    RET(0);
}
IMPORT(Mix_FadeOutMusicStream) {
    music_t *m = PTR(0);
    int ms = (int32_t)ARG(1), was = 0;
    pthread_mutex_lock(&lock);
    if (m && m->playing) { was = 1; start_fade(&m->fade, FADE_OUT, ms); if (ms <= 0) m->playing = 0; }
    pthread_mutex_unlock(&lock);
    RET((uint32_t)was);
}
IMPORT(Mix_PlayingMusicStream) { music_t *m = PTR(0); RET(m ? (uint32_t)m->playing : 0); }
IMPORT(Mix_FadingMusicStream) { music_t *m = PTR(0); RET(m && m->playing ? (uint32_t)m->fade.state : NO_FADE); }
IMPORT(Mix_VolumeMusicStream) {
    music_t *m = PTR(0);
    int volume = (int32_t)ARG(1), previous = m ? m->volume : 0;
    if (m && volume >= 0) m->volume = volume > MAX_VOLUME ? MAX_VOLUME : volume;
    RET((uint32_t)previous);
}
IMPORT(Mix_GetVolumeMusicStream) { music_t *m = PTR(0); RET(m ? (uint32_t)m->volume : 0); }
// Durations and positions are doubles in seconds, returned in XMM0.
IMPORT(Mix_MusicDuration) { music_t *m = PTR(0); c->x[0].f64[0] = m ? m->duration : -1.0; }
IMPORT(Mix_GetMusicPosition) {
    music_t *m = PTR(0);
    pthread_mutex_lock(&lock);
    c->x[0].f64[0] = m ? ov_time_tell(&m->file) : -1.0;
    pthread_mutex_unlock(&lock);
}
IMPORT(Mix_SetMusicStreamPosition) {
    music_t *m = PTR(0);
    if (!m) RET((uint32_t)-1);
    pthread_mutex_lock(&lock);
    int r = ov_time_seek(&m->file, c->x[1].f64[0]);
    if (m->convert) SDL_ClearAudioStream(m->convert);
    pthread_mutex_unlock(&lock);
    RET((uint32_t)(r == 0 ? 0 : -1));
}
