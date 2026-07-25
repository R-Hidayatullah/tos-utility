// TSV parser — C++ port of tosmole's src/tsv.rs.
//
// Tab-separated language/data tables (ETC.tsv, ITEM.tsv, ...). Each line is a
// row; columns are split on '\t'. Trailing '\r' from CRLF line endings is
// stripped (matching Rust's BufRead::lines).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace tos::tsv {

using Table = std::vector<std::vector<std::string>>;

// Parse one .tsv file into rows of string columns. Throws std::runtime_error if
// the file cannot be opened.
Table parseTsvFile(const std::string& path);

// Parse the ETC.tsv and ITEM.tsv tables in a language folder (returns
// {etc, item}). Reads the two files on separate threads, like the Rust
// parse_language_data_parallel.
std::pair<Table, Table> parseLanguageData(const std::string& langFolder);

} // namespace tos::tsv
