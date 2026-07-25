#include "bgm.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3_ex.h"

extern "C" {
#include "fsb_player.h"
}

#include <cstdio>
#include <vector>

namespace bgm {

static std::vector<int16_t> g_pcm;  // must outlive playback (fsb_player copies, so ok)

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

bool play_file(const std::string& path, bool loop) {
    std::vector<uint8_t> file;
    if (!read_file(path, file)) return false;

    mp3dec_ex_t dec;
    if (mp3dec_ex_open_buf(&dec, file.data(), file.size(), MP3D_SEEK_TO_SAMPLE) != 0)
        return false;
    int channels = dec.info.channels > 0 ? dec.info.channels : 2;
    int rate = dec.info.hz > 0 ? dec.info.hz : 44100;

    g_pcm.assign((size_t)dec.samples, 0);
    size_t got = mp3dec_ex_read(&dec, g_pcm.data(), dec.samples);
    mp3dec_ex_close(&dec);
    if (got == 0) return false;

    uint64_t frames = got / (uint64_t)channels;
    return fsb_player_start(g_pcm.data(), frames, channels, rate, loop ? 1 : 0) == 0;
}

void stop() { fsb_player_stop(); }
void shutdown() { fsb_player_shutdown(); g_pcm.clear(); }

} // namespace bgm
