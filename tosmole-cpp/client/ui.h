// ui.h — tiny immediate-mode widgets drawn with cgfx, driven by the Input
// snapshot. Enough for the login box and character-creation panel.
#pragma once

#include "app.h"
#include <string>

namespace ui {

struct Rect { float x, y, w, h; };
inline bool hit(const Rect& r, float px, float py) {
    return px >= r.x && px <= r.x + r.w && py >= r.y && py <= r.y + r.h;
}

// A text input field with its own value + focus state.
struct TextField {
    std::string value;
    bool focused = false;
    bool password = false;
    size_t maxLen = 24;
};

// Filled/bordered button; returns true on click (mouse released inside).
bool button(const Input& in, const Rect& r, const char* label, float fontPx = 30);

// Text field: handles focus, typing, backspace. `placeholder` shows when empty.
void textfield(const Input& in, const Rect& r, TextField& f, const char* placeholder,
               float fontPx = 30);

// A simple left/right stepper (< label >). Returns -1/+1 when an arrow is
// clicked this frame, else 0.
int stepper(const Input& in, const Rect& r, const char* label, float fontPx = 30);

} // namespace ui
