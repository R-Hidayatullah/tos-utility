// The right-hand panel: one packet as hex and ASCII.
//
// It shades the wire header rather than leaving you to count: opcode,
// sequence and checksum are distinct colours, the twelve bytes a client
// packet carries and the server ignores are greyed, and a variable packet's
// inline size field is boxed. Those four things are where nearly every
// misread of this protocol starts.
//
// Clicking a byte reports its offset and the u8/u16/u32/f32 that start there
// to the parent, which is how you check a field offset against
// packet_schema.json without leaving the window.
#pragma once

#include <windows.h>

#include "capture.h"

namespace view {

// Sent to the parent when the caret moves. wParam = byte offset, or (WPARAM)-1
// when there is no packet. lParam = the HWND of the hex view.
#define HEXN_CARET (WM_APP + 11)

void hexview_register();
HWND hexview_create(HWND parent, int id);

// `data` must outlive the next call. Pass nullptr to clear.
void hexview_set(HWND h, const uint8_t* data, size_t len, const Packet* p);

// Boxes a byte range -- what the template pane calls when a field is picked,
// so "server_time" and the four bytes it is made of are on screen together.
// A zero length clears it.
void hexview_highlight(HWND h, int off, int len);

// Whole packet as a text hexdump, for the clipboard.
std::wstring hexview_text(HWND h);

}  // namespace view
