/* fsb_player.h - non-blocking playback of decoded FSB PCM for the browser.
 *
 * Unlike fsbtool's player.c (which blocks until the clip ends), this owns a
 * persistent miniaudio device and returns immediately: the audio streams on
 * miniaudio's own thread while the Win32 message loop keeps running. Starting a
 * new clip or stopping tears the device down first, so only one clip is ever
 * audible at a time.
 */
#ifndef FSB_PLAYER_H
#define FSB_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy `pcm` (interleaved int16) and start playing it. Any current clip is
 * stopped first. loop != 0 repeats until stopped. Returns 0 on success. */
int  fsb_player_start(const int16_t *pcm, uint64_t frames, int channels, int rate, int loop);

/* Stop playback and release the device (no-op if nothing is playing). */
void fsb_player_stop(void);

/* 1 while a non-looping clip is still audible, else 0. */
int  fsb_player_is_playing(void);

/* Release everything (call at shutdown). */
void fsb_player_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
