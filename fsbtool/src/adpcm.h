/* adpcm.h - ADPCM decoders for FSB5.
 *   IMA    : reproduced from FMOD::IMAAdpcm_Decode* (fmodexD.dll) - authoritative.
 *   GCADPCM: Nintendo DSP ADPCM (coeffs from FSB DSPCOEFF metadata).
 *   VAG    : Sony PS ADPCM (PSX/PS2). HEVAG handled as PS-ADPCM (best effort).
 * All produce interleaved signed 16-bit. Return 0 on success.
 */
#ifndef FSB_ADPCM_H
#define FSB_ADPCM_H

#include "fsb5.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IMA ADPCM. `blockalign` is bytes per block *per channel* (FSB uses 36). */
int adpcm_ima_decode(const uint8_t *src, size_t src_size, int channels,
                     int block_per_ch, int16_t *dst, uint64_t max_frames);

/* Nintendo GC DSP ADPCM. `coefs` = channels*16 int16 (from DSPCOEFF metadata). */
int adpcm_gc_decode(const uint8_t *src, size_t src_size, int channels,
                    const int16_t *coefs, int16_t *dst, uint64_t max_frames);

/* Sony PS-ADPCM (VAG). 16-byte frames, 28 samples each, per channel. */
int adpcm_vag_decode(const uint8_t *src, size_t src_size, int channels,
                     int16_t *dst, uint64_t max_frames);

#ifdef __cplusplus
}
#endif
#endif
