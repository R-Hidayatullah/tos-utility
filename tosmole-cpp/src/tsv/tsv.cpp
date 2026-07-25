#include "tos/tsv/tsv.h"

#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>

namespace tos::tsv {

Table parseTsvFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open TSV file: " + path);

    Table rows;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back(); // strip CRLF's \r
        std::vector<std::string> cols;
        size_t start = 0;
        while (true) {
            size_t tab = line.find('\t', start);
            if (tab == std::string::npos) { cols.push_back(line.substr(start)); break; }
            cols.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        rows.push_back(std::move(cols));
    }
    return rows;
}

std::pair<Table, Table> parseLanguageData(const std::string& langFolder) {
    auto join = [&](const char* name) {
        std::string p = langFolder;
        if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
        return p + name;
    };
    std::string etcPath = join("ETC.tsv");
    std::string itemPath = join("ITEM.tsv");

    auto etcFut = std::async(std::launch::async, parseTsvFile, etcPath);
    auto itemFut = std::async(std::launch::async, parseTsvFile, itemPath);
    Table etc = etcFut.get();
    Table item = itemFut.get();
    return {std::move(etc), std::move(item)};
}

} // namespace tos::tsv
