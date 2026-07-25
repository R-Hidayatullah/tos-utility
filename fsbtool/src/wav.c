#include "wav.h"
#include <stdio.h>

static void w32(FILE *fp, uint32_t v) { unsigned char b[4]={(unsigned char)v,(unsigned char)(v>>8),(unsigned char)(v>>16),(unsigned char)(v>>24)}; fwrite(b,1,4,fp); }
static void w16(FILE *fp, uint16_t v) { unsigned char b[2]={(unsigned char)v,(unsigned char)(v>>8)}; fwrite(b,1,2,fp); }

int wav_write(const char *path, const int16_t *pcm, uint64_t frames, int channels, int rate) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    uint32_t data_bytes = (uint32_t)(frames * channels * 2);
    uint32_t byte_rate  = (uint32_t)(rate * channels * 2);

    fwrite("RIFF", 1, 4, fp);
    w32(fp, 36 + data_bytes);
    fwrite("WAVE", 1, 4, fp);

    fwrite("fmt ", 1, 4, fp);
    w32(fp, 16);
    w16(fp, 1);                       /* PCM */
    w16(fp, (uint16_t)channels);
    w32(fp, (uint32_t)rate);
    w32(fp, byte_rate);
    w16(fp, (uint16_t)(channels * 2)); /* block align */
    w16(fp, 16);                      /* bits per sample */

    fwrite("data", 1, 4, fp);
    w32(fp, data_bytes);
    fwrite(pcm, 1, data_bytes, fp);

    fclose(fp);
    return 0;
}
