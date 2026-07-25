// scene_ingame — the first map. Bakes West Siauliai Woods (.3dworld) into one
// mesh, spawns the created character on the ground, and gives a third-person
// follow camera with WASD movement (ground-snapped) and left-drag to orbit.
#include "app.h"
#include "client_gfx.h"
#include "model3d.h"
#include "map_model.h"
#include "char_model.h"
#include "tinymath.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* kClassFolder[4] = {"warrior", "archer", "cleric", "mage"};
const char* kGender[2] = {"m", "f"};

void mat16(const tmath::Mat4& m, float* out) { std::memcpy(out, &m.m[0][0], 16 * sizeof(float)); }

class InGameScene : public Scene {
    MapData m_map;
    std::vector<LoadedPart> m_char;
    float m_charRadius = 2.f;

    float m_pos[3] = {0, 0, 0};
    float m_facing = 0;       // character yaw
    float m_camYaw = 2.2f;    // camera orbit yaw
    float m_camPitch = 0.66f; // ~38 deg
    float m_lastMouseX = 0;
    bool m_dragging = false;

    void unloadChar() { for (auto& p : m_char) if (p.model) m3d::destroy(p.model); m_char.clear(); }

public:
    void enter(App& app) override {
        // Map (authentic newbie field). Basename lets load_map resolve the archive.
        m_map = load_map(app, "f_siauliai_west.3dworld");

        // Character = created selection (body + costume).
        std::string dir = std::string("char_hi/pc/") + kClassFolder[app.sel.classIdx] + "_" + kGender[app.sel.gender];
        std::string prefix = std::string(kClassFolder[app.sel.classIdx]) + "_" + kGender[app.sel.gender];
        for (const char* suf : {"_bodybase.xac", "_costume01.xac"}) {
            LoadedPart p = load_part(app, dir + "/" + prefix + suf);
            if (p.ok) { if (m_char.empty()) m_charRadius = p.radius > 0.01f ? p.radius : 2.f; m_char.push_back(p); }
        }

        if (m_map.ok) { m_pos[0] = m_map.spawn[0]; m_pos[1] = m_map.spawn[1]; m_pos[2] = m_map.spawn[2]; }
    }
    void leave(App& app) override {
        unloadChar();
        if (m_map.model) { m3d::destroy(m_map.model); m_map.model = nullptr; }
    }

    void frame(App& app, const Input& in, float dt) override {
        if (in.esc) { app.go(SceneId::CharSelect); return; }

        // --- camera orbit via left-drag ---
        if (in.mouseDown) {
            if (!m_dragging) { m_dragging = true; m_lastMouseX = in.mouseX; }
            m_camYaw += (in.mouseX - m_lastMouseX) * 0.005f;
            m_lastMouseX = in.mouseX;
        } else m_dragging = false;

        // --- movement (WASD relative to camera yaw) ---
        float mf[3] = {std::cos(m_camYaw), 0, std::sin(m_camYaw)};       // forward on XZ
        float mr[3] = {std::sin(m_camYaw), 0, -std::cos(m_camYaw)};      // right on XZ
        float dx = 0, dz = 0;
        if (in.keyW) { dx += mf[0]; dz += mf[2]; }
        if (in.keyS) { dx -= mf[0]; dz -= mf[2]; }
        if (in.keyD) { dx += mr[0]; dz += mr[2]; }
        if (in.keyA) { dx -= mr[0]; dz -= mr[2]; }
        float len = std::sqrt(dx * dx + dz * dz);
        if (len > 1e-4f) {
            dx /= len; dz /= len;
            float speed = m_charRadius * 3.5f;   // world units / sec
            m_pos[0] += dx * speed * dt;
            m_pos[2] += dz * speed * dt;
            m_facing = std::atan2(dz, dx);
        }
        // ground-snap: prefer the surface at/below the character (ignore canopy
        // overhead); fall back to the highest surface if nothing is below.
        float gy;
        if (map_ground(m_pos[0], m_pos[2], gy, m_pos[1] + m_charRadius * 2.f)) m_pos[1] = gy;
        else if (map_ground(m_pos[0], m_pos[2], gy)) m_pos[1] = gy;

        // --- camera ---
        float head = m_charRadius * 1.2f;
        tmath::Vec3 tgt{m_pos[0], m_pos[1] + head, m_pos[2]};
        float dist = m_charRadius * 7.f;
        float cp = std::cos(m_camPitch), sp = std::sin(m_camPitch);
        float cy = std::cos(m_camYaw), sy = std::sin(m_camYaw);
        tmath::Vec3 eye{tgt.x - dist * cp * cy, tgt.y + dist * sp, tgt.z - dist * cp * sy};
        tmath::Mat4 view = tmath::lookAtLH(eye, tgt, {0, 1, 0});
        float farP = m_map.ok ? m_map.radius * 2.5f : 5000.f;
        float nearP = std::max(dist * 0.05f, farP * 1e-4f);
        tmath::Mat4 proj = tmath::perspectiveFovLH(45.f * 3.14159265f / 180.f,
                                                   cgfx::viewport_aspect(), nearP, farP);
        float vp[16]; mat16(tmath::mul(view, proj), vp);
        float camPos[3] = {eye.x, eye.y, eye.z};
        float light[3] = {-0.4f, -0.75f, -0.5f};

        cgfx::begin_frame(0.45f, 0.62f, 0.80f);   // sky
        m3d::set_camera(vp, camPos, light);

        float ident[16]; mat16(tmath::identity(), ident);
        if (m_map.model) m3d::draw(m_map.model, ident);

        // character world matrix: face movement, stand at m_pos.
        float world[16];
        mat16(tmath::mul(tmath::rotationQuat(0, std::sin(-m_facing * 0.5f), 0, std::cos(-m_facing * 0.5f)),
                         tmath::translation(m_pos[0], m_pos[1], m_pos[2])), world);
        for (auto& p : m_char) if (p.model) m3d::draw(p.model, world);

        // --- HUD ---
        cgfx::use_2d();
        const float CW = cgfx::canvas_w(), CH = cgfx::canvas_h();
        cgfx::draw_rect(0, 0, CW, 54, 0, 0, 0, 0.35f);
        cgfx::draw_text(app.sel.name.c_str(), 24, 12, 30, 0.98f, 0.95f, 0.8f, 1, cgfx::LEFT, 1);
        cgfx::draw_text("West Siauliai Woods", CW - 24, 12, 26, 0.85f, 0.85f, 0.9f, 1, cgfx::RIGHT);
        cgfx::draw_text("WASD move  |  drag mouse to rotate  |  Esc back",
                        CW * 0.5f, CH - 40, 22, 0.9f, 0.9f, 0.92f, 1, cgfx::CENTER);
        if (!m_map.ok)
            cgfx::draw_text("(map failed to load)", CW * 0.5f, CH * 0.5f, 30, 1, 0.5f, 0.5f, 1, cgfx::CENTER);
        cgfx::end_frame();
    }
};

} // namespace

Scene* make_ingame_scene() { return new InGameScene(); }
