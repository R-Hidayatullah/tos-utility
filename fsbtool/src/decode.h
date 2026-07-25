/* decode.h - decode any supported FSB5 sample to interleaved int16 PCM. */
#ifndef FSB_DECODE_H
#define FSB_DECODE_H

#include "fsb5.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode a sample to interleaved signed 16-bit PCM. Caller frees *pcm.
 * Currently supports: VORBIS, PCM16, PCM8, PCMFLOAT.
 * Returns 0 on success, negative on error. */
int fsb_decode_pcm16(const fsb5 *f, const fsb_sample *s,
                     int16_t **pcm, uint64_t *frames, int *channels, int *rate);

#ifdef __cplusplus
}
#endif
#endif
