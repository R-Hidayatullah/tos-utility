// Filtering and sorting for the packet table.
//
// The filter box takes space-separated terms, ANDed together. A bare term
// matches the packet name or the opcode; a `field:value` term matches that
// field; a leading `-` negates. This is small enough to be obvious and big
// enough to answer the questions you actually have of a capture:
//
//   ZC_MOVE            name contains ZC_MOVE
//   dir:c2s zone       client->server, zone link
//   op:3106            one opcode (0x… also works)
//   conn:2 -chk:ok     one connection, checksum not verified ok
//   ip:52.5.58.238     either address
//   port:7001          listen port
//   len>100 len<500    body length range
//
// Unparseable terms are treated as free text rather than rejected: the box
// filters as you type, and half-typed input should narrow, not error.
#pragma once

#include <string>
#include <vector>

#include "capture.h"

namespace view {

enum Column {
    COL_INDEX = 0, COL_TIME, COL_CLOCK, COL_DIR, COL_CONN, COL_LISTEN,
    COL_SRC, COL_DST, COL_OPCODE, COL_NAME, COL_LEN, COL_DECL, COL_WIRE,
    COL_SEQ, COL_CHK, COL_LINK, COL_FLAGS, COL_COUNT
};

struct ColumnDef {
    const wchar_t* title;
    int width;
    bool right;         // numeric columns read better right-aligned
};
extern const ColumnDef kColumns[COL_COUNT];

// One row's text for one column.
std::wstring cell_text(const Packet& p, const Packet& first, int col);

class Filter {
public:
    // Recompiles from the raw box text. Cheap; called on every keystroke.
    void set(const std::wstring& query);
    bool empty() const { return terms_.empty(); }
    bool match(const Packet& p) const;

private:
    struct Term {
        enum Kind { Text, Dir, Op, Conn, Link, Ip, Port, LenCmp, Chk, Seq,
                    Flag } kind = Text;
        bool negate = false;
        std::wstring text;       // lower-cased
        uint64_t num = 0;
        int cmp = 0;             // -1 <, 0 ==, 1 >
    };
    std::vector<Term> terms_;
};

// Sorts `view` (indices into `pkts`) by `col`. Ties break on record order, so
// the table never reshuffles rows that compare equal.
void sort_view(std::vector<uint32_t>& view, const std::vector<Packet>& pkts,
               int col, bool ascending);

}  // namespace view
