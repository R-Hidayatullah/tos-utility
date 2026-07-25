#include "tos/emfx/xsmtime.h"

#include <fstream>
#include <stdexcept>

namespace tos::emfx {

XsmTime parseXsmTime(const uint8_t* data, size_t size) {
    tos::io::ByteReader r(data, size);
    XsmTime t;
    t.marker = r.read_i32();
    uint32_t count = r.read_u32();
    // Each record is a packed 6 bytes: float time + uint16 value (no padding).
    t.keys.reserve(count);
    for (uint32_t i = 0; i < count && r.can_read(6); ++i) {
        XsmTimeKey k;
        k.time = r.read_f32();
        k.value = r.read_u16();
        t.keys.push_back(k);
    }
    t.sizeExact = (static_cast<size_t>(8) + static_cast<size_t>(count) * 6 == size)
                  && t.keys.size() == count;
    return t;
}

XsmTime parseXsmTimeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + path);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseXsmTime(buf.data(), buf.size());
}

} // namespace tos::emfx
