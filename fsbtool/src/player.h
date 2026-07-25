#ifndef FSB_PLAYER_H
#define FSB_PLAYER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Blocking playback of interleaved int16 PCM. Returns 0 on success. */
int player_play_s16(const int16_t *pcm, uint64_t frames, int channels, int rate);
#ifdef __cplusplus
}
#endif
#endif
