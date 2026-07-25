// scene_charselect — the offline "barrack": renders the chosen starter
// character (bodybase + costume) as a real 3D XAC model, slowly rotating, with a
// class/gender/name creation panel. Confirm advances to the loading screen.
#include "app.h"
#include "client_gfx.h"
#include "model3d.h"
#include "char_model.h"
#include "ui.h"
#include "tinymath.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* kClassFolder[4] = {"warrior", "archer", "cleric", "mage"};
const char* kClassName[4]   = {"Swordsman", "Archer", "Cleric", "Wizard"};
const char* kGender[2]      = {"m", "f"};
const char* kGenderName[2]  = {"Male", "Female"};

void mat_to_16(const tmath::Mat4& m, float* out) { std::memcpy(out, &m.m[0][0], 16 * sizeof(float)); }

class CharSelectScene : public Scene {
    std::vector<LoadedPart> m_parts;
    ui::TextField m_name;
    int m_class = 0, m_gender = 0;
    float m_yaw = 2.2f;
    float m_center[3] = {0, 1, 0};
    float m_radius = 2.0f;
    bool m_loaded = false;

    void unload() {
        for (auto& p : m_parts) if (p.model) m3d::destroy(p.model);
        m_parts.clear();
    }

    void reload(App& app) {
        unload();
        std::string dir = std::string("char_hi/pc/") + kClassFolder[m_class] + "_" + kGender[m_gender];
        std::string prefix = std::string(kClassFolder[m_class]) + "_" + kGender[m_gender];
        std::vector<std::string> paths;
        paths.push_back(dir + "/" + prefix + "_bodybase.xac");
        paths.push_back(dir + "/" + prefix + "_costume01.xac");
        // NOTE: head/hair "std" parts are skinned to the BODY skeleton's head bone;
        // rendered standalone they land at the wrong place. Attaching them needs the
        // shared body pose (a later refinement) — see default_head_hair().

        bool framed = false;
        for (const auto& vp : paths) {
            LoadedPart p = load_part(app, vp);
            if (p.ok) {
                if (!framed) {  // frame the camera on the first (body) part
                    for (int k = 0; k < 3; ++k) m_center[k] = p.center[k];
                    m_radius = p.radius > 0.01f ? p.radius : 2.f;
                    framed = true;
                }
                m_parts.push_back(p);
            }
        }
        m_loaded = true;
    }

public:
    void enter(App& app) override {
        m_name.value = app.sel.name.empty() ? "Savior" : app.sel.name;
        m_class = app.sel.classIdx; m_gender = app.sel.gender;
        reload(app);
    }
    void leave(App&) override { unload(); }

    void frame(App& app, const Input& in, float dt) override {
        if (in.esc) { app.go(SceneId::Login); return; }
        m_yaw += dt * 0.5f;

        // ---- 3D character ----
        cgfx::begin_frame(0.10f, 0.11f, 0.14f);
        {
            tmath::Vec3 tgt{m_center[0], m_center[1], m_center[2]};
            float pitch = 0.18f, dist = m_radius * 2.4f;
            float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(m_yaw), sy = std::sin(m_yaw);
            tmath::Vec3 eye{tgt.x + dist * cp * cy, tgt.y + dist * sp, tgt.z + dist * cp * sy};
            tmath::Mat4 view = tmath::lookAtLH(eye, tgt, {0, 1, 0});
            float farP = dist + m_radius * 2.f;
            float nearP = std::max(farP * 1e-4f, dist * 0.02f);
            tmath::Mat4 proj = tmath::perspectiveFovLH(45.f * 3.14159265f / 180.f,
                                                       cgfx::viewport_aspect(), nearP, farP);
            float vp[16], world[16];
            mat_to_16(tmath::mul(view, proj), vp);
            mat_to_16(tmath::identity(), world);
            float camPos[3] = {eye.x, eye.y, eye.z};
            float light[3] = {-0.4f, -0.7f, -0.55f};
            m3d::set_camera(vp, camPos, light);
            for (auto& p : m_parts) if (p.model) m3d::draw(p.model, world);
        }

        // ---- 2D UI overlay ----
        cgfx::use_2d();
        const float CW = cgfx::canvas_w(), CH = cgfx::canvas_h();
        cgfx::draw_text("캐릭터 생성 / CHARACTER CREATION", CW * 0.5f, 40, 40,
                        0.96f, 0.9f, 0.72f, 1, cgfx::CENTER, 1);

        float px = CW - 470, pw = 420;
        cgfx::draw_rect(px, 200, pw, 520, 0.03f, 0.03f, 0.05f, 0.72f);
        float ix = px + 30, iw = pw - 60;

        cgfx::draw_text("Class", ix, 240, 26, 0.75f, 0.75f, 0.8f, 1);
        int dc = ui::stepper(in, {ix, 274, iw, 58}, kClassName[m_class], 30);
        if (dc) { m_class = (m_class + 4 + dc) % 4; reload(app); }

        cgfx::draw_text("Gender", ix, 360, 26, 0.75f, 0.75f, 0.8f, 1);
        int dg = ui::stepper(in, {ix, 394, iw, 58}, kGenderName[m_gender], 30);
        if (dg) { m_gender = (m_gender + 2 + dg) % 2; reload(app); }

        cgfx::draw_text("Name", ix, 480, 26, 0.75f, 0.75f, 0.8f, 1);
        ui::textfield(in, {ix, 514, iw, 58}, m_name, "Character name", 30);

        bool create = ui::button(in, {ix, 600, iw, 64}, "생성 / CREATE", 30);
        if (create || (in.enter && !m_name.focused)) {
            app.sel.classIdx = m_class; app.sel.gender = m_gender;
            app.sel.name = m_name.value.empty() ? "Savior" : m_name.value;
            app.go(SceneId::Loading);
        }
        if (ui::button(in, {ix, 674, iw, 40}, "< Back", 22)) app.go(SceneId::Login);

        if (m_parts.empty())
            cgfx::draw_text("(model not found for this class)", CW * 0.4f, CH * 0.5f, 26,
                            0.9f, 0.5f, 0.5f, 1, cgfx::CENTER);
        cgfx::end_frame();
    }
};

} // namespace

Scene* make_charselect_scene() { return new CharSelectScene(); }
