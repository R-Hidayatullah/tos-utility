#include "decode.h"
#include "fsb_vorbis.h"
#include "adpcm.h"
#include "mp3.h"
#include <stdlib.h>
#include <string.h>

static int16_t f32_to_s16(float v) {
    int iv = (int)(v * 32767.0f + (v >= 0 ? 0.5f : -0.5f));
    if (iv > 32767) iv = 32767; else if (iv < -32768) iv = -32768;
    return (int16_t)iv;
}

/* locate DSPCOEFF (GC) metadata for a sample */
static const uint8_t *find_meta(const fsb_sample *s, uint32_t type, uint32_t *size) {
    for (int i = 0; i < s->nmeta; ++i)
        if (s->metas[i].type == type) { if (size) *size = s->metas[i].size; return s->metas[i].data; }
    return NULL;
}

int fsb_decode_pcm16(const fsb5 *f, const fsb_sample *s,
                     int16_t **pcm, uint64_t *frames, int *channels, int *rate) {
    const uint8_t *p = f->buf + s->data_offset;
    int ch = s->channels ? s->channels : 1;

    switch (s->format) {   /* per-sample codec (FSB4 mixes formats) */
        case FSB_FMT_VORBIS:
            return fsb_vorbis_decode(f, s, pcm, frames, channels, rate);

        case FSB_FMT_MPEG:
            return mp3_decode_all(p, s->data_size, pcm, frames, channels, rate);

        case FSB_FMT_PCM16: {
            int be = f->mode & 1;                        /* big-endian sample data */
            uint64_t n = s->data_size / 2;
            int16_t *out = (int16_t *)malloc(n * sizeof(int16_t)); if (!out) return -2;
            for (uint64_t i = 0; i < n; ++i) {
                const uint8_t *q = p + 2 * i;
                out[i] = be ? (int16_t)((q[0] << 8) | q[1]) : (int16_t)fsb_rd16(q);
            }
            { uint64_t fr = n / ch;                      /* trim 32-byte-alignment padding */
              if (s->samples && fr > s->samples) fr = s->samples;
              *pcm = out; *frames = fr; *channels = ch; *rate = s->frequency; return 0; }
        }
        case FSB_FMT_PCM8: {
            uint64_t n = s->data_size;
            int16_t *out = (int16_t *)malloc(n * sizeof(int16_t)); if (!out) return -2;
            for (uint64_t i = 0; i < n; ++i) out[i] = (int16_t)(((int)(p[i] ^ 0x80) - 128) << 8);
            { uint64_t fr = n / ch;                      /* trim 32-byte-alignment padding */
              if (s->samples && fr > s->samples) fr = s->samples;
              *pcm = out; *frames = fr; *channels = ch; *rate = s->frequency; return 0; }
        }
        case FSB_FMT_PCM24: {
            uint64_t n = s->data_size / 3;
            int16_t *out = (int16_t *)malloc(n * sizeof(int16_t)); if (!out) return -2;
            for (uint64_t i = 0; i < n; ++i) {
                int32_t v = (int32_t)((uint32_t)p[3*i] | ((uint32_t)p[3*i+1] << 8) | ((uint32_t)p[3*i+2] << 16));
                if (v & 0x800000) v |= ~0xFFFFFF;      /* sign extend */
                out[i] = (int16_t)(v >> 8);
            }
            { uint64_t fr = n / ch;                      /* trim 32-byte-alignment padding */
              if (s->samples && fr > s->samples) fr = s->samples;
              *pcm = out; *frames = fr; *channels = ch; *rate = s->frequency; return 0; }
        }
        case FSB_FMT_PCM32: {
            uint64_t n = s->data_size / 4;
            int16_t *out = (int16_t *)malloc(n * sizeof(int16_t)); if (!out) return -2;
            for (uint64_t i = 0; i < n; ++i) out[i] = (int16_t)((int32_t)fsb_rd32(p + 4 * i) >> 16);
            { uint64_t fr = n / ch;                      /* trim 32-byte-alignment padding */
              if (s->samples && fr > s->samples) fr = s->samples;
              *pcm = out; *frames = fr; *channels = ch; *rate = s->frequency; return 0; }
        }
        case FSB_FMT_PCMFLOAT: {
            int be = f->mode & 1;
            uint64_t n = s->data_size / 4;
            int16_t *out = (int16_t *)malloc(n * sizeof(int16_t)); if (!out) return -2;
            for (uint64_t i = 0; i < n; ++i) {
                const uint8_t *q = p + 4 * i;
                uint32_t u = be ? ((uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3])
                                : fsb_rd32(q);
                float fv; memcpy(&fv, &u, 4);
                out[i] = f32_to_s16(fv);
            }
            { uint64_t fr = n / ch;                      /* trim 32-byte-alignment padding */
              if (s->samples && fr > s->samples) fr = s->samples;
              *pcm = out; *frames = fr; *channels = ch; *rate = s->frequency; return 0; }
        }
        case FSB_FMT_IMAADPCM: {
            if (ch < 1) return -20;   /* FSB stores 36-byte blocks per channel, contiguous */
            uint64_t nf = s->samples;
            int16_t *out = (int16_t *)calloc((size_t)(nf ? nf : 1) * ch, sizeof(int16_t)); if (!out) return -2;
            if (adpcm_ima_decode(p, s->data_size, ch, 36, out, nf) != 0) { free(out); return -21; }
            *pcm = out; *frames = nf; *channels = ch; *rate = s->frequency; return 0;
        }
        case FSB_FMT_GCADPCM: {
            /* DSP coeffs: FSB5 in DSPCOEFF metadata; FSB1-4 in the header extra.
             * Stored big-endian (GameCube), 16 int16 per channel. */
            uint32_t csz = 0;
            const uint8_t *cd = find_meta(s, FSB_META_DSPCOEFF, &csz);
            if (!cd && s->extra) { cd = s->extra; csz = s->extra_size; }
            if (!cd || csz < (uint32_t)(ch * 32)) return -22;
            int16_t *coefs = (int16_t *)malloc(ch * 16 * sizeof(int16_t));
            for (int i = 0; i < ch * 16; ++i)
                coefs[i] = (int16_t)((cd[2*i] << 8) | cd[2*i+1]);   /* big-endian */
            uint64_t nf = s->samples;
            int16_t *out = (int16_t *)calloc((size_t)(nf ? nf : 1) * ch, sizeof(int16_t)); if (!out) { free(coefs); return -2; }
            int r = adpcm_gc_decode(p, s->data_size, ch, coefs, out, nf);
            free(coefs);
            if (r != 0) { free(out); return -23; }
            *pcm = out; *frames = nf; *channels = ch; *rate = s->frequency; return 0;
        }
        case FSB_FMT_VAG:
        case FSB_FMT_HEVAG: {
            uint64_t nf = s->samples;
            int16_t *out = (int16_t *)calloc((size_t)(nf ? nf : 1) * ch, sizeof(int16_t)); if (!out) return -2;
            if (adpcm_vag_decode(p, s->data_size, ch, out, nf) != 0) { free(out); return -24; }
            *pcm = out; *frames = nf; *channels = ch; *rate = s->frequency; return 0;
        }
        default:
            return -1;   /* XMA / CELT / AT9 / XWMA: no portable decoder */
    }
}
