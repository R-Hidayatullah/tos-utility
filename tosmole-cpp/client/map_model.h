// map_model — bake a ToS .3dworld into a single m3d::Model (all placed .xac
// instances transformed into world space + textured), plus a grid-accelerated
// downward ground-height query for character movement. One map at a time.
#pragma once

#include "app.h"
#include "model3d.h"
#include <string>

struct MapData {
    m3d::Model* model = nullptr;
    float center[3] = {0, 0, 0};
    float radius = 100.f;
    float spawn[3] = {0, 0, 0};  // map centre XZ, ground Y
    bool ok = false;
};

// worldVpath e.g. "bg_hi/hi_entity/f_siauliai_west.3dworld" (or pass a basename
// and it will be resolved). Builds the global ground grid used by map_ground().
MapData load_map(App& app, const std::string& worldVpath);

// Ground height at world (x,z) via the baked triangles: the highest triangle
// whose surface is at or below `ceiling` (use to ignore tree canopy / roofs
// above the walker). false if nothing qualifies.
bool map_ground(float x, float z, float& outY, float ceiling = 1e30f);
