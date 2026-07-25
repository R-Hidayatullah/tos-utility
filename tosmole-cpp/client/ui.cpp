#include "ui.h"
#include "client_gfx.h"

#include <cmath>

namespace ui {

static bool g_blinkInit = false;
static double g_time = 0;

bool button(const Input& in, const Rect& r, const char* label, float fontPx) {
    bool over = hit(r, in.mouseX, in.mouseY);
    bool pressed = over && in.mouseDown;
    float base = pressed ? 0.16f : (over ? 0.30f : 0.22f);
    cgfx::draw_rect(r.x, r.y, r.w, r.h, base * 0.9f, base * 0.75f, base * 0.45f, 0.92f);
    // gold border
    float bw = 2.f, gr = over ? 0.95f : 0.72f, gg = over ? 0.82f : 0.58f, gb = 0.36f;
    cgfx::draw_rect(r.x, r.y, r.w, bw, gr, gg, gb, 1);
    cgfx::draw_rect(r.x, r.y + r.h - bw, r.w, bw, gr, gg, gb, 1);
    cgfx::draw_rect(r.x, r.y, bw, r.h, gr, gg, gb, 1);
    cgfx::draw_rect(r.x + r.w - bw, r.y, bw, r.h, gr, gg, gb, 1);
    cgfx::draw_text(label, r.x + r.w * 0.5f, r.y + (r.h - fontPx) * 0.5f, fontPx,
                    0.98f, 0.94f, 0.80f, 1, cgfx::CENTER);
    // Proper click: press AND release both inside the widget.
    return over && in.mouseReleased && hit(r, in.pressX, in.pressY);
}

void textfield(const Input& in, const Rect& r, TextField& f, const char* placeholder,
               float fontPx) {
    if (in.mouseClicked) f.focused = hit(r, in.mouseX, in.mouseY);
    if (f.focused) {
        for (size_t i = 0; i < in.typed.size();) {
            // append one UTF-8 code point at a time (respect maxLen by bytes)
            unsigned char c = in.typed[i];
            int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            if (f.value.size() < f.maxLen * 2)
                f.value.append(in.typed, i, len);
            i += len;
        }
        if (in.backspace && !f.value.empty()) {
            // pop one UTF-8 code point
            size_t n = f.value.size();
            while (n > 0 && (f.value[n - 1] & 0xC0) == 0x80) --n;
            if (n > 0) --n;
            f.value.resize(n);
        }
    }

    cgfx::draw_rect(r.x, r.y, r.w, r.h, 0.06f, 0.06f, 0.08f, 0.85f);
    float g = f.focused ? 0.95f : 0.55f;
    cgfx::draw_rect(r.x, r.y, r.w, 2, g, g * 0.85f, 0.45f, 1);
    cgfx::draw_rect(r.x, r.y + r.h - 2, r.w, 2, g, g * 0.85f, 0.45f, 1);
    cgfx::draw_rect(r.x, r.y, 2, r.h, g, g * 0.85f, 0.45f, 1);
    cgfx::draw_rect(r.x + r.w - 2, r.y, 2, r.h, g, g * 0.85f, 0.45f, 1);

    float ty = r.y + (r.h - fontPx) * 0.5f;
    if (f.value.empty() && !f.focused) {
        cgfx::draw_text(placeholder, r.x + 14, ty, fontPx, 0.6f, 0.6f, 0.62f, 1);
    } else {
        std::string shown = f.value;
        if (f.password) {
            shown.clear();
            // count code points
            int cps = 0;
            for (size_t i = 0; i < f.value.size(); ++i)
                if ((f.value[i] & 0xC0) != 0x80) ++cps;
            for (int i = 0; i < cps; ++i) shown += "*";
        }
        if (f.focused) shown += "|";
        cgfx::draw_text(shown.c_str(), r.x + 14, ty, fontPx, 0.95f, 0.95f, 0.92f, 1);
    }
}

int stepper(const Input& in, const Rect& r, const char* label, float fontPx) {
    Rect left{r.x, r.y, r.h, r.h};
    Rect right{r.x + r.w - r.h, r.y, r.h, r.h};
    int result = 0;
    if (button(in, left, "<", fontPx)) result = -1;
    if (button(in, right, ">", fontPx)) result = +1;
    cgfx::draw_text(label, r.x + r.w * 0.5f, r.y + (r.h - fontPx) * 0.5f, fontPx,
                    0.95f, 0.92f, 0.82f, 1, cgfx::CENTER);
    return result;
}

} // namespace ui
