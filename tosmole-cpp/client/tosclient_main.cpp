// tosclient — a Tree of Savior game *shell* (not a network client): it mounts
// the real IPF archives and walks the authentic scene flow — login → character
// selection/creation → first map — driving the game's own assets locally.
//
// Phase 1: window + unified 2D renderer + VFS + scene manager + authentic login.
// CharSelect / Loading / InGame are placeholders here, fleshed out in phase 2/3.
#include <windows.h>

#include "app.h"
#include "client_gfx.h"
#include "model3d.h"
#include "bgm.h"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Input accumulator (updated by WndProc, snapshotted each frame).
// ---------------------------------------------------------------------------
namespace {
Input g_in;
int   g_mousePxX = 0, g_mousePxY = 0;
bool  g_quit = false;

void append_utf8(std::string& s, wchar_t wc) {
    char buf[8];
    int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, buf, sizeof(buf), nullptr, nullptr);
    if (n > 0) s.append(buf, n);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        cgfx::resize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_MOUSEMOVE:
        g_mousePxX = (short)LOWORD(lParam);
        g_mousePxY = (short)HIWORD(lParam);
        return 0;
    case WM_LBUTTONDOWN:
        g_in.mouseDown = true; g_in.mouseClicked = true;
        SetCapture(hWnd);
        return 0;
    case WM_LBUTTONUP:
        g_in.mouseDown = false; g_in.mouseReleased = true;
        ReleaseCapture();
        return 0;
    case WM_CHAR: {
        wchar_t wc = (wchar_t)wParam;
        if (wc == 8) g_in.backspace = true;
        else if (wc == 13) g_in.enter = true;
        else if (wc == 9) g_in.tab = true;
        else if (wc == 27) g_in.esc = true;
        else if (wc >= 32) append_utf8(g_in.typed, wc);
        return 0;
    }
    case WM_CLOSE:
        g_quit = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// --- placeholder scenes (phase 2/3 replace these) --------------------------
class PlaceholderScene : public Scene {
    const char* m_title;
    SceneId m_back, m_forward;
    const char* m_fwdLabel;
public:
    PlaceholderScene(const char* title, SceneId back, SceneId forward, const char* fwd)
        : m_title(title), m_back(back), m_forward(forward), m_fwdLabel(fwd) {}
    void frame(App& app, const Input& in, float) override {
        if (in.esc) { app.go(m_back); return; }
        cgfx::begin_frame(0.05f, 0.06f, 0.08f);
        float CW = cgfx::canvas_w(), CH = cgfx::canvas_h();
        cgfx::draw_text(m_title, CW * 0.5f, CH * 0.5f - 60, 54,
                        0.95f, 0.9f, 0.75f, 1, cgfx::CENTER);
        cgfx::draw_text("(coming in the next build phase)", CW * 0.5f, CH * 0.5f + 10, 26,
                        0.7f, 0.7f, 0.72f, 1, cgfx::CENTER);
        cgfx::draw_text("Esc = back   |   click below to continue", CW * 0.5f, CH * 0.5f + 60, 22,
                        0.6f, 0.6f, 0.64f, 1, cgfx::CENTER);
        // simple continue button
        float bw = 320, bh = 60, bx = CW * 0.5f - bw * 0.5f, by = CH * 0.5f + 120;
        bool over = in.mouseX >= bx && in.mouseX <= bx + bw && in.mouseY >= by && in.mouseY <= by + bh;
        cgfx::draw_rect(bx, by, bw, bh, over ? 0.3f : 0.2f, 0.22f, 0.14f, 0.9f);
        cgfx::draw_text(m_fwdLabel, CW * 0.5f, by + 16, 28, 0.95f, 0.9f, 0.78f, 1, cgfx::CENTER);
        bool pressInside = in.pressX >= bx && in.pressX <= bx + bw && in.pressY >= by && in.pressY <= by + bh;
        if (over && in.mouseReleased && pressInside) app.go(m_forward);
        cgfx::end_frame();
    }
};
} // namespace

// Scene factory.
Scene* make_login_scene();
Scene* make_charselect_scene();
Scene* make_loading_scene();
Scene* make_ingame_scene();
Scene* make_scene(SceneId id) {
    switch (id) {
    case SceneId::Login:     return make_login_scene();
    case SceneId::CharSelect:return make_charselect_scene();
    case SceneId::Loading:   return make_loading_scene();
    case SceneId::InGame:    return make_ingame_scene();
    default:                 return make_login_scene();
    }
}

// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int nShow) {
    // Game root: command line arg, else the known default.
    std::string root = "C:/Users/Ridwan Hidayatullah/Documents/TreeOfSaviorCN";
    if (lpCmdLine && *lpCmdLine) {
        root = lpCmdLine;
        if (root.size() >= 2 && root.front() == '"' && root.back() == '"')
            root = root.substr(1, root.size() - 2);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TosClientWnd";
    RegisterClassExW(&wc);

    RECT wr{0, 0, 1280, 720};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hWnd = CreateWindowExW(0, wc.lpszClassName, L"Tree of Savior — Offline Client",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                wr.right - wr.left, wr.bottom - wr.top,
                                nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return 1;

    if (!cgfx::init(hWnd)) {
        MessageBoxW(hWnd, L"D3D11 initialisation failed.", L"tosclient", MB_ICONERROR);
        return 1;
    }
    cgfx::set_canvas(1920, 1080);
    m3d::init();

    App app;
    app.gameRoot = root;
    app.vfs.scanGameRoot(root);
    app.gameData.load(root, app.vfs);

    // Register the game fonts (imcm_book = UI, imcm_original = headings).
    std::vector<uint8_t> f0, f1;
    if (app.readAsset("font/imcm_book.ttf", f0)) cgfx::add_font(f0.data(), f0.size());
    if (app.readAsset("font/imcm_original.ttf", f1)) cgfx::add_font(f1.data(), f1.size());

    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    Scene* scene = make_scene(SceneId::Login);
    scene->enter(app);

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    MSG msg;
    while (!g_quit) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g_quit) break;

        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float dt = float(double(now.QuadPart - prev.QuadPart) / double(freq.QuadPart));
        prev = now;
        if (dt > 0.1f) dt = 0.1f;

        // Snapshot input for this frame.
        cgfx::window_to_canvas(g_mousePxX, g_mousePxY, &g_in.mouseX, &g_in.mouseY);
        if (g_in.mouseClicked) { g_in.pressX = g_in.mouseX; g_in.pressY = g_in.mouseY; }
        g_in.keyW = (GetAsyncKeyState('W') & 0x8000) != 0;
        g_in.keyA = (GetAsyncKeyState('A') & 0x8000) != 0;
        g_in.keyS = (GetAsyncKeyState('S') & 0x8000) != 0;
        g_in.keyD = (GetAsyncKeyState('D') & 0x8000) != 0;

        scene->frame(app, g_in, dt);

        // Clear per-frame latches.
        g_in.typed.clear();
        g_in.mouseClicked = g_in.mouseReleased = false;
        g_in.backspace = g_in.enter = g_in.tab = g_in.esc = false;

        // Perform any requested scene switch.
        if (app.switching) {
            app.switching = false;
            if (app.next == SceneId::Quit) { g_quit = true; }
            else {
                scene->leave(app);
                delete scene;
                scene = make_scene(app.next);
                scene->enter(app);
            }
        }
    }

    if (scene) { scene->leave(app); delete scene; }
    bgm::shutdown();
    cgfx::shutdown();
    return 0;
}
