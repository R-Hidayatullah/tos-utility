#ifndef FSB_WAV_H
#define FSB_WAV_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Write interleaved int16 PCM as a canonical WAV file. Returns 0 on success. */
int wav_write(const char *path, const int16_t *pcm, uint64_t frames, int channels, int rate);
#ifdef __cplusplus
}
#endif
#endif
