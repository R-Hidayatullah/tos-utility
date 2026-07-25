// app.h — shared context for the tosclient scene shell.
//
// App holds the mounted IPF virtual filesystem + the asset resolver, the
// current character-creation selection, and the scene-switch request. Scene is
// the interface each screen (login, char select, in-game) implements.
#pragma once

#include "tos/ipf/ipf_fs.h"
#include "game_data.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Per-frame input snapshot (mouse already mapped to canvas coordinates).
struct Input {
    float mouseX = 0, mouseY = 0;
    float pressX = 0, pressY = 0; // canvas pos where the current press started
    bool mouseDown = false;      // left button held
    bool mouseClicked = false;   // pressed this frame
    bool mouseReleased = false;  // released this frame
    std::string typed;           // printable WM_CHAR text this frame (utf8)
    bool backspace = false, enter = false, tab = false, esc = false;
    bool keyW = false, keyA = false, keyS = false, keyD = false;
};

enum class SceneId { Login, CharSelect, Loading, InGame, Quit };

struct App;

struct Scene {
    virtual ~Scene() {}
    virtual void enter(App&) {}
    virtual void leave(App&) {}
    // One combined tick: handle input, update, and draw (calls cgfx begin/end).
    virtual void frame(App&, const Input&, float dt) = 0;
};

struct App {
    tos::ipf::IpfFileSystem vfs;
    tosb::GameData gameData;
    std::string gameRoot;

    // Read a vpath's decoded bytes (follows duplicate aliases). false if absent.
    bool readAsset(const std::string& vpath, std::vector<uint8_t>& out);
    // Load an image vpath into a cached cgfx texture. -1 on failure.
    int  loadTexture(const std::string& vpath);
    // Global basename -> winning entry (built lazily); texture-resolution fallback.
    const tos::ipf::IpfEntry* resolveBasename(const std::string& base);

    // Character-creation choice: set by CharSelect, consumed by InGame.
    struct CharSel {
        int classIdx = 0;   // 0=Warrior 1=Archer 2=Cleric 3=Mage
        int gender = 0;      // 0=male 1=female
        std::string name = "Savior";
    } sel;

    // Scene switching: a scene calls go(); the main loop performs the swap.
    void go(SceneId id) { next = id; switching = true; }
    SceneId next = SceneId::Login;
    bool switching = false;

    std::unordered_map<std::string, const tos::ipf::IpfEntry*> basenameIdx;
    bool basenameBuilt = false;
};

// Scene factory (defined in tosclient_main.cpp).
Scene* make_scene(SceneId id);
