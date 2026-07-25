// char_model — load a single ToS character part (.xac: bodybase / costume /
// hair) into an m3d::Model with its diffuse textures resolved through the VFS.
#pragma once

#include "app.h"
#include "model3d.h"

#include <string>

struct LoadedPart {
    m3d::Model* model = nullptr;
    float center[3] = {0, 0, 0};
    float radius = 1.0f;
    bool ok = false;
};

// vpath is the normalized model path, e.g.
// "char_hi/pc/warrior_m/warrior_m_bodybase.xac". Returns ok=false on failure.
LoadedPart load_part(App& app, const std::string& vpath);

// Pick a default hairstyle's head + hair model vpaths for a class folder
// ("warrior"/"archer"/"cleric"/"mage") and gender ("m"/"f"). Scans the faces
// folder once and caches. head/hair are empty if none found.
void default_head_hair(App& app, const std::string& classFolder, const std::string& gender,
                       std::string& headVpath, std::string& hairVpath);
