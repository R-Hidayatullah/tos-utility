// Shared primitives for the relay: clocks, CRC32, small string helpers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace relay {

// Microseconds since the UNIX epoch. Wall clock -- what goes in the dump.
uint64_t now_us();
// Nanoseconds from a monotonic source. Immune to the clock being stepped, so
// rates and the "was it restored" timeline stay sane across an NTP correction.
uint64_t mono_ns();

// "20260801_143022" -- the timestamp that names dumps and logs.
std::string stamp_compact(uint64_t us);
// "2026-08-01 14:30:22.123" -- the timestamp that prefixes log lines.
std::string stamp_human(uint64_t us);

uint32_t crc32(const uint8_t* p, size_t n);
uint32_t crc32(const std::string& s);

bool read_file(const std::string& path, std::string& out);
bool write_file_atomic(const std::string& path, const std::string& data);
bool file_exists(const std::string& path);
bool make_dirs(const std::string& path);
std::string join_path(const std::string& a, const std::string& b);
std::string base_name(const std::string& path);

// Replaces the value of `attr="..."` wherever it appears. Returns the number
// of replacements. Deliberately not a parser: rewriting one attribute value in
// place keeps every byte of the client's own file -- comments, ordering,
// whitespace -- so the restore is a byte-for-byte comparison rather than a
// re-serialisation that happens to be equivalent.
int set_xml_attr(std::string& xml, const std::string& attr,
                 const std::string& value);

// First value of `attr="..."`, or "" if the attribute is not there.
std::string get_xml_attr(const std::string& xml, const std::string& attr);

std::string human_bytes(uint64_t n);

}  // namespace relay
