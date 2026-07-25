#include "mp3.h"
#include "minimp3.h"
#include <stdlib.h>

int mp3_decode_all(const uint8_t *data, size_t size,
                   int16_t **pcm_out, uint64_t *frames_out, int *channels, int *rate) {
    mp3dec_t dec;
    mp3dec_init(&dec);

    size_t cap_frames = 65536, nframes = 0;
    int ch = 0, hz = 0;
    int16_t *out = (int16_t *)malloc(cap_frames * 2 * sizeof(int16_t));
    if (!out) return -2;

    const uint8_t *p = data;
    int remain = (int)size;
    int16_t frame_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    while (remain > 0) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec, p, remain, frame_pcm, &info);
        if (info.frame_bytes <= 0) break;      /* no more frames */
        p += info.frame_bytes;
        remain -= info.frame_bytes;
        if (samples <= 0) continue;            /* header/skip frame */

        ch = info.channels; hz = info.hz;
        if (nframes + samples > cap_frames) {
            while (nframes + samples > cap_frames) cap_frames *= 2;
            int16_t *n = (int16_t *)realloc(out, cap_frames * (ch ? ch : 2) * sizeof(int16_t));
            if (!n) { free(out); return -3; }
            out = n;
        }
        for (int i = 0; i < samples * ch; ++i) out[nframes * ch + i] = frame_pcm[i];
        nframes += samples;
    }

    if (ch == 0) { free(out); return -1; }
    *pcm_out = out; *frames_out = nframes; *channels = ch; *rate = hz;
    return 0;
}
