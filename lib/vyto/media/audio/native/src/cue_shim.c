/* vyto/media/audio/cue native backing — short sounds, fire and forget.

   One mixer per process: a mono f32 48 kHz playback device (miniaudio converts
   to whatever the hardware wants) and eight voices. A clip is PCM in C memory,
   resampled to 48 kHz once, when it is made. play() hands a clip to a free
   voice and returns; the device callback mixes every live voice into its
   buffer and touches nothing but C memory — the rule of ../../README.md.

   VOICE OWNERSHIP is one atomic word per voice, so no lock exists anywhere:

       FREE     the Vyto thread may write the voice's fields, then publish it
       PLAYING  the callback reads the fields and advances pos
       RESTART  the Vyto thread asked for it to start over (PLAYING -> RESTART)
       STOP     the Vyto thread asked for silence (PLAYING/RESTART -> STOP)

   The Vyto thread only ever moves FREE->PLAYING (after filling the fields) and
   PLAYING->RESTART/STOP, by compare-and-swap. The callback only moves
   RESTART->PLAYING, STOP->FREE and, at the end of a clip, PLAYING->FREE —
   also by CAS, so a restart that lands while a clip is finishing is never
   lost: the end-of-clip CAS fails, the callback sees RESTART and rewinds.

   Replaying a clip that is still sounding RESTARTS its voice rather than
   stacking a second copy. Five scan errors in a second are one error sound
   restarted, not five overlapping buzzes.

   A clip is freed on the Vyto thread, and only once no voice is PLAYING it: a
   voice the callback has set FREE is one it will not read again. */

#include <stdint.h>

#ifdef VT_NO_LIBC

int vcue_open(void) { return 0; }
void vcue_close(void) { }
void *vcue_clip(const void *pcm, int64_t n, int32_t rate) { (void)pcm; (void)n; (void)rate; return 0; }
void *vcue_clip_wav(const void *data, int64_t len) { (void)data; (void)len; return 0; }
int64_t vcue_clip_frames(void *c) { (void)c; return 0; }
void vcue_clip_drop(void *c) { (void)c; }
int vcue_play(void *c, int32_t gain_milli) { (void)c; (void)gain_milli; return 0; }
void vcue_stop_all(void) { }
void vcue_set_volume(int32_t milli) { (void)milli; }
int vcue_playing(void) { return 0; }
int64_t vcue_frames(void) { return 0; }

#else

#include "audio_config.h"
#include "miniaudio/miniaudio.h"

#include <stdlib.h>
#include <string.h>

ma_context *vaud_context(void);    /* audio_shim.c */

#define CUE_RATE 48000
#define NVOICE 8
/* Longest clip accepted: these are alerts, not music. */
#define MAX_FRAMES (CUE_RATE * 30)

enum { V_FREE, V_PLAYING, V_RESTART, V_STOP };

typedef struct Clip {
    struct Clip *next;
    float *pcm;
    int64_t n;
    int dropped;             /* Vyto let go; free when no voice plays it */
} Clip;

typedef struct {
    int32_t state;           /* atomic */
    int32_t gain;            /* atomic, per-mille */
    Clip *clip;              /* written while FREE, read while not */
    int64_t pos;             /* callback-only once published */
} Voice;

static ma_device g_dev;
static int g_open;           /* 0 not tried, 1 open, -1 failed */
static Voice g_v[NVOICE];
static int32_t g_volume = 1000;     /* atomic, per-mille */
static int64_t g_frames;            /* atomic: frames mixed with a voice live */
static Clip *g_clips;               /* every clip not yet freed; Vyto thread only */

static int cas(int32_t *p, int32_t from, int32_t to) {
    return __atomic_compare_exchange_n(p, &from, to, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void mix_cb(ma_device *dev, void *out, const void *in, ma_uint32 n) {
    (void)dev;
    (void)in;
    float *o = (float *)out;
    memset(o, 0, sizeof(float) * n);
    int live = 0;
    for (int i = 0; i < NVOICE; i++) {
        Voice *v = &g_v[i];
        int32_t st = __atomic_load_n(&v->state, __ATOMIC_ACQUIRE);
        if (st == V_FREE) continue;
        if (st == V_STOP) { __atomic_store_n(&v->state, V_FREE, __ATOMIC_RELEASE); continue; }
        if (st == V_RESTART) { v->pos = 0; cas(&v->state, V_RESTART, V_PLAYING); }
        float g = (float)__atomic_load_n(&v->gain, __ATOMIC_RELAXED) / 1000.0f;
        const Clip *c = v->clip;
        int64_t left = c->n - v->pos;
        int64_t k = left < (int64_t)n ? left : (int64_t)n;
        const float *src = c->pcm + v->pos;
        for (int64_t j = 0; j < k; j++) o[j] += src[j] * g;
        v->pos += k;
        live = 1;
        if (v->pos >= c->n) {
            /* Fails when a restart or stop arrived meanwhile; the next
               callback honours it. */
            cas(&v->state, V_PLAYING, V_FREE);
        }
    }
    if (!live) return;
    float vol = (float)__atomic_load_n(&g_volume, __ATOMIC_RELAXED) / 1000.0f;
    for (ma_uint32 j = 0; j < n; j++) {
        float x = o[j] * vol;
        o[j] = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;   /* overlapping voices may sum past full scale */
    }
    __atomic_fetch_add(&g_frames, (int64_t)n, __ATOMIC_RELAXED);
}

/* Open and start the mixer's device once. It stays started: an idle callback
   writes a buffer of zeros every period (~10 ms), which costs nothing
   measurable, and a device that is already running answers play() in one
   period instead of the tens of ms a WASAPI start takes. */
int vcue_open(void) {
    if (g_open != 0) return g_open > 0;
    g_open = -1;
    ma_context *ctx = vaud_context();
    if (!ctx) return 0;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 1;
    cfg.sampleRate = CUE_RATE;
    cfg.dataCallback = mix_cb;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    if (ma_device_init(ctx, &cfg, &g_dev) != MA_SUCCESS) return 0;
    if (ma_device_start(&g_dev) != MA_SUCCESS) { ma_device_uninit(&g_dev); return 0; }
    g_open = 1;
    return 1;
}

/* Stop the device (waits for a callback in flight) and forget every voice.
   Clips stay valid; a later play() reopens. */
void vcue_close(void) {
    if (g_open <= 0) { g_open = 0; return; }
    ma_device_uninit(&g_dev);
    for (int i = 0; i < NVOICE; i++) g_v[i].state = V_FREE;
    g_open = 0;
}

/* Free every dropped clip no voice is playing. Vyto thread only. */
static void reap(void) {
    Clip **pp = &g_clips;
    while (*pp) {
        Clip *c = *pp;
        int busy = 0;
        if (c->dropped) {
            for (int i = 0; i < NVOICE && !busy; i++)
                if (__atomic_load_n(&g_v[i].state, __ATOMIC_ACQUIRE) != V_FREE && g_v[i].clip == c) busy = 1;
        }
        if (c->dropped && !busy) {
            *pp = c->next;
            free(c->pcm);
            free(c);
        } else {
            pp = &c->next;
        }
    }
}

/* Linear resampling to CUE_RATE. Alerts are tones and clicks well below the
   Nyquist limit of either rate, which is where linear interpolation is
   indistinguishable from anything fancier. */
static Clip *clip_from(const float *src, int64_t n, int32_t rate) {
    if (!src || n <= 0 || rate < 8000 || rate > 384000) return NULL;
    int64_t out = rate == CUE_RATE ? n : (int64_t)((double)n * CUE_RATE / rate);
    if (out <= 0 || out > MAX_FRAMES) return NULL;
    Clip *c = calloc(1, sizeof *c);
    float *pcm = malloc(sizeof(float) * (size_t)out);
    if (!c || !pcm) { free(c); free(pcm); return NULL; }
    if (rate == CUE_RATE) {
        memcpy(pcm, src, sizeof(float) * (size_t)n);
    } else {
        double step = (double)rate / CUE_RATE;
        for (int64_t i = 0; i < out; i++) {
            double t = (double)i * step;
            int64_t a = (int64_t)t;
            double f = t - (double)a;
            float x0 = src[a < n ? a : n - 1];
            float x1 = src[a + 1 < n ? a + 1 : n - 1];
            pcm[i] = (float)(x0 + (x1 - x0) * f);
        }
    }
    c->pcm = pcm;
    c->n = out;
    c->next = g_clips;
    g_clips = c;
    return c;
}

/* n mono f32 samples at `rate`, copied. */
void *vcue_clip(const void *pcm, int64_t n, int32_t rate) {
    reap();
    return clip_from((const float *)pcm, n, rate);
}

/* ---- WAV: RIFF PCM 8/16/24/32-bit integer or 32-bit float, any channel
   count (averaged to mono). Nothing compressed: that is vyto/media/codec's. */

static uint32_t le16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return le16(p) | (le16(p + 2) << 16); }

void *vcue_clip_wav(const void *data, int64_t len) {
    reap();
    const unsigned char *d = (const unsigned char *)data;
    if (!d || len < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0) return NULL;
    uint32_t fmt = 0, ch = 0, rate = 0, bits = 0;
    const unsigned char *pcm = NULL;
    int64_t pcm_len = 0;
    int64_t off = 12;
    while (off + 8 <= len) {
        uint32_t sz = le32(d + off + 4);
        const unsigned char *body = d + off + 8;
        int64_t avail = len - (off + 8);
        if ((int64_t)sz > avail) sz = (uint32_t)avail;   /* a truncated last chunk: take what is there */
        if (memcmp(d + off, "fmt ", 4) == 0 && sz >= 16) {
            fmt = le16(body);
            ch = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
            if (fmt == 0xFFFE && sz >= 26) fmt = le16(body + 24);   /* WAVE_FORMAT_EXTENSIBLE */
        } else if (memcmp(d + off, "data", 4) == 0) {
            pcm = body;
            pcm_len = sz;
        }
        off += 8 + (int64_t)sz + (sz & 1);   /* chunks are padded to even size */
    }
    if (!pcm || ch == 0 || ch > 32 || rate == 0) return NULL;
    int bps = (int)bits / 8;
    if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32)))
        return NULL;
    int64_t frames = pcm_len / (bps * (int64_t)ch);
    if (frames <= 0 || frames > (int64_t)rate * 30) return NULL;
    float *mono = malloc(sizeof(float) * (size_t)frames);
    if (!mono) return NULL;
    for (int64_t i = 0; i < frames; i++) {
        double sum = 0;
        for (uint32_t c = 0; c < ch; c++) {
            const unsigned char *p = pcm + (i * ch + c) * bps;
            double x;
            if (fmt == 3) {
                uint32_t u = le32(p);
                float f;
                memcpy(&f, &u, 4);
                x = f;
            } else if (bits == 8) {
                x = ((double)p[0] - 128.0) / 128.0;            /* 8-bit WAV is unsigned */
            } else if (bits == 16) {
                x = (double)(int16_t)le16(p) / 32768.0;
            } else if (bits == 24) {
                int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
                x = (double)v / 8388608.0;
            } else {
                x = (double)(int32_t)le32(p) / 2147483648.0;
            }
            sum += x;
        }
        mono[i] = (float)(sum / ch);
    }
    Clip *clip = clip_from(mono, frames, (int32_t)rate);
    free(mono);
    return clip;
}

int64_t vcue_clip_frames(void *h) { return h ? ((Clip *)h)->n : 0; }

void vcue_clip_drop(void *h) {
    if (!h) return;
    ((Clip *)h)->dropped = 1;
    reap();
}

/* 1 when the clip is sounding (started, or restarted because it already
   was), 0 when there is no device or all eight voices are busy. */
int vcue_play(void *h, int32_t gain_milli) {
    Clip *c = (Clip *)h;
    if (!c || c->dropped) return 0;
    reap();
    if (!vcue_open()) return 0;
    if (gain_milli < 0) gain_milli = 0;
    if (gain_milli > 4000) gain_milli = 4000;
    for (int i = 0; i < NVOICE; i++) {
        Voice *v = &g_v[i];
        if (v->clip == c && cas(&v->state, V_PLAYING, V_RESTART)) {
            __atomic_store_n(&v->gain, gain_milli, __ATOMIC_RELAXED);
            return 1;
        }
        if (v->clip == c && __atomic_load_n(&v->state, __ATOMIC_ACQUIRE) == V_RESTART) return 1;
    }
    for (int i = 0; i < NVOICE; i++) {
        Voice *v = &g_v[i];
        if (__atomic_load_n(&v->state, __ATOMIC_ACQUIRE) != V_FREE) continue;
        v->clip = c;
        v->pos = 0;
        __atomic_store_n(&v->gain, gain_milli, __ATOMIC_RELAXED);
        __atomic_store_n(&v->state, V_PLAYING, __ATOMIC_RELEASE);
        return 1;
    }
    return 0;
}

void vcue_stop_all(void) {
    for (int i = 0; i < NVOICE; i++) {
        if (!cas(&g_v[i].state, V_PLAYING, V_STOP)) cas(&g_v[i].state, V_RESTART, V_STOP);
    }
}

void vcue_set_volume(int32_t milli) {
    if (milli < 0) milli = 0;
    if (milli > 1000) milli = 1000;
    __atomic_store_n(&g_volume, milli, __ATOMIC_RELAXED);
}

int vcue_playing(void) {
    int n = 0;
    for (int i = 0; i < NVOICE; i++)
        if (__atomic_load_n(&g_v[i].state, __ATOMIC_ACQUIRE) != V_FREE) n++;
    return n;
}

int64_t vcue_frames(void) { return __atomic_load_n(&g_frames, __ATOMIC_RELAXED); }

#endif
