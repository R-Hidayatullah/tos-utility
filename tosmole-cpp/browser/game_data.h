// game_data — resolves ToS assets the way the client does, using the two side
// tables shipped with the game data:
//
//   * ies_client/xac.ies  — a "Mesh -> Path" table that tells us which texture
//     directory a given .xac model draws its textures from.
//   * release/<kind>_duplicates.xml — de-duplication tables. When several vpaths
//     hold byte-identical data the game stores the bytes once under a canonical
//     "source" vpath and lists every other "target" vpath that aliases it. A
//     target vpath therefore has no real entry in the archives; to open it we
//     must follow target -> source first.
//
// This module loads both up-front and offers resolveDuplicate()/textureDir()
// lookups on top of the IpfFileSystem.
#pragma once

#include "tos/ipf/ipf_fs.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace tosb {

// One <Model> row of a .3dworld map: a placed instance of a .xac with an
// optional local transform (DirectX/game space, same as the .xac vertices).
struct WorldModel {
    std::string file;   // e.g. "barrack_light.xac"
    std::string model;  // the "Model" attribute (display name)
    bool hasPos = false, hasRot = false, hasScale = false;
    float pos[3] = {0, 0, 0};
    float rot[4] = {0, 0, 0, 1}; // quaternion x,y,z,w
    float scale[3] = {1, 1, 1};
};

// A parsed .3dworld: the model directory (IpfName + Path locate the .xac files)
// and the list of placed models.
struct World3D {
    std::string modelIpf;   // ModelDir @IpfName (e.g. "bg_hi")
    std::string modelPath;  // ModelDir @Path   (e.g. "barrack3")
    // Texture search directories (normalized "ipf/path", no trailing slash) from
    // <TexDir>/<SubTexDir>/<ShaTexDir>. Map model textures live here (e.g.
    // "bg_texture/barrack"), not in ies_client/xac.ies.
    std::vector<std::string> texDirs;
    std::vector<WorldModel> models;
    bool ok = false;
};

class GameData {
public:
    // (Re)load xac.ies + the duplicates tables for a freshly scanned game root.
    // `vfs` must already be populated; `root` is the game folder (holds release/).
    void load(const std::string& root, const tos::ipf::IpfFileSystem& vfs);
    void clear();

    // Follow a duplicates table (chosen by extension) from a target vpath to its
    // stored source vpath. Returns the input unchanged when it is not a known
    // alias. `vpath` is expected normalized (lowercase, forward slashes).
    std::string resolveDuplicate(const std::string& vpath) const;

    // Resolve a vpath to a real archive entry, transparently following a
    // duplicates alias when the direct lookup misses. nullptr if unresolvable.
    const tos::ipf::IpfEntry* resolve(const tos::ipf::IpfFileSystem& vfs,
                                      const std::string& vpath) const;

    // Texture directory recorded in xac.ies for a model vpath (lowercased),
    // forward-slash form without trailing slash. Empty when unknown.
    std::string textureDir(const std::string& xacVpath) const;

    size_t meshMapCount() const { return meshPathMap_.size(); }
    size_t duplicateCount() const;

private:
    // target vpath -> source vpath, one per de-dup kind.
    std::unordered_map<std::string, std::string> dupXac_, dupXsm_, dupXsmtime_,
        dupXpm_, dupDds_;
    // xac model vpath (lc) -> texture directory (lc, forward slashes).
    std::unordered_map<std::string, std::string> meshPathMap_;

    const std::unordered_map<std::string, std::string>* dupFor(const std::string& ext) const;
};

// Parse a .3dworld document from memory (tinyxml2). Returns ok=false on error.
World3D parse3dworld(const uint8_t* data, size_t size);

// --- Map particle effects (.3deffect + forkparticle.xml) -------------------

// One <Effector> of a map's .3deffect: a placed Fork Particle effect (by F_ name)
// at a world position with a per-instance scale.
struct MapEffector {
    std::string effect;             // Fork Particle name, e.g. "F_bg_fire006"
    float pos[3] = {0, 0, 0};
    float scale = 1.0f;
    float viewDist = 0.0f;
};

// One <Particle> row of forkparticle.xml: maps a Fork Particle name to its .psb
// file (basename, no extension) plus the display params the client applies.
struct FpParticleDef {
    std::string file;               // psb basename (no ".psb")
    float scale = 1.0f;
    float color[4] = {1, 1, 1, 1};  // RGBA 0..1 (from "r;g;b;a" 0..255)
    bool additive = false;          // render == "ADD"
};

// Parse a map's .3deffect (<Zone><Effector .../></Zone>). Empty on error.
std::vector<MapEffector> parse3deffect(const uint8_t* data, size_t size);

// Parse a map's .3dprop (<Prop><Model Path="x.xac" .../></Prop>): static
// world-space prop meshes with no per-instance transform. Returns the model
// filenames (in document order). Empty on error.
std::vector<std::string> parse3dprop(const uint8_t* data, size_t size);

// Scene post-processing settings from a map's .3drender. Only the fields the
// viewer applies are kept (linear distance fog); the rest (bloom, vignette, …)
// are parsed-through as ignored.
struct RenderSettings {
    bool ok = false;
    bool fogEnable = false;
    float fogColor[3] = {0, 0, 0};
    float fogStart = 0.0f;
    float fogEnd = 0.0f;
};
RenderSettings parse3drender(const uint8_t* data, size_t size);

// Parse forkparticle.xml into name(lowercased) -> def. Empty on error.
std::unordered_map<std::string, FpParticleDef> parseForkParticle(const uint8_t* data, size_t size);

} // namespace tosb
