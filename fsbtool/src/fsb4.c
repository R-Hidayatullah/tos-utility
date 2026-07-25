/* fsb4.c - legacy FSB1 / FSB2 / FSB3 / FSB4 container parser.
 *
 * FSB3/FSB4 layout & codec flags are reverse-engineered from fmodexD.dll
 * (FMOD::CodecFSB::getWaveFormatInternal, FMOD_FSB_HEADER / FMOD_FSB_SAMPLE_HEADER)
 * — authoritative. FSB1/FSB2 codecs are NOT present in that DLL (FMOD Ex dropped
 * them), so their layout here follows the public format (vgmstream): best-effort,
 * not IDA-verified. All four share the same FSOUND per-sample codec bitfield.
 *
 * Header (id, numsamples@4, shdrsize@8, datasize@c [, version@10, mode@14]):
 *   FSB1 = 0x10 bytes, fixed 0x40 sample headers, name[0x20]@0, no size field.
 *   FSB2 = 0x18, FSB3 = 0x18, FSB4 = 0x30; sample headers carry a u16 size@0
 *   and name[0x1e]@2. Sample fields sit at the same offsets (0x20+) in every case.
 */
#include "fsb5.h"
#include <stdlib.h>
#include <string.h>

/* FSOUND sample mode flags (from CodecFSB::getWaveFormatInternal). */
#define FS_8BITS     0x00000008u
#define FS_16BITS    0x00000010u
#define FS_MPEG      0x00000200u
#define FS_PCMFLOAT  0x00200000u
#define FS_IMAADPCM  0x00400000u
#define FS_VAG       0x00800000u
#define FS_XMA       0x01000000u
#define FS_GCADPCM   0x02000000u
#define FS_CELT      0x08000000u
#define FS_STEREO    0x00000040u

#define FSB_SRC_BASICHEADERS 0x00000002u

static fsb_format flags_to_format(uint32_t m) {
    if (m & FS_8BITS)    return FSB_FMT_PCM8;
    if (m & FS_16BITS)   return FSB_FMT_PCM16;
    if (m & FS_PCMFLOAT) return FSB_FMT_PCMFLOAT;
    if (m & FS_IMAADPCM) return FSB_FMT_IMAADPCM;
    if (m & FS_VAG)      return FSB_FMT_VAG;
    if (m & FS_GCADPCM)  return FSB_FMT_GCADPCM;
    if (m & FS_XMA)      return FSB_FMT_XMA;
    if (m & FS_MPEG)     return FSB_FMT_MPEG;
    if (m & FS_CELT)     return FSB_FMT_CELT;
    return FSB_FMT_PCM16;
}

static void copy_name(char *dst, const char *src, size_t maxn) {
    size_t n = 0;
    while (n < maxn && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

int fsb4_open(const uint8_t *buf, size_t size, fsb5 *out) {
    if (!buf || size < 16) return -1;

    int container;
    size_t header_size;
    int fixed40;              /* FSB1: fixed 0x40 headers, name@0 len0x20, no size field */
    if      (!memcmp(buf, "FSB4", 4)) { container = 4; header_size = 0x30; fixed40 = 0; }
    else if (!memcmp(buf, "FSB3", 4)) { container = 3; header_size = 0x18; fixed40 = 0; }
    else if (!memcmp(buf, "FSB2", 4)) { container = 2; header_size = 0x18; fixed40 = 0; }
    else if (!memcmp(buf, "FSB1", 4)) { container = 1; header_size = 0x10; fixed40 = 1; }
    else return -2;

    memset(out, 0, sizeof *out);
    out->buf         = buf;
    out->size        = size;
    out->container   = container;
    out->num_samples = fsb_rd32(buf + 0x04);
    out->format      = FSB_FMT_NONE;   /* per-sample */

    if (fixed40) {
        /* FSB1: header 0x10 = id, numsamples@4, datasize@8; fixed 0x40 headers. */
        out->data_size           = fsb_rd32(buf + 0x08);
        out->sample_headers_size = out->num_samples * 0x40;
        out->version             = 1;
        out->mode                = 0;
    } else {
        out->sample_headers_size = fsb_rd32(buf + 0x08);
        out->data_size           = fsb_rd32(buf + 0x0C);
        out->version             = fsb_rd32(buf + 0x10);
        out->mode                = fsb_rd32(buf + 0x14);
    }

    if (header_size + (size_t)out->sample_headers_size > size) return -3;
    out->data_start = header_size + (uint64_t)out->sample_headers_size;

    if (out->num_samples == 0) return -4;
    out->samples = (fsb_sample *)calloc(out->num_samples, sizeof(fsb_sample));
    if (!out->samples) return -5;

    const uint8_t *shdr = buf + header_size;
    const uint8_t *shdr_end = shdr + out->sample_headers_size;
    int basic = !fixed40 && (out->mode & FSB_SRC_BASICHEADERS);

    fsb_format shared_fmt = FSB_FMT_PCM16;
    int shared_ch = 1, shared_freq = 44100;
    char shared_name[256] = {0};

    uint64_t data_cursor = out->data_start;
    const uint8_t *p = shdr;
    const uint8_t *basic_arr = NULL;

    for (uint32_t i = 0; i < out->num_samples; ++i) {
        fsb_sample *s = &out->samples[i];
        s->index = i;
        s->loop_end = -1;

        uint32_t lensamples = 0, lenbytes = 0, loopstart = 0, loopend = 0, smode = 0;
        int chans = 1, freq = 44100;
        const char *name = "";
        int name_len = fixed40 ? 0x20 : 0x1e;
        int name_off = fixed40 ? 0x00 : 0x02;

        if (basic && i > 0) {
            const uint8_t *b = basic_arr + (size_t)(i - 1) * 8;
            if (b + 8 > shdr_end) { out->num_samples = i; break; }
            lensamples = fsb_rd32(b + 0);
            lenbytes   = fsb_rd32(b + 4);
            chans = shared_ch; freq = shared_freq; name = shared_name;
            s->format = shared_fmt;
        } else {
            /* full header (fixed 0x40 for FSB1, else size field @0) */
            size_t need = fixed40 ? 0x40 : 0x50;
            if (p + need > shdr_end) { out->num_samples = i; break; }
            uint32_t hsize = fixed40 ? 0x40 : fsb_rd16(p);
            if (!hsize) hsize = (uint32_t)need;

            name       = (const char *)(p + name_off);
            lensamples = fsb_rd32(p + 0x20);
            lenbytes   = fsb_rd32(p + 0x24);
            if (fixed40) {
                /* FSB1 (FMOD3): compact layout, deffreq@0x28, mode@0x34. */
                freq   = (int)fsb_rd32(p + 0x28);
                smode  = fsb_rd32(p + 0x34);
                chans  = (smode & FS_STEREO) ? 2 : 1;
                /* FMOD3 codec flags are unreliable here; only trust PCM bits. */
                if      (smode & FS_8BITS)  s->format = FSB_FMT_PCM8;
                else if (smode & FS_16BITS) s->format = FSB_FMT_PCM16;
                else                        s->format = FSB_FMT_NONE; /* -> raw dump */
            } else {
                loopstart  = fsb_rd32(p + 0x28);
                loopend    = fsb_rd32(p + 0x2C);
                smode      = fsb_rd32(p + 0x30);
                freq       = (int)fsb_rd32(p + 0x34);
                chans      = fsb_rd16(p + 0x3E);
                s->format  = flags_to_format(smode);
                /* extra header bytes past 0x50 (GC DSP coeffs live here) */
                if (hsize > 0x50) { s->extra = p + 0x50; s->extra_size = hsize - 0x50; }
            }

            if (basic && i == 0) {
                shared_fmt = s->format;
                shared_ch  = chans ? chans : ((smode & FS_STEREO) ? 2 : 1);
                shared_freq = freq;
                copy_name(shared_name, name, name_len);
                basic_arr = p + hsize;
            }
            p += hsize;
        }

        s->channels    = chans ? chans : ((smode & FS_STEREO) ? 2 : 1);
        if (s->channels < 1) s->channels = 1;
        s->frequency   = freq;
        s->samples     = lensamples;
        s->data_offset = data_cursor;
        s->data_size   = lenbytes;
        if (loopend > loopstart) { s->loop_start = (int)loopstart; s->loop_end = (int)loopend; s->has_loop = 1; }
        else                     { s->loop_start = 0; s->loop_end = (int)lensamples - 1; }
        copy_name(s->name, name, name_len);

        data_cursor += lenbytes;
        if (data_cursor > out->data_start + out->data_size)
            data_cursor = out->data_start + out->data_size;
    }

    return 0;
}
