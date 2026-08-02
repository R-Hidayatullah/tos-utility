// Byte buffers, little-endian accessors, logging.
//
// Everything on the wire is little-endian and unaligned, so reads and writes
// go through these rather than through struct overlays: the packet layouts
// recovered from the client (packets.h) are packed with no padding, and a
// struct overlay would silently insert some.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace tos {

using Bytes = std::vector<uint8_t>;

// ---- little-endian accessors -------------------------------------------

inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

inline uint64_t rd64(const uint8_t* p) {
    return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32);
}

inline void wr16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

inline void wr64(uint8_t* p, uint64_t v) {
    wr32(p, uint32_t(v));
    wr32(p + 4, uint32_t(v >> 32));
}

inline float rdf(const uint8_t* p) { return std::bit_cast<float>(rd32(p)); }
inline void wrf(uint8_t* p, float f) { wr32(p, std::bit_cast<uint32_t>(f)); }

// ---- files -------------------------------------------------------------

bool read_file(const std::string& path, Bytes& out);
bool write_file(const std::string& path, const uint8_t* p, size_t n);

// ---- strings -----------------------------------------------------------

std::string to_lower(std::string s);
std::string trim(std::string_view s);
// Fixed-width NUL-terminated field -> std::string.
std::string cstr(const uint8_t* p, size_t max);
std::string hex(const uint8_t* p, size_t n, size_t cap = 32);

// ---- logging -----------------------------------------------------------
//
// One writer lock so lines from the accept thread, the connection threads and
// the world tick do not interleave mid-line.

enum class LogLevel { Debug, Info, Warn, Error };

void log_line(LogLevel lv, std::string_view tag, std::string_view msg);
void log_set_min_level(LogLevel lv);

inline void log_debug(std::string_view t, std::string_view m) { log_line(LogLevel::Debug, t, m); }
inline void log_info(std::string_view t, std::string_view m) { log_line(LogLevel::Info, t, m); }
inline void log_warn(std::string_view t, std::string_view m) { log_line(LogLevel::Warn, t, m); }
inline void log_error(std::string_view t, std::string_view m) { log_line(LogLevel::Error, t, m); }

// Wall-clock seconds since the server started, monotonic.
double now_sec();
// Windows FILETIME for the current instant, which several packets carry.
uint64_t now_filetime();
// "YYYY-MM-DD HH:MM:SS" in UTC, offset by `delta_sec`.
std::string utc_string(int delta_sec = 0);

}  // namespace tos
