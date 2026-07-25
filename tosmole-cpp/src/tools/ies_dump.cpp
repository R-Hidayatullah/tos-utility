// ies_dump / tsv_dump — validate the IES and TSV parsers against real files.
//
//   ies_dump <file.ies>              dump header + columns + first rows
//   ies_dump --mesh <file.ies>       dump the Mesh->Path map
//   ies_dump --tsv <file.tsv>        dump a TSV table (first rows)
#include "tos/ies/ies.h"
#include "tos/tsv/tsv.h"

#include <cstdio>
#include <cstring>
#include <string>

static int dumpIes(const char* path) {
    auto root = tos::ies::IESRoot::fromFile(path);
    const auto& h = root.header;
    std::printf("IES %s\n", path);
    std::printf("  idspace=%s keyspace=%s version=%u\n",
                h.idspace.c_str(), h.keyspace.c_str(), h.version);
    std::printf("  sizes: info=%u data=%u total=%u  useClassId=%u\n",
                h.infoSize, h.dataSize, h.totalSize, h.useClassId);
    std::printf("  rows(numField)=%u columns=%u  numeric=%u string=%u\n",
                h.numField, h.numColumn, h.numColumnNumber, h.numColumnString);

    std::printf("  columns:\n");
    for (size_t i = 0; i < root.columns.size(); ++i) {
        const auto& c = root.columns[i];
        std::printf("    [%zu] column=\"%s\" name=\"%s\" type=%u access=%u sync=%u decl=%u\n",
                    i, c.column.c_str(), c.name.c_str(), c.typeData, c.accessData, c.syncData, c.declIdx);
    }

    size_t nrows = root.data.size() < 8 ? root.data.size() : 8;
    std::printf("  first %zu rows:\n", nrows);
    for (size_t i = 0; i < nrows; ++i) {
        const auto& r = root.data[i];
        std::printf("    #%d key=\"%s\" floats=[", r.index, r.rowText.text.c_str());
        for (size_t k = 0; k < r.floats.size(); ++k)
            std::printf("%s%.3g", k ? "," : "", r.floats[k]);
        std::printf("] texts=[");
        for (size_t k = 0; k < r.texts.size(); ++k)
            std::printf("%s\"%s\"", k ? "," : "", r.texts[k].text.c_str());
        std::printf("]\n");
    }
    return 0;
}

static int dumpMesh(const char* path) {
    auto root = tos::ies::IESRoot::fromFile(path);
    auto map = root.extractMeshPathMap();
    std::printf("Mesh->Path map (%zu entries):\n", map.size());
    for (const auto& [mesh, p] : map) std::printf("  %s -> %s\n", mesh.c_str(), p.c_str());
    return 0;
}

static int dumpTsv(const char* path) {
    auto table = tos::tsv::parseTsvFile(path);
    std::printf("TSV %s: %zu rows\n", path, table.size());
    size_t nrows = table.size() < 10 ? table.size() : 10;
    for (size_t i = 0; i < nrows; ++i) {
        std::printf("  row %zu (%zu cols):", i, table[i].size());
        size_t ncol = table[i].size() < 6 ? table[i].size() : 6;
        for (size_t k = 0; k < ncol; ++k) std::printf(" | %s", table[i][k].c_str());
        std::printf("\n");
    }
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::strcmp(argv[1], "--mesh") == 0) return dumpMesh(argv[2]);
        if (argc == 3 && std::strcmp(argv[1], "--tsv") == 0) return dumpTsv(argv[2]);
        if (argc == 2) return dumpIes(argv[1]);
        std::fprintf(stderr, "usage: ies_dump [--mesh|--tsv] <file>\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
