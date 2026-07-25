/* fsb5.h - portable FMOD FSB5 container parser.
 *
 * Format reverse-engineered from fmodexD.dll (FMOD::CodecFSB5):
 *   - 60-byte header (64 when version==0)
 *   - per-sample 64-bit bitfield header + optional metadata chunks
 *   - name table + interleaved data chunk
 * Little-endian on disk; readers here are endian-independent.
 */
#ifndef FSB5_H
#define FSB5_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FSB_FMT_NONE = 0,
    FSB_FMT_PCM8, FSB_FMT_PCM16, FSB_FMT_PCM24, FSB_FMT_PCM32, FSB_FMT_PCMFLOAT,
    FSB_FMT_GCADPCM, FSB_FMT_IMAADPCM, FSB_FMT_VAG, FSB_FMT_HEVAG,
    FSB_FMT_XMA, FSB_FMT_MPEG, FSB_FMT_CELT, FSB_FMT_AT9, FSB_FMT_XWMA, FSB_FMT_VORBIS
} fsb_format;

/* On-disk metadata chunk types (chunkheader >> 25). */
typedef enum {
    FSB_META_CHANNELS   = 1,
    FSB_META_FREQUENCY  = 2,
    FSB_META_LOOP       = 3,
    FSB_META_XMASEEK    = 6,
    FSB_META_DSPCOEFF   = 7,
    FSB_META_XWMADATA   = 10,
    FSB_META_VORBISDATA = 11
} fsb_meta_type;

typedef struct {
    uint32_t       type;
    uint32_t       size;
    const uint8_t *data;   /* borrowed pointer into the file buffer */
} fsb_meta;

typedef struct {
    uint32_t   index;
    char       name[256];

    fsb_format format;      /* per-sample codec (FSB4 mixes; FSB5 = container format) */
    int        channels;    /* 1/2/6/8, possibly overridden by metadata */
    int        frequency;   /* Hz */
    uint64_t   samples;     /* PCM sample-frames */

    uint64_t data_offset;   /* absolute offset of encoded data in the file */
    uint64_t data_size;     /* encoded byte length */

    int      loop_start;
    int      loop_end;
    int      has_loop;

    /* Vorbis specifics */
    uint32_t       vorbis_crc32;      /* codebook key */
    const uint8_t *vorbis_setup;      /* in-file setup packet, or NULL */
    uint32_t       vorbis_setup_size;

    fsb_meta metas[16];
    int      nmeta;

    /* legacy FSB1-4: extra bytes past the 0x50 base header (e.g. GC DSP coeffs) */
    const uint8_t *extra;
    uint32_t       extra_size;
} fsb_sample;

typedef struct {
    const uint8_t *buf;
    size_t         size;

    int        container;    /* 3, 4 or 5 (FSB version) */
    uint32_t   version;
    uint32_t   num_samples;
    uint32_t   sample_headers_size;
    uint32_t   name_table_size;
    uint32_t   data_size;
    fsb_format format;       /* container format (FSB5); FSB4 = per-sample */
    uint32_t   mode;         /* header flags; bit0 = big-endian sample data */

    uint64_t   data_start;   /* absolute offset of the data chunk */

    fsb_sample *samples;     /* num_samples entries (malloc'd) */
} fsb5;

/* Sniff magic and parse any FSB (3/4/5). `buf` must outlive `out`.
 * Returns 0 on success, negative on error. */
int  fsb_open(const uint8_t *buf, size_t size, fsb5 *out);

/* Version-specific parsers (usually call fsb_open instead). */
int  fsb5_open(const uint8_t *buf, size_t size, fsb5 *out);
int  fsb4_open(const uint8_t *buf, size_t size, fsb5 *out);   /* handles FSB3 + FSB4 */
void fsb5_close(fsb5 *f);

const char *fsb_format_name(fsb_format f);

/* little-endian byte readers (safe on any host endianness) */
static inline uint32_t fsb_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t fsb_rd64(const uint8_t *p) {
    return (uint64_t)fsb_rd32(p) | ((uint64_t)fsb_rd32(p + 4) << 32);
}
static inline uint16_t fsb_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

#ifdef __cplusplus
}
#endif
#endif
