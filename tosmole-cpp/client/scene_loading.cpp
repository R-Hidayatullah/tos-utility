// scene_loading — a brief loading screen shown before the (blocking) map load.
// It renders the loading art for a few frames, then hands off to InGame, whose
// enter() performs the heavy load while this last frame stays on screen.
#include "app.h"
#include "client_gfx.h"

namespace {

class LoadingScene : public Scene {
    int m_bg = -1;
    float m_t = 0;
public:
    void enter(App& app) override {
        m_bg = app.loadTexture("ui/fixframe/loadingbg/loadingbg.jpg");
        if (m_bg < 0) m_bg = app.loadTexture("ui/fixframe/loginui_title/login_bg.jpg");
        m_t = 0;
    }
    void frame(App& app, const Input&, float dt) override {
        m_t += dt;
        cgfx::begin_frame(0.02f, 0.02f, 0.03f);
        const float CW = cgfx::canvas_w(), CH = cgfx::canvas_h();
        if (m_bg >= 0) cgfx::draw_sprite(m_bg, 0, 0, CW, CH, 0.8f, 0.8f, 0.8f, 1);
        cgfx::draw_rect(0, CH - 120, CW, 120, 0, 0, 0, 0.55f);
        cgfx::draw_text("West Siauliai Woods", CW * 0.5f, CH - 100, 40,
                        0.96f, 0.9f, 0.72f, 1, cgfx::CENTER, 1);
        const char* dots = (((int)(m_t * 2)) % 4 == 0) ? "Loading" :
                           (((int)(m_t * 2)) % 4 == 1) ? "Loading." :
                           (((int)(m_t * 2)) % 4 == 2) ? "Loading.." : "Loading...";
        cgfx::draw_text(dots, CW * 0.5f, CH - 52, 26, 0.8f, 0.8f, 0.84f, 1, cgfx::CENTER);
        cgfx::end_frame();
        if (m_t > 0.6f) app.go(SceneId::InGame);
    }
};

} // namespace

Scene* make_loading_scene() { return new LoadingScene(); }
