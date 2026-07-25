#include "fsb5.h"
#include <stdlib.h>
#include <string.h>

/* Frequency code table - exact values from CodecFSB5::getWaveFormatInternal. */
static const int FREQ_TABLE[11] = {
    4000, 8000, 11000, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000
};
/* Channel code table (2 bits) - from the same function. */
static const int CHAN_TABLE[4] = { 1, 2, 6, 8 };

const char *fsb_format_name(fsb_format f) {
    switch (f) {
        case FSB_FMT_NONE:     return "NONE";
        case FSB_FMT_PCM8:     return "PCM8";
        case FSB_FMT_PCM16:    return "PCM16";
        case FSB_FMT_PCM24:    return "PCM24";
        case FSB_FMT_PCM32:    return "PCM32";
        case FSB_FMT_PCMFLOAT: return "PCMFLOAT";
        case FSB_FMT_GCADPCM:  return "GCADPCM";
        case FSB_FMT_IMAADPCM: return "IMAADPCM";
        case FSB_FMT_VAG:      return "VAG";
        case FSB_FMT_HEVAG:    return "HEVAG";
        case FSB_FMT_XMA:      return "XMA";
        case FSB_FMT_MPEG:     return "MPEG";
        case FSB_FMT_CELT:     return "CELT";
        case FSB_FMT_AT9:      return "AT9";
        case FSB_FMT_XWMA:     return "XWMA";
        case FSB_FMT_VORBIS:   return "VORBIS";
    }
    return "?";
}

/* Parse the metadata chunk chain that follows a sample's 8-byte bitfield.
 * Returns the offset (relative to the sample header start) just past the last
 * chunk, or `start_off` (8) if there are no chunks. */
static size_t parse_metadata(const uint8_t *buf, size_t buf_size,
                             size_t sh_off, fsb_sample *s) {
    size_t off = sh_off + 8;
    int next = 1;
    while (next) {
        if (off + 4 > buf_size) break;
        uint32_t hdr = fsb_rd32(buf + off);
        next            = (int)(hdr & 1);
        uint32_t size   = (hdr >> 1) & 0xFFFFFF;
        uint32_t type   = hdr >> 25;
        const uint8_t *data = buf + off + 4;
        if (off + 4 + size > buf_size) break;

        if (s->nmeta < (int)(sizeof s->metas / sizeof s->metas[0])) {
            s->metas[s->nmeta].type = type;
            s->metas[s->nmeta].size = size;
            s->metas[s->nmeta].data = data;
            s->nmeta++;
        }

        switch (type) {
            case FSB_META_CHANNELS:
                if (size >= 1) s->channels = data[0];
                break;
            case FSB_META_FREQUENCY:
                if (size >= 4) s->frequency = (int)fsb_rd32(data);
                break;
            case FSB_META_LOOP:
                if (size >= 8) {
                    s->loop_start = (int)fsb_rd32(data);
                    s->loop_end   = (int)fsb_rd32(data + 4);
                    s->has_loop   = 1;
                }
                break;
            case FSB_META_VORBISDATA:
                if (size >= 4) s->vorbis_crc32 = fsb_rd32(data);
                /* remainder is the seek table (u32 dataOffset, u32 pcmPos)* */
                break;
            default:
                break;
        }
        off += 4 + size;
    }
    return off;
}

int fsb5_open(const uint8_t *buf, size_t size, fsb5 *out) {
    if (!buf || size < 60) return -1;
    if (memcmp(buf, "FSB5", 4) != 0) return -2;

    memset(out, 0, sizeof *out);
    out->buf                 = buf;
    out->size                = size;
    out->container           = 5;
    out->version             = fsb_rd32(buf + 0x04);
    out->num_samples         = fsb_rd32(buf + 0x08);
    out->sample_headers_size = fsb_rd32(buf + 0x0C);
    out->name_table_size     = fsb_rd32(buf + 0x10);
    out->data_size           = fsb_rd32(buf + 0x14);
    out->format              = (fsb_format)fsb_rd32(buf + 0x18);
    out->mode                = fsb_rd32(buf + 0x20);

    size_t header_size = (out->version == 0) ? 64 : 60;
    if (header_size + (size_t)out->sample_headers_size + (size_t)out->name_table_size > size)
        return -3;

    out->data_start = header_size
                    + (uint64_t)out->sample_headers_size
                    + (uint64_t)out->name_table_size;

    if (out->num_samples == 0) return -4;
    out->samples = (fsb_sample *)calloc(out->num_samples, sizeof(fsb_sample));
    if (!out->samples) return -5;

    const uint8_t *name_table = buf + header_size + out->sample_headers_size;

    size_t sh_off = header_size;              /* absolute offset of sample headers region start */
    size_t region_end = header_size + out->sample_headers_size;

    for (uint32_t i = 0; i < out->num_samples; ++i) {
        fsb_sample *s = &out->samples[i];
        s->index = i;
        s->format = out->format;      /* FSB5: one codec for the whole file */
        s->loop_end = -1;

        if (sh_off + 8 > region_end) return -6;
        uint64_t raw = fsb_rd64(buf + sh_off);

        int has_meta   = (int)(raw & 1);
        int freq_code  = (int)((raw >> 1) & 0xF);
        int chan_code  = (int)((raw >> 5) & 3);
        /* data-offset field is bits 7..33 (27 bits); bits 34..63 are the sample
         * count. A 28-bit mask leaks the low sample bit into the offset. */
        uint64_t doff  = ((raw >> 7) & 0x07FFFFFF) * 32ull;
        uint64_t samp  = (raw >> 34) & 0x3FFFFFFF;

        s->channels    = CHAN_TABLE[chan_code];
        s->frequency   = (freq_code < 11) ? FREQ_TABLE[freq_code] : 0;
        s->samples     = samp;
        s->data_offset = out->data_start + doff;

        size_t after = sh_off + 8;
        if (has_meta)
            after = parse_metadata(buf, region_end, sh_off, s);

        /* name from name table (array of u32 offsets, then C-strings) */
        if (out->name_table_size >= 4 * (uint64_t)out->num_samples) {
            uint32_t noff = fsb_rd32(name_table + 4 * i);
            if (noff < out->name_table_size) {
                const char *nm = (const char *)(name_table + noff);
                size_t maxn = out->name_table_size - noff;
                size_t n = 0;
                while (n < maxn && n < sizeof(s->name) - 1 && nm[n]) { s->name[n] = nm[n]; n++; }
                s->name[n] = 0;
            }
        }

        sh_off = after;
    }

    /* data_size per sample = next sample's data_offset - this one (last uses data chunk end) */
    for (uint32_t i = 0; i < out->num_samples; ++i) {
        uint64_t end = (i + 1 < out->num_samples)
                     ? out->samples[i + 1].data_offset
                     : out->data_start + out->data_size;
        if (end < out->samples[i].data_offset) end = out->samples[i].data_offset;
        if (end > size) end = size;
        out->samples[i].data_size = end - out->samples[i].data_offset;

        if (!out->samples[i].has_loop) {
            out->samples[i].loop_start = 0;
            out->samples[i].loop_end   = (int)out->samples[i].samples - 1;
        }
    }

    return 0;
}

void fsb5_close(fsb5 *f) {
    if (f && f->samples) { free(f->samples); f->samples = NULL; }
}

int fsb_open(const uint8_t *buf, size_t size, fsb5 *out) {
    if (!buf || size < 16) return -1;
    if (memcmp(buf, "FSB5", 4) == 0) return fsb5_open(buf, size, out);
    if (memcmp(buf, "FSB4", 4) == 0 || memcmp(buf, "FSB3", 4) == 0 ||
        memcmp(buf, "FSB2", 4) == 0 || memcmp(buf, "FSB1", 4) == 0)
        return fsb4_open(buf, size, out);
    return -2;
}
