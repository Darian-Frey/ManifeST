#include "manifest/MenuImporter.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace manifest {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
    return s;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Canonical disk-number form: strip leading zeros but keep the value
// printable ("032" → "32", "0" → "0"). Any trailing A/B sub-disk suffix
// stays as-is ("013A" → "13A").
std::string canonicalDiskNumber(const std::string& s) {
    std::size_t end_digits = 0;
    while (end_digits < s.size() &&
           std::isdigit(static_cast<unsigned char>(s[end_digits]))) ++end_digits;
    if (end_digits == 0) return s;
    std::size_t first_nonzero = 0;
    while (first_nonzero + 1 < end_digits && s[first_nonzero] == '0') ++first_nonzero;
    return s.substr(first_nonzero, end_digits - first_nonzero) + s.substr(end_digits);
}

std::string readFileText(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open: " + p.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// In-memory working catalog before we serialize to JSON.
// group_code → { disk_number → [games, in disk-position order] }
using Working = std::unordered_map<std::string,
    std::unordered_map<std::string, std::vector<std::string>>>;

void mergeIntoJson(const Working& w,
                   const std::filesystem::path& json_path,
                   MenuImporter::Summary& s) {
    QJsonObject root;
    if (std::filesystem::exists(json_path)) {
        QFile in(QString::fromStdString(json_path.string()));
        if (in.open(QIODevice::ReadOnly)) {
            QJsonParseError err{};
            auto doc = QJsonDocument::fromJson(in.readAll(), &err);
            if (err.error == QJsonParseError::NoError && doc.isObject()) {
                root = doc.object();
            }
            in.close();
        }
    }

    for (const auto& [group, disks] : w) {
        QJsonObject group_obj;
        if (root.contains(QString::fromStdString(group)) &&
            root.value(QString::fromStdString(group)).isObject()) {
            group_obj = root.value(QString::fromStdString(group)).toObject();
        }
        for (const auto& [num, games] : disks) {
            QJsonArray arr;
            for (const auto& g : games) arr.append(QString::fromStdString(g));
            group_obj[QString::fromStdString(num)] = arr;
            s.games_total += games.size();
            ++s.disks_touched;
        }
        root[QString::fromStdString(group)] = group_obj;
    }

    std::filesystem::create_directories(json_path.parent_path());
    QFile out(QString::fromStdString(json_path.string()));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("cannot write: " + json_path.string());
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.close();
}

// ---- spiny.org format ---------------------------------------------------
// Lines look like:    CD1   FIRE ZONE   MUNSTER   TRACKSUIT MANAGER
// Split on tabs. First field is the disk code, rest are games.

Working parseSpiny(const std::string& src_in, MenuImporter::Summary& s) {
    // Source has a few rendering bugs where a row is concatenated to the end
    // of the previous row, e.g. "STUNTTEAMCD12\tLAST TROOPER...". Inject a
    // newline before every mid-line CD-token so each disk lands on its own
    // line before we tokenize.
    static const std::regex split_re(R"((\S)(CD\d+[A-Za-z]?\s))",
                                     std::regex::icase);
    std::string src = std::regex_replace(src_in, split_re, "$1\n$2");

    Working w;
    std::istringstream in(src);
    std::string line;
    static const std::regex cd_re(R"(^CD(\d+[A-Za-z]?)\s*$)", std::regex::icase);

    while (std::getline(in, line)) {
        ++s.lines_read;
        // Split on tabs; tolerate stray CR.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::vector<std::string> cols;
        std::string cur;
        for (char c : line) {
            if (c == '\t') { cols.push_back(trim(cur)); cur.clear(); }
            else           { cur += c; }
        }
        cols.push_back(trim(cur));
        // Drop empties.
        cols.erase(std::remove_if(cols.begin(), cols.end(),
                                  [](const std::string& x){ return x.empty(); }),
                   cols.end());
        if (cols.size() < 2) { ++s.skipped; continue; }

        std::smatch m;
        if (!std::regex_match(cols.front(), m, cd_re)) { ++s.skipped; continue; }
        const std::string disk_num = canonicalDiskNumber(m[1].str());
        auto& bucket = w["MB"][disk_num];
        for (std::size_t i = 1; i < cols.size(); ++i) bucket.push_back(cols[i]);
    }
    return w;
}

// ---- 8bitchip MenuDG format --------------------------------------------
// Structure is an HTML table:
//   <tr><td>GameName</td><td>Req</td><td>Seq</td><td>CODE</td><td>Type</td></tr>
// Continuation rows for the same game carry empty GameName cells:
//   <tr><td></td><td></td><td>A</td><td>GG075</td><td></td></tr>
//
// We carry the last non-empty GameName as the "current game" across rows
// and emit a (group_code, disk_num, game) triple per row.
//
// Regex HTML parsing is fragile but the source page has a very regular
// row shape (single-line <tr>…</tr> with <td> cells that may contain
// inline markup). We strip tags from cell content and use the <tr> /
// <td> boundaries as the structural cues.

Working parseBitchip(const std::string& src_in, MenuImporter::Summary& s) {
    Working w;

    // Common HTML entity fixups.
    std::string src = src_in;
    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to){
        std::size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(src, "&amp;",  "&");
    replaceAll(src, "&nbsp;", " ");
    replaceAll(src, "&lt;",   "<");
    replaceAll(src, "&gt;",   ">");
    replaceAll(src, "&#39;",  "'");
    replaceAll(src, "&quot;", "\"");

    static const std::regex row_re(R"(<tr[^>]*>([\s\S]*?)</tr>)",
                                   std::regex::icase);
    static const std::regex cell_re(R"(<td[^>]*>([\s\S]*?)</td>)",
                                    std::regex::icase);
    static const std::regex tag_re(R"(<[^>]+>)");
    static const std::regex code_re(R"(^([A-Z]{2})(\d{1,4})([A-Za-z]*)$)");

    std::string current_game;

    auto begin = std::sregex_iterator(src.begin(), src.end(), row_re);
    auto end   = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        ++s.lines_read;
        const std::string row_inner = (*it)[1].str();

        // Pull out the <td> cells.
        std::vector<std::string> cells;
        auto cb = std::sregex_iterator(row_inner.begin(), row_inner.end(), cell_re);
        auto ce = std::sregex_iterator();
        for (auto ci = cb; ci != ce; ++ci) {
            std::string c = (*ci)[1].str();
            c = std::regex_replace(c, tag_re, "");   // strip inline tags
            // Collapse whitespace.
            std::string clean;
            bool prev_ws = false;
            for (char ch : c) {
                if (std::isspace(static_cast<unsigned char>(ch))) {
                    if (!prev_ws && !clean.empty()) clean += ' ';
                    prev_ws = true;
                } else {
                    clean += ch;
                    prev_ws = false;
                }
            }
            cells.push_back(trim(clean));
        }

        // Header rows (e.g. "Name | Req | Seq | Disk | Type") and the
        // group-code index at the top aren't data rows — they're harmless
        // to let through the regex check below, but we'll skip them.
        if (cells.size() < 4) { ++s.skipped; continue; }

        const std::string& game_cell = cells[0];
        const std::string& disk_cell = cells[3];
        if (!game_cell.empty()) current_game = game_cell;
        if (current_game.empty()) { ++s.skipped; continue; }

        std::smatch m;
        const std::string code = upper(disk_cell);
        if (!std::regex_match(code, m, code_re)) { ++s.skipped; continue; }

        const std::string group_code = m[1].str();
        const std::string disk_num   = canonicalDiskNumber(m[2].str());
        w[group_code][disk_num].push_back(current_game);
    }
    return w;
}

} // namespace

MenuImporter::Summary
MenuImporter::importFile(Format fmt,
                         const std::filesystem::path& input,
                         const std::filesystem::path& json_out) {
    Summary s;
    const std::string src = readFileText(input);
    Working w = (fmt == Format::Spiny) ? parseSpiny(src, s)
                                       : parseBitchip(src, s);
    mergeIntoJson(w, json_out, s);
    return s;
}

} // namespace manifest
