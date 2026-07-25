/* fsbtool - portable FSB5 inspector / exporter / player.
 *
 *   fsbtool info   <file.fsb>
 *   fsbtool list   <file.fsb> [--limit N]
 *   fsbtool export <file.fsb> [-o outdir] [--ogg|--wav] [--index N] [--limit N]
 *   fsbtool play   <file.fsb> [--index N]
 */
#include "fsb5.h"
#include "fsb_vorbis.h"
#include "decode.h"
#include "wav.h"
#include "player.h"
#include "fsbcrypt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0) { fclose(fp); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); fclose(fp); return NULL; }
    fclose(fp);
    *size = (size_t)n;
    return buf;
}

static void sanitize(char *s) {
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || strchr("<>:\"/\\|?*", c)) *s = '_';
    }
}

static void base_name(const char *path, char *out, size_t n) {
    const char *b = path, *p;
    for (p = path; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
    size_t i = 0;
    for (; b[i] && i < n - 1; ++i) out[i] = b[i];
    out[i] = 0;
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static void print_header(const fsb5 *f, const char *path) {
    printf("File   : %s\n", path);
    printf("Container: FSB%d (version 0x%x)\n", f->container, f->version);
    if (f->container == 5)
        printf("Format : %s\n", fsb_format_name(f->format));
    else
        printf("Format : per-sample\n");
    printf("Samples: %u\n", f->num_samples);
    printf("DataAt : 0x%llx  size=0x%x\n",
           (unsigned long long)f->data_start, f->data_size);
}

static int cmd_list(const fsb5 *f, int limit) {
    for (uint32_t i = 0; i < f->num_samples; ++i) {
        if (limit && (int)i >= limit) { printf("  ... (%u more)\n", f->num_samples - limit); break; }
        const fsb_sample *s = &f->samples[i];
        printf("[%4u] %-32s %-8s %dch %6dHz frames=%-8llu size=%-8llu",
               i, s->name[0] ? s->name : "(noname)", fsb_format_name(s->format),
               s->channels, s->frequency,
               (unsigned long long)s->samples, (unsigned long long)s->data_size);
        if (s->format == FSB_FMT_VORBIS) printf(" crc32=0x%08x", s->vorbis_crc32);
        printf("\n");
    }
    return 0;
}

/* write a raw byte blob to outdir/name_idx.ext */
static int write_blob(const char *outdir, const char *name, uint32_t idx,
                      const char *ext, const uint8_t *data, size_t size) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s_%u.%s", outdir, name, idx, ext);
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    fwrite(data, 1, size, fp);
    fclose(fp);
    printf("  -> %s (%zu bytes)\n", path, size);
    return 0;
}

static const char *raw_ext(fsb_format f) {
    switch (f) {
        case FSB_FMT_XMA:  return "xma";
        case FSB_FMT_CELT: return "celt";
        case FSB_FMT_AT9:  return "at9";
        case FSB_FMT_XWMA: return "xwma";
        default:           return "bin";
    }
}

static int export_one(const fsb5 *f, const fsb_sample *s, const char *outdir,
                      int force_wav) {
    char name[300];
    snprintf(name, sizeof name, "%s", s->name[0] ? s->name : "sample");
    sanitize(name);

    char path[1024];
    fsb_format fmt = s->format;

    /* Vorbis -> lossless .ogg repack */
    if (fmt == FSB_FMT_VORBIS && !force_wav) {
        uint8_t *ogg; size_t ogg_size;
        int r = fsb_vorbis_rebuild_ogg(f, s, &ogg, &ogg_size);
        if (r != 0) {
            fprintf(stderr, "  ! [%u] %s: ogg rebuild failed (%d)\n", s->index, name, r);
            return -1;
        }
        snprintf(path, sizeof path, "%s/%s_%u.ogg", outdir, name, s->index);
        FILE *fp = fopen(path, "wb");
        if (!fp) { free(ogg); return -1; }
        fwrite(ogg, 1, ogg_size, fp);
        fclose(fp);
        free(ogg);
        printf("  -> %s (%zu bytes)\n", path, ogg_size);
        return 0;
    }

    /* MPEG -> lossless .mp3 passthrough */
    if (fmt == FSB_FMT_MPEG && !force_wav) {
        return write_blob(outdir, name, s->index, "mp3",
                          f->buf + s->data_offset, (size_t)s->data_size);
    }

    /* Proprietary / unknown codecs with no portable decoder -> dump raw stream */
    if (fmt == FSB_FMT_XMA || fmt == FSB_FMT_CELT ||
        fmt == FSB_FMT_AT9 || fmt == FSB_FMT_XWMA || fmt == FSB_FMT_NONE) {
        fprintf(stderr, "  ~ [%u] %s: %s has no portable decoder; dumping raw stream\n",
                s->index, name, fsb_format_name(fmt));
        return write_blob(outdir, name, s->index, raw_ext(fmt),
                          f->buf + s->data_offset, (size_t)s->data_size);
    }

    int16_t *pcm; uint64_t frames; int ch, rate;
    int r = fsb_decode_pcm16(f, s, &pcm, &frames, &ch, &rate);
    if (r != 0) {
        fprintf(stderr, "  ! [%u] %s: decode failed (%d, format %s)\n",
                s->index, name, r, fsb_format_name(fmt));
        return -1;
    }
    snprintf(path, sizeof path, "%s/%s_%u.wav", outdir, name, s->index);
    wav_write(path, pcm, frames, ch, rate);
    free(pcm);
    printf("  -> %s (%llu frames)\n", path, (unsigned long long)frames);
    return 0;
}

static int cmd_export(const fsb5 *f, const char *infile, const char *outdir_in,
                      int force_wav, int index, int limit) {
    char outdir[1024];
    if (outdir_in) {
        snprintf(outdir, sizeof outdir, "%s", outdir_in);
    } else {
        char base[256]; base_name(infile, base, sizeof base);
        snprintf(outdir, sizeof outdir, "fsb_out/%s", base);
        MKDIR("fsb_out");
    }
    MKDIR(outdir);

    int ok = 0, fail = 0;
    if (index >= 0) {
        if (index >= (int)f->num_samples) { fprintf(stderr, "index out of range\n"); return 1; }
        (export_one(f, &f->samples[index], outdir, force_wav) == 0) ? ok++ : fail++;
    } else {
        for (uint32_t i = 0; i < f->num_samples; ++i) {
            if (limit && (int)i >= limit) break;
            (export_one(f, &f->samples[i], outdir, force_wav) == 0) ? ok++ : fail++;
        }
    }
    printf("done: %d ok, %d failed -> %s\n", ok, fail, outdir);
    return 0;
}

static int cmd_play(const fsb5 *f, int index) {
    if (index < 0) index = 0;
    if (index >= (int)f->num_samples) { fprintf(stderr, "index out of range\n"); return 1; }
    const fsb_sample *s = &f->samples[index];

    int16_t *pcm; uint64_t frames; int ch, rate;
    int r = fsb_decode_pcm16(f, s, &pcm, &frames, &ch, &rate);
    if (r != 0) { fprintf(stderr, "decode failed (%d)\n", r); return 1; }

    printf("Playing [%d] %s  %dch %dHz  %llu frames (%.2fs)\n",
           index, s->name[0] ? s->name : "(noname)", ch, rate,
           (unsigned long long)frames, rate ? (double)frames / rate : 0.0);
    int rc = player_play_s16(pcm, frames, ch, rate);
    free(pcm);
    return rc;
}

static void usage(void) {
    fprintf(stderr,
        "fsbtool - FSB5 inspector / exporter / player\n\n"
        "  fsbtool info   <file.fsb>\n"
        "  fsbtool list   <file.fsb> [--limit N]\n"
        "  fsbtool export <file.fsb> [-o outdir] [--ogg|--wav] [--index N] [--limit N]\n"
        "  fsbtool play   <file.fsb> [--index N]\n"
        "\n"
        "  --key <k>   decrypt an encrypted FSB first (ASCII or 0x-hex key);\n"
        "              auto-detects the XOR scheme via the FSB magic.\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 1; }

    const char *cmd  = argv[1];
    const char *file = argv[2];

    const char *outdir = NULL, *key = NULL;
    int force_wav = 0, index = -1, limit = 0;
    for (int i = 3; i < argc; ++i) {
        if      (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--wav")) force_wav = 1;
        else if (!strcmp(argv[i], "--ogg")) force_wav = 0;
        else if (!strcmp(argv[i], "--index") && i + 1 < argc) index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--key")   && i + 1 < argc) key = argv[++i];
    }

    size_t size;
    uint8_t *buf = read_file(file, &size);
    if (!buf) { fprintf(stderr, "cannot read %s\n", file); return 1; }

    /* Optional decryption. If the file already has a plaintext magic, skip. */
    if (key && !fsb_has_magic(buf, size)) {
        int scheme = fsb_try_decrypt(buf, size, key);
        if (scheme) fprintf(stderr, "decrypted with key (scheme %d)\n", scheme);
        else fprintf(stderr, "warning: key did not yield a valid FSB magic "
                             "(wrong key or unsupported cipher)\n");
    }

    fsb5 f;
    int r = fsb_open(buf, size, &f);
    if (r != 0) { fprintf(stderr, "not a valid FSB file (%d) — encrypted? try --key <key>\n", r); free(buf); return 1; }

    int rc = 0;
    if      (!strcmp(cmd, "info"))   { print_header(&f, file); }
    else if (!strcmp(cmd, "list"))   { print_header(&f, file); printf("\n"); cmd_list(&f, limit); }
    else if (!strcmp(cmd, "export")) { print_header(&f, file); printf("\n"); rc = cmd_export(&f, file, outdir, force_wav, index, limit); }
    else if (!strcmp(cmd, "play"))   { rc = cmd_play(&f, index); }
    else { usage(); rc = 1; }

    fsb5_close(&f);
    free(buf);
    return rc;
}
