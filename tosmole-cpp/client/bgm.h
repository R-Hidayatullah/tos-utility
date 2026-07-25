// bgm.h — decode an MP3 (minimp3) into PCM and stream it via the non-blocking
// fsb_player device. Used for the title/login music.
#pragma once
#include <string>

namespace bgm {
// Decode `path` (disk mp3) fully and start playback. loop repeats. false on fail.
bool play_file(const std::string& path, bool loop);
void stop();
void shutdown();
}
