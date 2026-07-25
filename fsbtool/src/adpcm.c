#include "adpcm.h"
#include <string.h>

/* ---- IMA ADPCM (exact FMOD algorithm) ----------------------------------- */

/* 89-entry IMA step table (verified against fmodexD.dll IMAAdpcm_StepTab). */
static const int IMA_STEP[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,
    88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,
    544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,
    2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
    10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int IMA_INDEX[16] = { -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8 };

/* FMOD::IMAAdpcm_DecodeSample */
static int ima_decode_sample(int nibble, int pred, int step) {
    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 8) diff = -diff;
    int s = pred + diff;
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    return s;
}
static int ima_next_index(int nibble, int idx) {
    idx += IMA_INDEX[nibble & 15];
    if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
    return idx;
}

/* Decode one channel's block (mono path, matches IMAAdpcm_DecodeM16).
 * Writes `frames` samples with destination stride `stride`. */
static void ima_block_mono(const uint8_t *blk, int16_t *dst, int stride, int frames) {
    int pred = (int16_t)(blk[0] | (blk[1] << 8));
    int idx  = blk[2];
    if (idx > 88) idx = 88;
    const uint8_t *p = blk + 4;
    int n = 0;
    *dst = (int16_t)pred; dst += stride; n++;
    while (n < frames) {
        uint8_t b = *p++;
        int lo = b & 0xF, hi = b >> 4;
        pred = ima_decode_sample(lo, pred, IMA_STEP[idx]); idx = ima_next_index(lo, idx);
        *dst = (int16_t)pred; dst += stride; if (++n >= frames) break;
        pred = ima_decode_sample(hi, pred, IMA_STEP[idx]); idx = ima_next_index(hi, idx);
        *dst = (int16_t)pred; dst += stride; ++n;
    }
}

int adpcm_ima_decode(const uint8_t *src, size_t src_size, int channels,
                     int block_per_ch, int16_t *dst, uint64_t max_frames) {
    if (channels < 1 || block_per_ch < 5) return -1;
    const int SPB = 64;                      /* FSB: 64 samples per block */
    size_t block = (size_t)block_per_ch * channels;
    uint64_t done = 0;
    const uint8_t *p = src, *end = src + src_size;

    while (p + block <= end && done < max_frames) {
        int frames = SPB;
        if (done + frames > max_frames) frames = (int)(max_frames - done);
        for (int c = 0; c < channels; ++c)
            ima_block_mono(p + (size_t)c * block_per_ch, dst + done * channels + c, channels, frames);
        done += frames;
        p += block;
    }
    return 0;
}

/* ---- Nintendo GC DSP ADPCM ---------------------------------------------- */
static int clamp16(int v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }

int adpcm_gc_decode(const uint8_t *src, size_t src_size, int channels,
                    const int16_t *coefs, int16_t *dst, uint64_t max_frames) {
    if (channels < 1 || !coefs) return -1;
    /* DSP frame: 1 header byte (scale<<... + coef index) + 7 data = 14 samples.
     * FSB interleaves per-channel by whole-sample streams; treat channels as
     * consecutive independent streams of equal length. */
    size_t per_ch = src_size / channels;
    for (int c = 0; c < channels; ++c) {
        const uint8_t *p = src + (size_t)c * per_ch;
        const uint8_t *end = p + per_ch;
        const int16_t *co = coefs + c * 16;
        int hist1 = 0, hist2 = 0;
        uint64_t frame = 0;
        int16_t *o = dst + c;
        while (p + 8 <= end && frame * 14 < max_frames) {
            uint8_t hdr = *p++;
            int scale = 1 << (hdr & 0xF);
            int ci = (hdr >> 4) & 0xF;
            int c1 = co[ci * 2], c2 = co[ci * 2 + 1];
            for (int i = 0; i < 14; ++i) {
                uint64_t idx = frame * 14 + i;
                if (idx >= max_frames) break;
                int nib = (i & 1) ? (p[i >> 1] & 0xF) : (p[i >> 1] >> 4);
                int s = (nib < 8) ? nib : nib - 16;
                int pred = (s * scale << 11) + 1024 + c1 * hist1 + c2 * hist2;
                pred >>= 11;
                pred = clamp16(pred);
                o[idx * channels] = (int16_t)pred;
                hist2 = hist1; hist1 = pred;
            }
            p += 7;
            frame++;
        }
    }
    return 0;
}

/* ---- Sony PS-ADPCM (VAG) ------------------------------------------------- */
static const int VAG_F[5][2] = {
    {0,0},{60,0},{115,-52},{98,-55},{122,-60}
};

int adpcm_vag_decode(const uint8_t *src, size_t src_size, int channels,
                     int16_t *dst, uint64_t max_frames) {
    if (channels < 1) return -1;
    /* 16-byte frames, 28 samples. Channels interleaved every 16 bytes. */
    size_t frame_stride = (size_t)16 * channels;
    int hist1[8] = {0}, hist2[8] = {0};
    uint64_t done = 0;
    const uint8_t *base = src, *end = src + src_size;

    while (base + frame_stride <= end && done < max_frames) {
        int frames_this = 28;
        if (done + frames_this > max_frames) frames_this = (int)(max_frames - done);
        for (int c = 0; c < channels; ++c) {
            const uint8_t *p = base + (size_t)c * 16;
            int shift = p[0] & 0xF;
            int filter = (p[0] >> 4) & 0xF;
            if (filter > 4) filter = 0;
            int f0 = VAG_F[filter][0], f1 = VAG_F[filter][1];
            int flag = p[1];
            for (int i = 0; i < frames_this; ++i) {
                int byte = p[2 + (i >> 1)];
                int nib = (i & 1) ? (byte >> 4) : (byte & 0xF);
                int s = (nib < 8) ? nib : nib - 16;
                int samp = s << (12 - shift);
                samp = (samp << 6) + f0 * hist1[c] + f1 * hist2[c];
                samp >>= 6;
                samp = clamp16(samp);
                dst[(done + i) * channels + c] = (int16_t)samp;
                hist2[c] = hist1[c]; hist1[c] = samp;
            }
            if (flag == 7) { /* end marker */ }
        }
        done += frames_this;
        base += frame_stride;
    }
    return 0;
}
