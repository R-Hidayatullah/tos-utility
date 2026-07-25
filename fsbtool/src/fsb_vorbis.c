#include "fsb_vorbis.h"
#include "fsb_vorbis_setup_table.h"

#include <ogg/ogg.h>
#include <vorbis/codec.h>

#include <stdlib.h>
#include <string.h>

/* ---- built-in codebook table lookup ------------------------------------- */
const fsb_vorbis_setup *fsb_vorbis_setup_lookup(unsigned int crc32) {
    for (int i = 0; i < fsb_vorbis_setups_count; ++i)
        if (fsb_vorbis_setups[i].crc32 == crc32)
            return &fsb_vorbis_setups[i];
    return NULL;
}

/* ---- canonical Vorbis header builders ----------------------------------- */
static void put_u32le(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v);       p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* Identification header. FSB5 Vorbis uses blocksizes 256 (2^8) and 2048 (2^11). */
static int build_id_header(unsigned char *out, int channels, int rate) {
    out[0] = 0x01;
    memcpy(out + 1, "vorbis", 6);
    put_u32le(out + 7, 0);            /* vorbis version */
    out[11] = (unsigned char)channels;
    put_u32le(out + 12, (uint32_t)rate);
    put_u32le(out + 16, 0);           /* bitrate maximum   */
    put_u32le(out + 20, 0);           /* bitrate nominal   */
    put_u32le(out + 24, 0);           /* bitrate minimum   */
    out[28] = (unsigned char)(8 | (11 << 4)); /* blocksize_0=256, blocksize_1=2048 */
    out[29] = 0x01;                   /* framing bit */
    return 30;
}

static int build_comment_header(unsigned char *out) {
    static const char vendor[] = "fsbtool (FSB5)";
    int n = 0;
    out[n++] = 0x03;
    memcpy(out + n, "vorbis", 6); n += 6;
    put_u32le(out + n, (uint32_t)(sizeof(vendor) - 1)); n += 4;
    memcpy(out + n, vendor, sizeof(vendor) - 1); n += (int)(sizeof(vendor) - 1);
    put_u32le(out + n, 0); n += 4;    /* user comment list length */
    out[n++] = 0x01;                  /* framing bit */
    return n;
}

/* Resolve the setup (codebook) packet for a sample. */
static int get_setup(const fsb_sample *s, const unsigned char **setup, int *setup_len) {
    if (s->vorbis_setup && s->vorbis_setup_size >= 7) {
        *setup = s->vorbis_setup;
        *setup_len = (int)s->vorbis_setup_size;
        return 0;
    }
    const fsb_vorbis_setup *e = fsb_vorbis_setup_lookup(s->vorbis_crc32);
    if (!e) return -1;
    *setup = e->data;
    *setup_len = e->length;
    return 0;
}

/* Initialise a vorbis_info/comment from the three rebuilt headers. */
static int headers_in(vorbis_info *vi, vorbis_comment *vc,
                      unsigned char *idbuf, int idlen,
                      unsigned char *cmbuf, int cmlen,
                      const unsigned char *setup, int setup_len) {
    ogg_packet op;
    memset(&op, 0, sizeof op);

    op.packet = idbuf;   op.bytes = idlen; op.b_o_s = 1; op.packetno = 0;
    if (vorbis_synthesis_headerin(vi, vc, &op) < 0) return -1;

    op.packet = cmbuf;   op.bytes = cmlen; op.b_o_s = 0; op.packetno = 1;
    if (vorbis_synthesis_headerin(vi, vc, &op) < 0) return -2;

    op.packet = (unsigned char *)setup; op.bytes = setup_len; op.packetno = 2;
    if (vorbis_synthesis_headerin(vi, vc, &op) < 0) return -3;

    return 0;
}

/* Walk the u16-length-prefixed audio packets in a sample's data. */
typedef struct { const uint8_t *p; const uint8_t *end; } pkt_iter;
static void pkt_begin(pkt_iter *it, const fsb5 *f, const fsb_sample *s) {
    it->p   = f->buf + s->data_offset;
    it->end = it->p + s->data_size;
}
/* Returns 1 and fills (data,len) for next packet, 0 when done. */
static int pkt_next(pkt_iter *it, const uint8_t **data, int *len) {
    if (it->p + 2 > it->end) return 0;
    uint16_t sz = fsb_rd16(it->p);
    if (sz == 0) return 0;
    if (it->p + 2 + sz > it->end) return 0;
    *data = it->p + 2;
    *len  = sz;
    it->p += 2 + sz;
    return 1;
}

/* ---- decode to interleaved int16 ---------------------------------------- */
int fsb_vorbis_decode(const fsb5 *f, const fsb_sample *s,
                      int16_t **pcm_out, uint64_t *frames_out, int *channels, int *rate) {
    const unsigned char *setup; int setup_len;
    if (get_setup(s, &setup, &setup_len) != 0) return -10;

    unsigned char idbuf[30], cmbuf[64];
    int idlen = build_id_header(idbuf, s->channels, s->frequency);
    int cmlen = build_comment_header(cmbuf);

    vorbis_info vi; vorbis_comment vc;
    vorbis_info_init(&vi);
    vorbis_comment_init(&vc);
    if (headers_in(&vi, &vc, idbuf, idlen, cmbuf, cmlen, setup, setup_len) != 0) {
        vorbis_comment_clear(&vc); vorbis_info_clear(&vi); return -11;
    }

    vorbis_dsp_state vd; vorbis_block vb;
    if (vorbis_synthesis_init(&vd, &vi) != 0) {
        vorbis_comment_clear(&vc); vorbis_info_clear(&vi); return -12;
    }
    vorbis_block_init(&vd, &vb);

    int ch = vi.channels;
    size_t cap_frames = (size_t)(s->samples ? s->samples + 8192 : 65536);
    int16_t *out = (int16_t *)malloc(cap_frames * ch * sizeof(int16_t));
    if (!out) { vorbis_block_clear(&vb); vorbis_dsp_clear(&vd);
                vorbis_comment_clear(&vc); vorbis_info_clear(&vi); return -13; }
    size_t nframes = 0;

    pkt_iter it; pkt_begin(&it, f, s);
    const uint8_t *pd; int pl; long packetno = 3;
    ogg_packet op; memset(&op, 0, sizeof op);

    while (pkt_next(&it, &pd, &pl)) {
        op.packet = (unsigned char *)pd;
        op.bytes  = pl;
        op.b_o_s  = 0;
        op.e_o_s  = (it.p + 2 > it.end) ? 1 : 0;
        op.granulepos = -1;
        op.packetno = packetno++;

        if (vorbis_synthesis(&vb, &op) == 0)
            vorbis_synthesis_blockin(&vd, &vb);

        float **pcm;
        int got;
        while ((got = vorbis_synthesis_pcmout(&vd, &pcm)) > 0) {
            if (nframes + got > cap_frames) {
                size_t ncap = (nframes + got) * 2;
                int16_t *n = (int16_t *)realloc(out, ncap * ch * sizeof(int16_t));
                if (!n) { got = 0; break; }
                out = n; cap_frames = ncap;
            }
            for (int i = 0; i < got; ++i) {
                for (int c = 0; c < ch; ++c) {
                    float v = pcm[c][i];
                    int iv = (int)(v * 32767.0f + (v >= 0 ? 0.5f : -0.5f));
                    if (iv > 32767) iv = 32767; else if (iv < -32768) iv = -32768;
                    out[(nframes + i) * ch + c] = (int16_t)iv;
                }
            }
            nframes += got;
            vorbis_synthesis_read(&vd, got);
        }
    }

    /* Trim decoder-tail overshoot to the declared sample count. */
    if (s->samples && nframes > s->samples) nframes = (size_t)s->samples;

    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);

    *pcm_out    = out;
    *frames_out = nframes;
    *channels   = ch;
    *rate       = s->frequency;
    return 0;
}

/* ---- rebuild a standalone .ogg ------------------------------------------ */
int fsb_vorbis_rebuild_ogg(const fsb5 *f, const fsb_sample *s,
                           uint8_t **ogg_out, size_t *ogg_size) {
    const unsigned char *setup; int setup_len;
    if (get_setup(s, &setup, &setup_len) != 0) return -10;

    unsigned char idbuf[30], cmbuf[64];
    int idlen = build_id_header(idbuf, s->channels, s->frequency);
    int cmlen = build_comment_header(cmbuf);

    /* need vorbis_info to compute per-packet block sizes for granulepos */
    vorbis_info vi; vorbis_comment vc;
    vorbis_info_init(&vi); vorbis_comment_init(&vc);
    if (headers_in(&vi, &vc, idbuf, idlen, cmbuf, cmlen, setup, setup_len) != 0) {
        vorbis_comment_clear(&vc); vorbis_info_clear(&vi); return -11;
    }

    ogg_stream_state os;
    ogg_stream_init(&os, (int)(s->vorbis_crc32 ? s->vorbis_crc32 : 1) ^ (int)s->index);

    /* growable output buffer */
    size_t cap = 65536, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) { ogg_stream_clear(&os); vorbis_comment_clear(&vc); vorbis_info_clear(&vi); return -13; }
    #define OGG_APPEND(ptr, n) do { \
        if (len + (n) > cap) { while (len + (n) > cap) cap *= 2; \
            uint8_t *nb = (uint8_t *)realloc(buf, cap); if (!nb) goto oom; buf = nb; } \
        memcpy(buf + len, (ptr), (n)); len += (n); } while (0)

    ogg_page pg;
    ogg_packet op; memset(&op, 0, sizeof op);

    /* header packets */
    op.packet = idbuf; op.bytes = idlen; op.b_o_s = 1; op.granulepos = 0; op.packetno = 0;
    ogg_stream_packetin(&os, &op);
    while (ogg_stream_flush(&os, &pg)) { OGG_APPEND(pg.header, pg.header_len); OGG_APPEND(pg.body, pg.body_len); }

    op.packet = cmbuf; op.bytes = cmlen; op.b_o_s = 0; op.granulepos = 0; op.packetno = 1;
    ogg_stream_packetin(&os, &op);
    op.packet = (unsigned char *)setup; op.bytes = setup_len; op.granulepos = 0; op.packetno = 2;
    ogg_stream_packetin(&os, &op);
    while (ogg_stream_flush(&os, &pg)) { OGG_APPEND(pg.header, pg.header_len); OGG_APPEND(pg.body, pg.body_len); }

    /* audio packets with computed granule positions */
    pkt_iter it; pkt_begin(&it, f, s);
    const uint8_t *pd; int pl; long packetno = 3;
    ogg_int64_t granule = 0, last_bs = 0;

    while (pkt_next(&it, &pd, &pl)) {
        op.packet = (unsigned char *)pd; op.bytes = pl;
        op.b_o_s = 0;
        op.e_o_s = (it.p + 2 > it.end) ? 1 : 0;
        op.packetno = packetno++;

        long bs = vorbis_packet_blocksize(&vi, &op);
        if (bs > 0) {
            if (last_bs) granule += (last_bs + bs) / 4;
            last_bs = bs;
        }
        op.granulepos = granule;

        ogg_stream_packetin(&os, &op);
        while (ogg_stream_pageout(&os, &pg)) { OGG_APPEND(pg.header, pg.header_len); OGG_APPEND(pg.body, pg.body_len); }
    }
    while (ogg_stream_flush(&os, &pg)) { OGG_APPEND(pg.header, pg.header_len); OGG_APPEND(pg.body, pg.body_len); }

    ogg_stream_clear(&os);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    *ogg_out = buf;
    *ogg_size = len;
    return 0;

oom:
    free(buf);
    ogg_stream_clear(&os);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return -14;
    #undef OGG_APPEND
}
