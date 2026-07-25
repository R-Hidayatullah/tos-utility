#include "player.h"
#include "miniaudio.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep(ms); }
#else
#include <time.h>
static void sleep_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
#endif

typedef struct {
    const int16_t *pcm;
    uint64_t       frames;
    int            channels;
    uint64_t       cursor;   /* frames already fed */
    int            done;
} play_ctx;

static void data_cb(ma_device *dev, void *output, const void *input, ma_uint32 frame_count) {
    (void)input;
    play_ctx *c = (play_ctx *)dev->pUserData;
    int16_t *out = (int16_t *)output;

    uint64_t remain = (c->frames > c->cursor) ? (c->frames - c->cursor) : 0;
    ma_uint32 n = (frame_count < remain) ? frame_count : (ma_uint32)remain;

    if (n) {
        memcpy(out, c->pcm + c->cursor * c->channels, (size_t)n * c->channels * sizeof(int16_t));
        c->cursor += n;
    }
    if (n < frame_count) {
        memset(out + (size_t)n * c->channels, 0, (size_t)(frame_count - n) * c->channels * sizeof(int16_t));
        if (c->cursor >= c->frames) c->done = 1;
    }
}

int player_play_s16(const int16_t *pcm, uint64_t frames, int channels, int rate) {
    play_ctx ctx = { pcm, frames, channels, 0, 0 };

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_s16;
    cfg.playback.channels = (ma_uint32)channels;
    cfg.sampleRate        = (ma_uint32)rate;
    cfg.dataCallback      = data_cb;
    cfg.pUserData         = &ctx;

    ma_device device;
    if (ma_device_init(NULL, &cfg, &device) != MA_SUCCESS) {
        fprintf(stderr, "player: failed to open audio device\n");
        return -1;
    }
    if (ma_device_start(&device) != MA_SUCCESS) {
        ma_device_uninit(&device);
        fprintf(stderr, "player: failed to start device\n");
        return -2;
    }

    while (!ctx.done) sleep_ms(20);
    sleep_ms(120); /* let the last buffer drain */

    ma_device_uninit(&device);
    return 0;
}
