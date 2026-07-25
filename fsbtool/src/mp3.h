/* mp3.h - decode FSB5 MPEG (MP3) sample data to interleaved int16 via minimp3. */
#ifndef FSB_MP3_H
#define FSB_MP3_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Decodes the whole MP3 byte stream. Caller frees *pcm. Returns 0 on success. */
int mp3_decode_all(const uint8_t *data, size_t size,
                   int16_t **pcm, uint64_t *frames, int *channels, int *rate);
#ifdef __cplusplus
}
#endif
#endif
