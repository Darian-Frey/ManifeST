#include "manifest/MenuDiskCatalog.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cctype>
#include <memory>
#include <regex>
#include <unordered_map>

namespace manifest {

namespace {

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Human cracker-group name (from engine / TOSEC publisher) → 8bitchip code.
// Only the groups that ship known menu-disk catalogues are mapped; others
// (Copylock, TRSI, etc.) produce singles, not numbered menu disks.
const std::unordered_map<std::string, std::string>& groupNameToCode() {
    static const std::unordered_map<std::string, std::string> m{
        {"MEDWAY BOYS",       "MB"},
        {"POMPEY PIRATES",    "PP"},
        {"D-BUG",             "DB"},
        {"DBUG",              "DB"},
        {"AUTOMATION",        "AU"},
        {"CYNIX",             "CY"},
        {"FUZION",            "FZ"},
        {"FLAME OF FINLAND",  "FF"},
        {"SUPERIOR",          "SU"},
        {"VECTRONIX",         "VE"},
        {"SPACED OUT",        "SO"},
        {"FOF",               "FF"},
        {"ELITE",             "EL"},   // not in 8bitchip — placeholder
        {"REPLICANTS",        "RP"},   // ditto
    };
    return m;
}

// Match the volume-label "PREFIX<ws?><digits>" pattern — Medway disks use
// "MEDWAY 98", Pompey tends to use "PP<num>", Automation "AUTO<num>", etc.
// Returns (prefix_upper, disk_number_string) on success.
std::optional<std::pair<std::string, std::string>>
splitLabelPrefixDigits(const std::string& label) {
    static const std::regex re(R"(^\s*([A-Za-z][A-Za-z .\-_]*?)[\s_-]*(\d{1,4})\s*$)");
    std::smatch m;
    if (!std::regex_match(label, m, re)) return std::nullopt;
    std::string prefix = m[1].str();
    // Trim trailing ws/punct from prefix.
    while (!prefix.empty() &&
           (std::isspace(static_cast<unsigned char>(prefix.back())) ||
            prefix.back() == '.' || prefix.back() == '-' || prefix.back() == '_')) {
        prefix.pop_back();
    }
    if (prefix.size() < 2) return std::nullopt;
    return std::make_pair(upper(std::move(prefix)), m[2].str());
}

// Label prefix (uppercased) → group code. Complements the group-name map
// for the common case where the engine didn't fire a cracker-group tag
// but the label alone names the group.
const std::unordered_map<std::string, std::string>& labelPrefixToCode() {
    static const std::unordered_map<std::string, std::string> m{
        {"MEDWAY",    "MB"},
        {"MB",        "MB"},
        {"POMPEY",    "PP"},
        {"PP",        "PP"},
        {"DBUG",      "DB"},
        {"D-BUG",     "DB"},
        {"DB",        "DB"},
        {"AUTO",      "AU"},
        {"AUTOMATION","AU"},
        {"AU",        "AU"},
        {"CYNIX",     "CY"},
        {"CY",        "CY"},
        {"FUZION",    "FZ"},
        {"FZ",        "FZ"},
        {"SUPERIOR",  "SU"},
        {"SU",        "SU"},
        {"VECTRONIX", "VE"},
        {"VE",        "VE"},
        {"SPACED",    "SO"},
        {"SO",        "SO"},
        {"FOF",       "FF"},
        {"FF",        "FF"},
    };
    return m;
}

// TOSEC-style filename: "<Group>... Menu Disk NNN (19xx)...".
// Returns (disk_number_string, "group_title") if matched; caller resolves
// the group code from the prefix words.
std::optional<std::pair<std::string, std::string>>
splitFilenameMenuDisk(const std::string& filename) {
    static const std::regex re(
        R"(^(.+?)\s+Menu\s+Disk\s+(\d{1,4})\b)",
        std::regex::icase);
    std::smatch m;
    if (!std::regex_search(filename, m, re)) return std::nullopt;
    return std::make_pair(m[2].str(), m[1].str());
}

// Normalize a disk-number string to a common form: strip leading zeros for
// lookup, but preserve the original as the display form. Callers look up
// using both.
std::string stripLeadingZeros(const std::string& s) {
    std::size_t i = 0;
    while (i + 1 < s.size() && s[i] == '0') ++i;
    return s.substr(i);
}

} // namespace

struct MenuDiskCatalog::Impl {
    // group_code → { disk_number_string → [games] }
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::vector<std::string>>> catalog;
    std::unordered_set<std::string> all_games_upper;  // lazy-initialized

    std::optional<std::vector<std::string>>
    lookup(const std::string& group_code, const std::string& disk_number) const {
        const auto git = catalog.find(group_code);
        if (git == catalog.end()) return std::nullopt;
        // Try exact match first, then stripped leading zeros, then zero-padded.
        if (auto it = git->second.find(disk_number); it != git->second.end()) {
            return it->second;
        }
        const std::string stripped = stripLeadingZeros(disk_number);
        if (stripped != disk_number) {
            if (auto it = git->second.find(stripped); it != git->second.end()) {
                return it->second;
            }
        }
        for (int pad : {2, 3, 4}) {
            if (static_cast<int>(stripped.size()) < pad) {
                std::string padded(pad - stripped.size(), '0');
                padded += stripped;
                if (auto it = git->second.find(padded); it != git->second.end()) {
                    return it->second;
                }
            }
        }
        return std::nullopt;
    }
};

MenuDiskCatalog::MenuDiskCatalog(const std::filesystem::path& json_path)
    : impl_(std::make_shared<Impl>()) {
    QFile f(QString::fromStdString(json_path.string()));
    if (!f.open(QIODevice::ReadOnly)) return;
    const auto bytes = f.readAll();
    f.close();

    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    const auto root = doc.object();
    for (auto git = root.begin(); git != root.end(); ++git) {
        if (!git.value().isObject()) continue;
        const std::string group_code = upper(git.key().toStdString());
        const auto disks = git.value().toObject();
        for (auto dit = disks.begin(); dit != disks.end(); ++dit) {
            if (!dit.value().isArray()) continue;
            std::vector<std::string> games;
            for (const auto& v : dit.value().toArray()) {
                if (v.isString()) games.push_back(v.toString().toStdString());
            }
            if (!games.empty()) {
                impl_->catalog[group_code][dit.key().toStdString()] = std::move(games);
            }
        }
    }
    loaded_ = !impl_->catalog.empty();
}

std::optional<MenuDiskCatalog::Match>
MenuDiskCatalog::detectMenuDisk(const std::string& filename,
                                const std::string& volume_label,
                                const std::vector<std::string>& tags) {
    // (1) Filename: "Medway Boys Menu Disk 098 (...)".
    if (auto fn = splitFilenameMenuDisk(filename)) {
        const std::string group_title = upper(fn->second);
        for (const auto& [name, code] : groupNameToCode()) {
            if (group_title.find(name) != std::string::npos) {
                return Match{code, fn->first};
            }
        }
    }

    // (2) Volume label: "MEDWAY 98" / "MB98" / "POMPEY 22".
    if (auto vl = splitLabelPrefixDigits(volume_label)) {
        const auto& [prefix, num] = *vl;
        const auto& pm = labelPrefixToCode();
        if (auto it = pm.find(prefix); it != pm.end()) {
            return Match{it->second, num};
        }
    }

    // Strategy 3 (engine tag + any digit) was removed — it caused false
    // positives for disks that carry a cracker-group signature but aren't
    // numbered menu compilations (e.g. "Zero 5 Loader.st" tagged Medway
    // Boys would incorrectly pick up MB5's games from the digit alone).
    return std::nullopt;
}

const std::unordered_set<std::string>& MenuDiskCatalog::allKnownGamesUpper() const {
    static const std::unordered_set<std::string> empty;
    if (!impl_) return empty;
    if (impl_->all_games_upper.empty()) {
        for (const auto& [group, disks] : impl_->catalog) {
            for (const auto& [num, games] : disks) {
                for (const auto& g : games) impl_->all_games_upper.insert(upper(g));
            }
        }
    }
    return impl_->all_games_upper;
}

bool MenuDiskCatalog::enrich(DiskRecord& record) const {
    if (!impl_ || impl_->catalog.empty()) return false;

    auto match = detectMenuDisk(record.filename, record.volume_label, record.tags);
    if (!match) return false;

    auto games = impl_->lookup(match->group_code, match->disk_number);
    if (!games) return false;

    record.menu_games.clear();
    for (std::size_t i = 0; i < games->size(); ++i) {
        MenuGame g;
        g.name     = (*games)[i];
        g.position = static_cast<int>(i);
        record.menu_games.push_back(std::move(g));
    }
    return true;
}

} // namespace manifest
