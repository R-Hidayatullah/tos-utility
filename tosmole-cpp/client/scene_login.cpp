// scene_login — the title/login screen, reproduced from the game's own
// loginui_title layout (login_bg.jpg full-screen + r1_logo.png centred, offset
// up 230px on a 1920x1080 canvas). Offline: the ID/PW box and Start button are
// local only — clicking Start advances to character selection.
#include "app.h"
#include "client_gfx.h"
#include "ui.h"
#include "bgm.h"

#include <string>

namespace {

class LoginScene : public Scene {
    int m_bg = -1, m_logo = -1;
    ui::TextField m_id, m_pw;

public:
    void enter(App& app) override {
        m_bg = app.loadTexture("ui/fixframe/loginui_title/login_bg.jpg");
        m_logo = app.loadTexture("ui/fixframe/loginui_title/r1_logo.png");
        m_pw.password = true;
        m_id.value = "savior";
        std::string bgmPath = app.gameRoot + "/release/bgm/Orgel_Tree_of_Savior.mp3";
        bgm::play_file(bgmPath, true);
    }

    void frame(App& app, const Input& in, float) override {
        if (in.esc) { app.go(SceneId::Quit); return; }

        cgfx::begin_frame(0.02f, 0.02f, 0.03f);
        const float CW = cgfx::canvas_w(), CH = cgfx::canvas_h();

        if (m_bg >= 0) cgfx::draw_sprite(m_bg, 0, 0, CW, CH);
        else cgfx::draw_rect(0, 0, CW, CH, 0.06f, 0.07f, 0.10f, 1);

        // Logo: 688x363, centred, nudged up (matches loginui_title margin).
        if (m_logo >= 0) {
            float lw = 688, lh = 363;
            cgfx::draw_sprite(m_logo, CW * 0.5f - lw * 0.5f, CH * 0.5f - 230 - lh * 0.5f, lw, lh);
        }

        // Login panel.
        float pw = 440, ph = 300, px = CW * 0.5f - pw * 0.5f, py = CH * 0.5f + 120;
        cgfx::draw_rect(px, py, pw, ph, 0.03f, 0.03f, 0.05f, 0.72f);
        cgfx::draw_text("로그인 / LOGIN", px + pw * 0.5f, py + 16, 26,
                        0.92f, 0.82f, 0.55f, 1, cgfx::CENTER);

        float fx = px + 20, fw = pw - 40;
        ui::textfield(in, {fx, py + 60, fw, 54}, m_id, "ID / Team Name", 28);
        ui::textfield(in, {fx, py + 124, fw, 54}, m_pw, "Password", 28);

        bool start = ui::button(in, {fx, py + 196, fw, 62}, "게임 시작 / START", 30);
        if (start || in.enter) app.go(SceneId::CharSelect);

        cgfx::draw_text("Tree of Savior — Offline Client  (no network)",
                        CW * 0.5f, CH - 46, 22, 0.7f, 0.7f, 0.74f, 1, cgfx::CENTER);
        cgfx::end_frame();
    }
};

} // namespace

Scene* make_login_scene() { return new LoginScene(); }
