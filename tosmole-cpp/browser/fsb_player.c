/* Non-blocking FSB PCM playback (see fsb_player.h). */
#include "fsb_player.h"
#include "miniaudio.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int16_t  *pcm;      /* owned copy of the interleaved samples */
    ma_uint64 frames;
    int       channels;
    ma_uint64 cursor;   /* frames already fed */
    int       loop;
    volatile int done;  /* set by the audio thread when a one-shot finishes */
} play_ctx;

static ma_device g_dev;
static int       g_dev_inited = 0;
static play_ctx  g_ctx;

/* Audio-thread callback: copy the next span, looping or padding with silence. */
static void data_cb(ma_device *dev, void *output, const void *input, ma_uint32 frame_count) {
    (void)input; (void)dev;
    play_ctx *c = &g_ctx;
    int16_t *out = (int16_t *)output;
    ma_uint32 filled = 0;

    while (filled < frame_count) {
        if (c->cursor >= c->frames) {
            if (c->loop && c->frames) c->cursor = 0;
            else { c->done = 1; break; }
        }
        ma_uint64 remain = c->frames - c->cursor;
        ma_uint32 want = frame_count - filled;
        ma_uint32 n = (ma_uint32)(want < remain ? want : remain);
        memcpy(out + (size_t)filled * c->channels,
               c->pcm + c->cursor * c->channels,
               (size_t)n * c->channels * sizeof(int16_t));
        c->cursor += n;
        filled   += n;
    }
    if (filled < frame_count)
        memset(out + (size_t)filled * c->channels, 0,
               (size_t)(frame_count - filled) * c->channels * sizeof(int16_t));
}

void fsb_player_stop(void) {
    /* uninit joins the audio thread, so freeing the buffer afterwards is safe. */
    if (g_dev_inited) { ma_device_uninit(&g_dev); g_dev_inited = 0; }
    if (g_ctx.pcm)   { free(g_ctx.pcm); }
    memset(&g_ctx, 0, sizeof(g_ctx));
}

int fsb_player_start(const int16_t *pcm, uint64_t frames, int channels, int rate, int loop) {
    fsb_player_stop();
    if (!pcm || !frames || channels <= 0 || rate <= 0) return -1;

    size_t bytes = (size_t)frames * (size_t)channels * sizeof(int16_t);
    g_ctx.pcm = (int16_t *)malloc(bytes);
    if (!g_ctx.pcm) return -2;
    memcpy(g_ctx.pcm, pcm, bytes);
    g_ctx.frames   = frames;
    g_ctx.channels = channels;
    g_ctx.cursor   = 0;
    g_ctx.loop     = loop;
    g_ctx.done     = 0;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_s16;
    cfg.playback.channels = (ma_uint32)channels;
    cfg.sampleRate        = (ma_uint32)rate;
    cfg.dataCallback      = data_cb;
    if (ma_device_init(NULL, &cfg, &g_dev) != MA_SUCCESS) { fsb_player_stop(); return -3; }
    g_dev_inited = 1;
    if (ma_device_start(&g_dev) != MA_SUCCESS) { fsb_player_stop(); return -4; }
    return 0;
}

int fsb_player_is_playing(void) {
    return (g_dev_inited && !g_ctx.done) ? 1 : 0;
}

void fsb_player_shutdown(void) { fsb_player_stop(); }
