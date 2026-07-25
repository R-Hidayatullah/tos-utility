/* fsb_vorbis.h - reconstruct standard Ogg Vorbis from an FSB5 VORBIS sample.
 *
 * FSB5 stores only raw Vorbis audio packets (u16-length framed) plus a crc32
 * that keys the shared codec setup (codebooks). The setup packet is looked up
 * in the built-in table extracted from fmodexD.dll, then the three canonical
 * Vorbis headers (identification/comment/setup) are rebuilt so libvorbis can
 * decode. Block sizes are 256/2048 (from CodecVorbis::addCodecSetup).
 */
#ifndef FSB_VORBIS_H
#define FSB_VORBIS_H

#include "fsb5.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode a VORBIS sample to interleaved signed 16-bit PCM.
 * Caller frees *pcm. Returns 0 on success, negative on error. */
int fsb_vorbis_decode(const fsb5 *f, const fsb_sample *s,
                      int16_t **pcm, uint64_t *frames, int *channels, int *rate);

/* Rebuild a standalone .ogg (Ogg Vorbis) bitstream for a VORBIS sample.
 * Caller frees *ogg. Returns 0 on success, negative on error. */
int fsb_vorbis_rebuild_ogg(const fsb5 *f, const fsb_sample *s,
                           uint8_t **ogg, size_t *ogg_size);

#ifdef __cplusplus
}
#endif
#endif
