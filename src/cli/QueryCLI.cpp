#include "manifest/QueryCLI.hpp"

#include "manifest/Database.hpp"
#include "manifest/HatariLauncher.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(MANIFEST_HAVE_READLINE)
#  include <readline/history.h>
#  include <readline/readline.h>
#endif

namespace manifest::cli {

namespace {

// ---- line input ----------------------------------------------------------

#if defined(MANIFEST_HAVE_READLINE)
std::optional<std::string> readLine(const char* prompt) {
    char* line = ::readline(prompt);
    if (!line) return std::nullopt;        // Ctrl-D
    std::string out(line);
    if (!out.empty()) ::add_history(line);
    std::free(line);
    return out;
}

void initHistory() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        const std::string path = std::string(home) + "/.manifest_history";
        ::read_history(path.c_str());
    }
}

void saveHistory() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        const std::string path = std::string(home) + "/.manifest_history";
        ::write_history(path.c_str());
    }
}
#else
std::optional<std::string> readLine(const char* prompt) {
    std::cout << prompt << std::flush;
    std::string s;
    if (!std::getline(std::cin, s)) return std::nullopt;
    return s;
}
void initHistory() {}
void saveHistory() {}
#endif

// ---- helpers -------------------------------------------------------------

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
    return s;
}

std::pair<std::string, std::string> splitOnce(const std::string& line) {
    const auto pos = line.find_first_of(" \t");
    if (pos == std::string::npos) return {line, ""};
    return {line.substr(0, pos), trim(line.substr(pos + 1))};
}

std::string optStr(const std::optional<std::string>& v) { return v.value_or(std::string{}); }

std::string truncate(std::string s, std::size_t n) {
    if (s.size() <= n) return s;
    s.resize(n - 1);
    s.push_back('.');
    return s;
}

void printDisksTable(const std::vector<DiskRecord>& rows) {
    if (rows.empty()) { std::puts("(no matches)"); return; }
    std::printf("%-5s  %-30s  %-18s  %-4s  %-12s\n",
                "ID", "Title", "Publisher", "Year", "Label");
    std::printf("%-5s  %-30s  %-18s  %-4s  %-12s\n",
                "----", "-----", "---------", "----", "-----");
    for (const auto& r : rows) {
        std::printf("%-5lld  %-30s  %-18s  %-4s  %-12s\n",
                    static_cast<long long>(r.id),
                    truncate(r.identified_title.value_or("(unidentified)"), 30).c_str(),
                    truncate(optStr(r.publisher), 18).c_str(),
                    r.year ? std::to_string(*r.year).c_str() : "",
                    truncate(r.volume_label, 12).c_str());
    }
    std::printf("(%zu rows)\n", rows.size());
}

void printInfo(const DiskRecord& r) {
    std::printf("id              %lld\n",   static_cast<long long>(r.id));
    std::printf("title           %s\n",     r.identified_title.value_or("(unidentified)").c_str());
    std::printf("publisher       %s\n",     optStr(r.publisher).c_str());
    std::printf("year            %s\n",     r.year ? std::to_string(*r.year).c_str() : "");
    std::printf("path            %s\n",     r.path.c_str());
    std::printf("filename        %s\n",     r.filename.c_str());
    std::printf("format          %s\n",     r.format.c_str());
    std::printf("image_hash      %s\n",     r.image_hash.c_str());
    std::printf("volume_label    %s\n",     r.volume_label.c_str());
    std::printf("oem_name        %s\n",     r.oem_name.c_str());
    std::printf("geometry        %u sides / %u tracks / %u spt / %u bps\n",
                r.sides, r.tracks, r.sectors_per_track, r.bytes_per_sector);
    if (!r.tags.empty()) {
        std::printf("tags           ");
        for (const auto& t : r.tags) std::printf(" %s", t.c_str());
        std::puts("");
    }
    if (r.notes && !r.notes->empty()) {
        std::printf("notes\n");
        // Indent each line for readable display.
        std::string line;
        for (char c : *r.notes) {
            if (c == '\n') { std::printf("  %s\n", line.c_str()); line.clear(); }
            else           { line += c; }
        }
        if (!line.empty()) std::printf("  %s\n", line.c_str());
    }
    if (!r.menu_games.empty()) {
        std::printf("menu games (%zu):\n", r.menu_games.size());
        for (const auto& g : r.menu_games) {
            std::printf("  %2d. %s\n", g.position + 1, g.name.c_str());
        }
    }
    if (!r.files.empty()) {
        std::printf("files (%zu):\n", r.files.size());
        for (const auto& f : r.files) {
            std::printf("  %c %-14s  %8u bytes  %s\n",
                        f.is_launcher ? '*' : ' ',
                        f.filename.c_str(), f.size_bytes, f.file_hash.c_str());
        }
    }
}

void printHelp() {
    std::puts(
        "Commands:\n"
        "  find <term>     Search title / publisher / label / filenames\n"
        "  list            List every disk in the catalog\n"
        "  info <id>       Show full record for a disk\n"
        "  launch <id>     Launch the disk in Hatari (must be on $PATH)\n"
        "  tags            List all tags with counts\n"
        "  tags <tag>      List disks carrying <tag>\n"
        "  dupes           Show duplicate-image groups\n"
        "  sets            Show multi-disk sets\n"
        "  note <id>       Edit / clear the note for a disk (multi-line, end with .)\n"
        "  help            Show this message\n"
        "  quit | exit     Leave the shell");
}

} // namespace

QueryCLI::QueryCLI(Database& db) : db_(db) {}

int QueryCLI::findOnce(const std::string& term) {
    auto rows = db_.queryByTitle(term);
    printDisksTable(rows);
    return rows.empty() ? 1 : 0;
}

int QueryCLI::run() {
    initHistory();
    std::puts("ManifeST query shell — type `help` for commands, `quit` to exit.");

    while (true) {
        auto line = readLine("manifest> ");
        if (!line) { std::puts(""); break; }

        const std::string trimmed = trim(*line);
        if (trimmed.empty()) continue;

        const auto [cmd, args] = splitOnce(trimmed);

        if (cmd == "quit" || cmd == "exit" || cmd == "q") break;

        if (cmd == "help" || cmd == "?") { printHelp(); continue; }

        try {
            if (cmd == "find") {
                if (args.empty()) { std::puts("usage: find <term>"); continue; }
                printDisksTable(db_.queryByTitle(args));
            }
            else if (cmd == "list") {
                printDisksTable(db_.listAll());
            }
            else if (cmd == "info") {
                if (args.empty()) { std::puts("usage: info <id>"); continue; }
                const int64_t id = std::stoll(args);
                if (auto r = db_.queryById(id)) printInfo(*r);
                else std::printf("no such id: %lld\n", static_cast<long long>(id));
            }
            else if (cmd == "launch") {
                if (args.empty()) { std::puts("usage: launch <id>"); continue; }
                const int64_t id = std::stoll(args);
                auto r = db_.queryById(id);
                if (!r) { std::printf("no such id: %lld\n", static_cast<long long>(id)); continue; }
                const auto result = HatariLauncher::launch(r->path);
                if (result.launched) std::printf("launched: %s\n", r->path.c_str());
                else                 std::fprintf(stderr, "ERR: %s\n", result.error.c_str());
            }
            else if (cmd == "tags") {
                if (args.empty()) {
                    for (const auto& tc : db_.listAllTags()) {
                        std::printf("  %-24s  %zu\n", tc.tag.c_str(), tc.count);
                    }
                } else {
                    std::vector<DiskRecord> rows;
                    for (auto id : db_.idsWithTag(args)) {
                        if (auto r = db_.queryById(id)) rows.push_back(*r);
                    }
                    std::sort(rows.begin(), rows.end(),
                              [](const DiskRecord& a, const DiskRecord& b){
                                  return a.identified_title.value_or(a.filename)
                                       < b.identified_title.value_or(b.filename);
                              });
                    printDisksTable(rows);
                }
            }
            else if (cmd == "dupes") {
                const auto groups = db_.listDuplicates();
                if (groups.empty()) { std::puts("(no duplicates)"); continue; }
                for (const auto& g : groups) {
                    std::printf("== %s (%zu copies) ==\n",
                                g.image_hash.c_str(), g.disks.size());
                    for (const auto& d : g.disks) {
                        std::printf("  %5lld  %s\n",
                                    static_cast<long long>(d.id), d.path.c_str());
                    }
                }
            }
            else if (cmd == "note") {
                if (args.empty()) { std::puts("usage: note <id>"); continue; }
                const int64_t id = std::stoll(args);
                auto r = db_.queryById(id);
                if (!r) { std::printf("no such id: %lld\n", static_cast<long long>(id)); continue; }

                std::printf("Current note for id=%lld (%s):\n",
                            static_cast<long long>(id),
                            r->identified_title.value_or(r->filename).c_str());
                if (r->notes && !r->notes->empty()) std::printf("  %s\n", r->notes->c_str());
                else                                std::puts("  (none)");
                std::puts("Enter new note; single '.' on its own line finishes. "
                          "Empty input clears the note.");

                std::string buf;
                std::string line;
                while (std::getline(std::cin, line)) {
                    if (line == ".") break;
                    if (!buf.empty()) buf += '\n';
                    buf += line;
                }
                db_.setNotes(id, buf);
                std::printf("%s\n", buf.empty() ? "Note cleared." : "Note saved.");
            }
            else if (cmd == "sets") {
                const auto sets = db_.listDiskSets();
                if (sets.empty()) { std::puts("(no multi-disk sets)"); continue; }
                for (const auto& s : sets) {
                    std::printf("== set %lld · %s ==\n",
                                static_cast<long long>(s.set_id), s.title.c_str());
                    for (const auto& [disk_id, num] : s.members) {
                        if (auto r = db_.queryById(disk_id)) {
                            std::printf("  disk %d  id=%lld  %s\n",
                                        num, static_cast<long long>(disk_id),
                                        r->filename.c_str());
                        }
                    }
                }
            }
            else {
                std::printf("unknown command: %s — type `help`\n", cmd.c_str());
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ERR: %s\n", e.what());
        }
    }

    saveHistory();
    return 0;
}

} // namespace manifest::cli
