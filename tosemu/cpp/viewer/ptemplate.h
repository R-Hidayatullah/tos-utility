// The third panel: the selected packet's bytes read as named fields.
//
// A hex dump tells you what the bytes are; this tells you what they mean, the
// way a 010 Editor template does -- name, type, value, offset, size, one row
// per field, and clicking a row lights up exactly those bytes in the hex
// panel.
//
// Layouts come from packet_template.tsv (name_fields.py), which fuses three
// sources: the client's own handlers via IDA, layouts verified by rebuilding
// captured replies byte-for-byte, and value statistics from the captures. The
// three do not carry the same weight, so every row keeps its origin and the
// pane shows it -- a name backed by a byte-exact rebuild and a name backed by
// "these four bytes look like a float" must not read the same on screen.
//
// Bytes no layout accounts for are not hidden. They get their own rows, which
// is the point: the gaps are what a capture is opened to find.
#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

#include "capture.h"

namespace view {

// Sent to the parent when a field row is picked. wParam = byte offset,
// lParam = length. Both zero means nothing is selected.
#define TPLN_PICK (WM_APP + 12)

// One field of one layout, as parsed from packet_template.tsv.
struct TField {
    uint32_t off = 0;
    uint32_t size = 0;          // 0 = runs to the end of the packet
    std::string type;           // u8 u16 u32 u64 f32 f32[N] str[N] bytes[N]
    std::string name;
    std::string fmt;            // hex bool time text ip, or empty
    std::string origin;         // docs/02, ida:sub_…, census, bytes, or empty
    std::string note;
};

struct TLayout {
    uint16_t op = 0;
    std::string name;
    uint32_t size = 0;
    std::string coverage;       // exact partial variable curated none
    std::vector<TField> fields;
};

class Templates {
public:
    // Looks beside the dump, beside the exe, and a few levels up from either,
    // the same way the opcode table is found.
    bool load(const std::wstring& beside);
    const TLayout* find(uint16_t op) const;
    bool ready() const { return !by_op_.empty(); }
    size_t count() const { return by_op_.size(); }

private:
    std::map<uint16_t, TLayout> by_op_;
};

// A row as displayed. Header rows are synthesised from the packet itself
// rather than stored in the template, because the wire header is the same for
// every opcode and its shape depends on direction and link, not on the layout.
struct TRow {
    enum Kind { Header, Named, Guess, Gap, Note } kind = Named;
    std::wstring name, type, value, origin;
    uint32_t off = 0, size = 0;
    int depth = 0;              // 1 for the elements of an array field
};

// Reads `data` through `lay` (which may be null) and returns what to show.
std::vector<TRow> template_rows(const Packet& p, const uint8_t* data,
                                size_t len, const TLayout* lay);

void ptemplate_register();
HWND ptemplate_create(HWND parent, int id);

// `data` must outlive the next call. Pass nullptr to clear.
void ptemplate_set(HWND h, const uint8_t* data, size_t len, const Packet* p,
                   const TLayout* lay);

// Selects the row covering `off`, for when the hex caret moves. -1 clears.
void ptemplate_sync(HWND h, int off);

// The whole table as text, for the clipboard.
std::wstring ptemplate_text(HWND h);

}  // namespace view
