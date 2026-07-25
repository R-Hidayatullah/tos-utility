// tex_decode — resolve a ToS texture reference through the VFS (candidate dirs +
// .dds variant + duplicates table + basename fallback) and decode it to a
// GPU-ready subresource (native BCn for DDS, else RGBA via stb_image). Shared by
// the character and map model loaders.
#pragma once

#include "app.h"
#include <cstdint>
#include <string>
#include <vector>

struct DecodedTex {
    bool ok = false;
    uint32_t fmt = 0, w = 0, h = 0, pitch = 0, mip = 1;
    std::vector<uint8_t> bytes;
};

DecodedTex decode_tex(App& app, const std::string& name, const std::vector<std::string>& dirs);
