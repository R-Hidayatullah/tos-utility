// psb.h - Fork Particle .psb container parser (Tree of Savior effect system).
//
// The map/skill effect engine (imcEffect::CForkParticleManager) loads
// forkparticle/psb/<name>.psb through the linked Fork Particle SDK. The binary
// was reversed empirically and cross-checked against the client:
//
//   0x00  "PSB\0"
//   0x04  u32 version (100)
//   0x08  u32 payloadSize      (file size - 0x40)
//   0x0C  u32 headerSize       (0x40)
//   0x40  emitter records, back to back, until 0x40+payloadSize
//
// Each emitter is a fixed 0x4B0 block followed by nTex texture entries of 0x120.
// A literal "Custom Data" tag sits at emitter+0x344 exactly once per emitter, so
// emitters enumerate reliably by scanning for it (start = tag - 0x344); the
// texture count then follows from the record size: nTex = (size-0x4B0)/0x120.
//
// This parser decodes the container (emitter names + texture references). The
// full per-emitter simulation parameters (emission, curves, velocity) live in
// the 0x4B0 block and are a later phase.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tosb {

struct PsbEmitter {
    std::string name;
    std::vector<std::string> textures;  // texture basenames (lowercased); may be empty slots
    int declaredTexCount = 0;           // field @0x234 (distinct textures the editor wrote)
    uint32_t offset = 0;                // emitter start offset in the file (for reference)
    uint32_t size = 0;                  // emitter record byte size
    // Decoded look parameters (from the emitter param block):
    float color[4] = {1, 1, 1, 1};      // base tint RGB @+0x120 (candle=warm, fire=orange)
    float particleSize = 1.0f;          // peak particle size @+0x17c..0x18c
};

struct PsbFile {
    bool ok = false;
    uint32_t version = 0;
    std::vector<PsbEmitter> emitters;
};

// Parse a .psb image. Returns ok=false if the magic/layout is not recognized.
PsbFile parsePsb(const uint8_t* data, size_t size);

}  // namespace tosb
