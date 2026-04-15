#include "manifest/GameStringScanner.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace manifest {

namespace {

bool isPrintable(uint8_t c) {
    return c >= 0x20 && c < 0x7F;
}

std::string normalize(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    for (char c : s) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isspace(uc)) {
            if (!prev_space && !out.empty()) out += ' ';
            prev_space = true;
        } else {
            out += static_cast<char>(std::toupper(uc));
            prev_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// A string that looks like a FAT12 directory entry filename + extension
// (8 + 3 ASCII bytes with space padding) — skip these, they flood the
// matches with things like "MEAN    PRG" that aren't game titles.
bool looksLikeFatDirent(const std::string& s) {
    // Canonical shape: exactly 11 ASCII chars, spaces-packed, with a
    // run of trailing spaces inside the name slot.
    if (s.size() != 11) return false;
    // At least one space in the first 8 characters (name padding),
    // AND last 3 chars are letters/digits or spaces.
    const bool has_name_pad =
        std::find(s.begin(), s.begin() + 8, ' ') != s.begin() + 8;
    if (!has_name_pad) return false;
    for (int i = 8; i < 11; ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c != ' ' && !std::isalnum(c)) return false;
    }
    return true;
}

} // namespace

std::vector<std::string>
GameStringScanner::extractRuns(const std::vector<uint8_t>& bytes, const Config& cfg) {
    std::vector<std::string> raw;
    std::string cur;
    auto flush = [&]{
        if (cur.size() >= cfg.min_run && cur.size() <= cfg.max_run) {
            raw.push_back(std::move(cur));
        }
        cur.clear();
    };
    for (uint8_t b : bytes) {
        if (isPrintable(b)) cur += static_cast<char>(b);
        else                flush();
    }
    flush();

    // Normalise + dedupe in one pass.
    std::vector<std::string> out;
    out.reserve(raw.size());
    std::unordered_map<std::string, std::size_t> seen;
    for (auto& s : raw) {
        if (looksLikeFatDirent(s)) continue;
        auto n = normalize(std::move(s));
        if (n.size() < cfg.min_run) continue;
        if (seen.insert({n, out.size()}).second) out.push_back(std::move(n));
    }
    return out;
}

namespace {

// Word-boundary containment test: game must appear in run at a position
// where the char before (if any) and the char after (if any) are NOT
// alphanumeric. Keeps "JAMES POND!!!/" matching "JAMES POND" while
// rejecting "WORKER" matching "ORK".
bool containsAsWord(const std::string& run, const std::string& game) {
    std::size_t pos = 0;
    while ((pos = run.find(game, pos)) != std::string::npos) {
        const bool left_ok  = (pos == 0) ||
            !std::isalnum(static_cast<unsigned char>(run[pos - 1]));
        const std::size_t end = pos + game.size();
        const bool right_ok = (end >= run.size()) ||
            !std::isalnum(static_cast<unsigned char>(run[end]));
        if (left_ok && right_ok) return true;
        ++pos;
    }
    return false;
}

} // namespace

std::vector<DetectedGame>
GameStringScanner::matchKnownGames(const std::vector<std::string>& runs,
                                   const std::unordered_set<std::string>& known_upper,
                                   const Config& cfg) {
    std::unordered_map<std::string, std::string> hits;   // GAME_UPPER → evidence_run

    for (const auto& run : runs) {
        for (const auto& game : known_upper) {
            if (game.size() < cfg.min_game_len) continue;
            if (!containsAsWord(run, game))      continue;
            auto it = hits.find(game);
            if (it == hits.end() || it->second.size() > run.size()) {
                // Prefer the shortest run containing the game — cleaner
                // evidence ("JAMES POND" vs a wall of text that happens
                // to include "JAMES POND").
                hits[game] = run;
            }
        }
    }

    std::vector<DetectedGame> out;
    out.reserve(hits.size());
    for (auto& [game, evidence] : hits) {
        DetectedGame d;
        d.name     = game;
        d.evidence = evidence;
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(),
              [](const DetectedGame& a, const DetectedGame& b){ return a.name < b.name; });
    return out;
}

std::vector<DetectedGame>
GameStringScanner::scan(const std::vector<uint8_t>& bytes,
                        const std::unordered_set<std::string>& known_upper) {
    const Config cfg = defaultConfig();
    return matchKnownGames(extractRuns(bytes, cfg), known_upper, cfg);
}

} // namespace manifest
