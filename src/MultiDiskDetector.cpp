#include "manifest/MultiDiskDetector.hpp"

#include "manifest/Database.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <utility>

namespace manifest {

namespace {

std::string rtrim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::optional<MultiDiskDetector::PrefixSplit>
MultiDiskDetector::splitLabel(const std::string& label_in) {
    const std::string label = rtrim(label_in);
    if (label.size() < 2) return std::nullopt;

    // Find the trailing run of digits.
    std::size_t end = label.size();
    std::size_t digits_begin = end;
    while (digits_begin > 0 &&
           std::isdigit(static_cast<unsigned char>(label[digits_begin - 1]))) {
        --digits_begin;
    }
    if (digits_begin == end) return std::nullopt;         // no trailing digits

    const std::string digits = label.substr(digits_begin);
    if (digits.size() > 3) return std::nullopt;           // "MEDWAY 98" etc.

    const int num = std::stoi(digits);
    if (num < 1 || num > 99) return std::nullopt;

    // Prefix = everything before digits, trimmed. Require ≥2 chars.
    std::string prefix = rtrim(label.substr(0, digits_begin));
    if (prefix.size() < 2) return std::nullopt;
    prefix = upper(std::move(prefix));

    return PrefixSplit{std::move(prefix), num};
}

std::optional<MultiDiskDetector::FilenameSplit>
MultiDiskDetector::parseFilenameDiskNum(const std::string& filename) {
    static const std::regex re(R"(\(Disk\s+(\d+)\s+of\s+(\d+)\))", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(filename, m, re)) return std::nullopt;

    FilenameSplit out;
    out.disk_num    = std::stoi(m[1].str());
    out.total_disks = std::stoi(m[2].str());
    return out;
}

std::vector<DiskSet> MultiDiskDetector::detect(const Database& db) {
    const auto disks = db.listAll();

    // Group key used for both strategies: a canonical identifier string.
    // Map from key → (title_for_display, [(disk_id, disk_num), ...])
    struct Acc {
        std::string                                title;
        std::vector<std::pair<int64_t, int>>       members;
        std::set<int>                              seen_nums;  // dedupe by disk_num
    };
    std::map<std::string, Acc> by_key;
    std::set<int64_t> assigned;   // disks already placed by a strategy

    // --- Strategy 1: TOSEC title + filename "Disk N of M" ---
    for (const auto& d : disks) {
        if (!d.identified_title) continue;
        const auto split = parseFilenameDiskNum(d.filename);
        if (!split) continue;

        std::string key = "title:" + upper(*d.identified_title);
        if (d.publisher) key += "|" + upper(*d.publisher);
        if (d.year)      key += "|" + std::to_string(*d.year);

        auto& acc = by_key[key];
        if (acc.title.empty()) acc.title = *d.identified_title;
        if (acc.seen_nums.insert(split->disk_num).second) {
            acc.members.emplace_back(d.id, split->disk_num);
        }
        assigned.insert(d.id);
    }

    // --- Strategy 2: volume-label prefix (only for disks not already grouped) ---
    for (const auto& d : disks) {
        if (assigned.count(d.id)) continue;
        const auto split = splitLabel(d.volume_label);
        if (!split) continue;

        const std::string key = "label:" + split->prefix;
        auto& acc = by_key[key];
        if (acc.title.empty()) {
            acc.title = d.identified_title.value_or(split->prefix);
        }
        if (acc.seen_nums.insert(split->disk_num).second) {
            acc.members.emplace_back(d.id, split->disk_num);
        }
    }

    // Only groups with ≥2 members are real sets. For label-keyed groups we
    // also require disk numbers to form a plausible 1..N sequence: this
    // rejects false positives like "MEDWAY 98" and "MEDWAY 100" getting
    // glued together purely because they share a label prefix.
    std::vector<DiskSet> out;
    for (auto& [key, acc] : by_key) {
        if (acc.members.size() < 2) continue;
        std::sort(acc.members.begin(), acc.members.end(),
                  [](const auto& a, const auto& b){ return a.second < b.second; });

        if (key.rfind("label:", 0) == 0) {
            // Require 1..N with no gap, highest ≤ 9.
            bool ok = acc.members.front().second == 1
                   && acc.members.back().second == static_cast<int>(acc.members.size())
                   && acc.members.back().second <= 9;
            if (!ok) continue;
        }

        DiskSet s;
        s.title   = std::move(acc.title);
        s.members = std::move(acc.members);
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(),
              [](const DiskSet& a, const DiskSet& b){ return a.title < b.title; });
    return out;
}

void MultiDiskDetector::detectAndPersist(Database& db) {
    db.rebuildDiskSets(detect(db));
}

} // namespace manifest
