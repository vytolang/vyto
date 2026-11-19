/* vyto/media/audio native backing — miniaudio devices behind C-owned rings.

   First-party. The upstream half is src/miniaudio/miniaudio.h, vendored
   byte-for-byte (see ../refresh-miniaudio.sh) and instantiated in audio_ma.c.

   THE RULE (README, "The constraint that shapes the API"): the device callback
   runs on a thread the OS owns, so it touches C memory only. Each stream is a
   malloc'd vaud_stream holding the ma_device and a single-producer /
   single-consumer ma_pcm_rb. The callback drains (playback) or fills
   (capture) the ring and bumps two counters; the Vyto thread does the other
   end through vaud_write / vaud_read. No Vyto object, no closure, no lock.

   Counters are written by the callback and read by the Vyto thread, so they go
   through __atomic builtins (gcc and clang, which is every compiler vytoc
   drives: cc, mingw-w64, the NDK's clang).

   Backend choice, decided once, lazily, at the first call:
     VYTO_AUDIO unset   every enabled backend except null, in miniaudio's order
                        (WASAPI > DirectSound > WinMM; PulseAudio > ALSA; ...)
     VYTO_AUDIO=null    the null backend only: a device that consumes samples
                        at real-time pace and plays nothing. How tests run.
     VYTO_AUDIO=off     no backend: audio_available() is false, every open
                        returns null. How the no-sound-card path is tested.
   null is never a fallback. A headless box must report "no audio", not
   silently accept a stream that goes nowhere.

   Every entry point has a VT_NO_LIBC arm returning its failure sentinel. */

#ifdef VT_NO_LIBC
/* Freestanding: no threads, no dlopen, no OS audio. Keep every symbol so a
   program that merely imports vyto/media/audio still links, and answer as a
   machine with no sound card would. Deliberately no #include. */

typedef long long vaud_i64;

int vaud_available(void) { return 0; }
const char *vaud_backend(void) { return ""; }
int vaud_dev_refresh(void) { return 0; }
int vaud_dev_kind(int i) { (void)i; return 0; }
const char *vaud_dev_name(int i) { (void)i; return ""; }
const char *vaud_dev_id(int i) { (void)i; return ""; }
int vaud_dev_channels(int i) { (void)i; return 0; }
int vaud_dev_rate(int i) { (void)i; return 0; }
void *vaud_open(int kind, const char *id, int rate, int channels) {
    (void)kind; (void)id; (void)rate; (void)channels; return 0;
}
void vaud_close(void *h) { (void)h; }
int vaud_start(void *h) { (void)h; return 0; }
int vaud_stop(void *h) { (void)h; return 0; }
vaud_i64 vaud_write(void *h, const void *src, vaud_i64 n) { (void)h; (void)src; (void)n; return 0; }
vaud_i64 vaud_read(void *h, void *dst, vaud_i64 max) { (void)h; (void)dst; (void)max; return 0; }
vaud_i64 vaud_writable(void *h) { (void)h; return 0; }
vaud_i64 vaud_readable(void *h) { (void)h; return 0; }
vaud_i64 vaud_xruns(void *h) { (void)h; return 0; }
vaud_i64 vaud_frames(void *h) { (void)h; return 0; }

#else

#include "audio_config.h"
#include "miniaudio/miniaudio.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- the context: one per process, made on first use ----------------- */

static ma_context g_ctx;
static int g_state;          /* 0 not tried, 1 up, -1 failed or turned off */

static int ctx_up(void) {
    if (g_state != 0) return g_state > 0;
    g_state = -1;
    const char *want = getenv("VYTO_AUDIO");
    ma_backend list[ma_backend_null + 1];
    size_t n = 0;
    if (want && strcmp(want, "off") == 0) return 0;
    if (want && strcmp(want, "null") == 0) {
        list[n++] = ma_backend_null;
    } else {
        ma_backend all[ma_backend_null + 1];
        size_t count = 0;
        if (ma_get_enabled_backends(all, ma_backend_null + 1, &count) != MA_SUCCESS) return 0;
        for (size_t i = 0; i < count; i++)
            if (all[i] != ma_backend_null && all[i] != ma_backend_custom) list[n++] = all[i];
        if (n == 0) return 0;
    }
    ma_context_config cfg = ma_context_config_init();
    if (ma_context_init(list, (ma_uint32)n, &cfg, &g_ctx) != MA_SUCCESS) return 0;
    g_state = 1;
    return 1;
}

int vaud_available(void) { return ctx_up(); }

/* For cue_shim.c, the other user of the one context: NULL when there is no
   backend. Not part of the Vyto-facing API. */
ma_context *vaud_context(void) { return ctx_up() ? &g_ctx : NULL; }

/* Which backend won, for diagnostics and the tests ("Null", "PulseAudio"). */
const char *vaud_backend(void) {
    if (!ctx_up()) return "";
    return ma_get_backend_name(g_ctx.backend);
}

/* ---- device ids: ma_device_id as an opaque hex string ---------------
   ma_device_id is a union of backend-specific forms (a wchar_t[64] for WASAPI,
   a char[256] for ALSA, an int for null, ...). The only thing every one of them
   survives is a byte copy, so the id Vyto sees is the union's bytes in hex,
   trailing zero bytes trimmed. It round-trips exactly and promises nothing
   else, which is what "opaque" means in audio.vt. */

static void id_to_hex(const ma_device_id *id, char *out) {
    const unsigned char *b = (const unsigned char *)id;
    size_t n = sizeof *id;
    while (n > 0 && b[n - 1] == 0) n--;
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hx[b[i] >> 4];
        out[2 * i + 1] = hx[b[i] & 15];
    }
    out[2 * n] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_id(const char *s, ma_device_id *id) {
    size_t len = strlen(s);
    if (len % 2 != 0 || len / 2 > sizeof *id) return 0;
    memset(id, 0, sizeof *id);
    unsigned char *b = (unsigned char *)id;
    for (size_t i = 0; i < len / 2; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        b[i] = (unsigned char)(hi * 16 + lo);
    }
    return 1;
}

/* ---- enumeration: a snapshot, read back by index -------------------- */

typedef struct {
    int kind;                /* 0 playback, 1 capture: audio.vt's AUDIO_* */
    char name[MA_MAX_DEVICE_NAME_LENGTH + 1];
    char id[2 * sizeof(ma_device_id) + 1];
    int channels, rate;
} vaud_dev;

static vaud_dev *g_devs;
static int g_ndevs;

static void add_devs(ma_device_info *infos, ma_uint32 count, int kind, int at) {
    for (ma_uint32 i = 0; i < count; i++) {
        vaud_dev *d = &g_devs[at + (int)i];
        d->kind = kind;
        memcpy(d->name, infos[i].name, sizeof d->name);
        d->name[sizeof d->name - 1] = 0;
        id_to_hex(&infos[i].id, d->id);
        d->channels = 0;
        d->rate = 0;
        /* Enumeration does not fill the native formats on most backends; a
           device-info query does. It may fail on a device another program
           holds open, which leaves the zeros: "unknown", not an error. */
        ma_device_info full;
        if (ma_context_get_device_info(&g_ctx, kind == 0 ? ma_device_type_playback : ma_device_type_capture,
                                       &infos[i].id, &full) == MA_SUCCESS && full.nativeDataFormatCount > 0) {
            d->channels = (int)full.nativeDataFormats[0].channels;
            d->rate = (int)full.nativeDataFormats[0].sampleRate;
        }
    }
}

/* Re-enumerate and return how many devices there are, both directions. */
int vaud_dev_refresh(void) {
    free(g_devs);
    g_devs = NULL;
    g_ndevs = 0;
    if (!ctx_up()) return 0;
    ma_device_info *play, *cap;
    ma_uint32 np = 0, nc = 0;
    if (ma_context_get_devices(&g_ctx, &play, &np, &cap, &nc) != MA_SUCCESS) return 0;
    if (np + nc == 0) return 0;
    /* get_device_info below may re-enumerate and invalidate play/cap, so copy
       the two lists before touching it. */
    ma_device_info *copy = malloc(sizeof *copy * (np + nc));
    g_devs = malloc(sizeof *g_devs * (np + nc));
    if (!copy || !g_devs) { free(copy); free(g_devs); g_devs = NULL; return 0; }
    memcpy(copy, play, sizeof *copy * np);
    memcpy(copy + np, cap, sizeof *copy * nc);
    add_devs(copy, np, 0, 0);
    add_devs(copy + np, nc, 1, (int)np);
    free(copy);
    g_ndevs = (int)(np + nc);
    return g_ndevs;
}

static vaud_dev *dev_at(int i) { return (i >= 0 && i < g_ndevs) ? &g_devs[i] : NULL; }
int vaud_dev_kind(int i) { vaud_dev *d = dev_at(i); return d ? d->kind : 0; }
const char *vaud_dev_name(int i) { vaud_dev *d = dev_at(i); return d ? d->name : ""; }
const char *vaud_dev_id(int i) { vaud_dev *d = dev_at(i); return d ? d->id : ""; }
int vaud_dev_channels(int i) { vaud_dev *d = dev_at(i); return d ? d->channels : 0; }
int vaud_dev_rate(int i) { vaud_dev *d = dev_at(i); return d ? d->rate : 0; }

/* ---- streams --------------------------------------------------------- */

/* The ring holds half a second. Capacity caps how far ahead a producer may
   write; it is not latency — latency is what the producer chooses to queue —
   so a larger ring only costs memory: 48 kHz stereo f32 is 192 KB here. */
#define VAUD_RING_MS 500

typedef struct {
    ma_device dev;           /* must not move once initialised: malloc'd */
    ma_pcm_rb rb;
    int kind;
    int channels;
    int64_t frames;          /* atomic: frames moved through the ring */
    int64_t xruns;           /* atomic: underruns (play) / overruns (capture) */
} vaud_stream;

static void play_cb(ma_device *dev, void *out, const void *in, ma_uint32 n) {
    (void)in;
    vaud_stream *s = (vaud_stream *)dev->pUserData;
    size_t fsz = (size_t)s->channels * sizeof(float);
    unsigned char *o = (unsigned char *)out;
    ma_uint32 done = 0;
    while (done < n) {
        ma_uint32 k = n - done;
        void *p;
        if (ma_pcm_rb_acquire_read(&s->rb, &k, &p) != MA_SUCCESS || k == 0) break;
        memcpy(o + (size_t)done * fsz, p, (size_t)k * fsz);
        ma_pcm_rb_commit_read(&s->rb, k);
        done += k;
    }
    if (done < n) {
        memset(o + (size_t)done * fsz, 0, (size_t)(n - done) * fsz);
        __atomic_fetch_add(&s->xruns, 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&s->frames, (int64_t)done, __ATOMIC_RELAXED);
}

static void cap_cb(ma_device *dev, void *out, const void *in, ma_uint32 n) {
    (void)out;
    vaud_stream *s = (vaud_stream *)dev->pUserData;
    size_t fsz = (size_t)s->channels * sizeof(float);
    const unsigned char *src = (const unsigned char *)in;
    ma_uint32 done = 0;
    while (done < n) {
        ma_uint32 k = n - done;
        void *p;
        if (ma_pcm_rb_acquire_write(&s->rb, &k, &p) != MA_SUCCESS || k == 0) break;
        memcpy(p, src + (size_t)done * fsz, (size_t)k * fsz);
        ma_pcm_rb_commit_write(&s->rb, k);
        done += k;
    }
    if (done < n) __atomic_fetch_add(&s->xruns, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->frames, (int64_t)done, __ATOMIC_RELAXED);
}

/* kind 0 playback, 1 capture. id "" = the system default. NULL when no
   device could be opened, which is ordinary on a machine with no sound card. */
void *vaud_open(int kind, const char *id, int rate, int channels) {
    if (!ctx_up()) return NULL;
    if (rate < 8000 || rate > 384000 || channels < 1 || channels > 2) return NULL;
    ma_device_id did;
    int have_id = id && id[0];
    if (have_id && !hex_to_id(id, &did)) return NULL;

    vaud_stream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->kind = kind;
    s->channels = channels;
    ma_uint32 ring = (ma_uint32)((int64_t)rate * VAUD_RING_MS / 1000);
    if (ma_pcm_rb_init(ma_format_f32, (ma_uint32)channels, ring, NULL, NULL, &s->rb) != MA_SUCCESS) {
        free(s);
        return NULL;
    }
    ma_device_config cfg = ma_device_config_init(kind == 0 ? ma_device_type_playback : ma_device_type_capture);
    cfg.sampleRate = (ma_uint32)rate;
    cfg.dataCallback = kind == 0 ? play_cb : cap_cb;
    cfg.pUserData = s;
    if (kind == 0) {
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = (ma_uint32)channels;
        cfg.playback.pDeviceID = have_id ? &did : NULL;
    } else {
        cfg.capture.format = ma_format_f32;
        cfg.capture.channels = (ma_uint32)channels;
        cfg.capture.pDeviceID = have_id ? &did : NULL;
    }
    if (ma_device_init(&g_ctx, &cfg, &s->dev) != MA_SUCCESS) {
        ma_pcm_rb_uninit(&s->rb);
        free(s);
        return NULL;
    }
    return s;
}

/* Stops the device (uninit waits for a callback in flight to return) before
   the ring it reads is freed, so the OS thread can never see freed memory. */
void vaud_close(void *h) {
    vaud_stream *s = (vaud_stream *)h;
    if (!s) return;
    ma_device_uninit(&s->dev);
    ma_pcm_rb_uninit(&s->rb);
    free(s);
}

int vaud_start(void *h) {
    vaud_stream *s = (vaud_stream *)h;
    if (!s) return 0;
    if (ma_device_is_started(&s->dev)) return 1;
    return ma_device_start(&s->dev) == MA_SUCCESS;
}

int vaud_stop(void *h) {
    vaud_stream *s = (vaud_stream *)h;
    if (!s) return 0;
    if (!ma_device_is_started(&s->dev)) return 1;
    return ma_device_stop(&s->dev) == MA_SUCCESS;
}

/* Producer side of a playback ring: n interleaved samples in, how many taken
   out. Whole frames only; a trailing partial frame is never taken. */
int64_t vaud_write(void *h, const void *src, int64_t n) {
    vaud_stream *s = (vaud_stream *)h;
    if (!s || s->kind != 0 || !src || n <= 0) return 0;
    size_t fsz = (size_t)s->channels * sizeof(float);
    int64_t want = n / s->channels;
    int64_t done = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (done < want) {
        ma_uint32 k = (ma_uint32)(want - done > 0x7fffffff ? 0x7fffffff : want - done);
        void *dst;
        if (ma_pcm_rb_acquire_write(&s->rb, &k, &dst) != MA_SUCCESS || k == 0) break;
        memcpy(dst, p + (size_t)done * fsz, (size_t)k * fsz);
        ma_pcm_rb_commit_write(&s->rb, k);
        done += k;
    }
    return done * s->channels;
}

/* Consumer side of a capture ring: up to max samples into dst. */
int64_t vaud_read(void *h, void *dst, int64_t max) {
    vaud_stream *s = (vaud_stream *)h;
    if (!s || s->kind != 1 || !dst || max <= 0) return 0;
    size_t fsz = (size_t)s->channels * sizeof(float);
    int64_t want = max / s->channels;
    int64_t done = 0;
    unsigned char *o = (unsigned char *)dst;
    while (done < want) {
        ma_uint32 k = (ma_uint32)(want - done > 0x7fffffff ? 0x7fffffff : want - done);
        void *p;
        if (ma_pcm_rb_acquire_read(&s->rb, &k, &p) != MA_SUCCESS || k == 0) break;
        memcpy(o + (size_t)done * fsz, p, (size_t)k * fsz);
        ma_pcm_rb_commit_read(&s->rb, k);
        done += k;
    }
    return done * s->channels;
}

int64_t vaud_writable(void *h) {
    vaud_stream *s = (vaud_stream *)h;
    if (!s || s->kind != 0) return 0;
    return (int64_t)ma_pcm_rb_available_write(&s->rb) * s->channels;
}

int64_t vaud_readable(void *h) {
    vaud_stream *s = (vaud_stream *)h;
    if (!s || s->kind != 1) return 0;
    return (int64_t)ma_pcm_rb_available_read(&s->rb) * s->channels;
}

int64_t vaud_xruns(void *h) {
    vaud_stream *s = (vaud_stream *)h;
    return s ? __atomic_load_n(&s->xruns, __ATOMIC_RELAXED) : 0;
}

int64_t vaud_frames(void *h) {
    vaud_stream *s = (vaud_stream *)h;
    return s ? __atomic_load_n(&s->frames, __ATOMIC_RELAXED) : 0;
}

#endif
