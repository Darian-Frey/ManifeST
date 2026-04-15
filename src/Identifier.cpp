#include "manifest/Identifier.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <regex>
#include <unordered_map>
#include <utility>

namespace manifest {

namespace {

struct JsonEntry {
    std::string              title;
    std::optional<std::string> publisher;
    std::optional<int>       year;
    std::vector<std::string> tags;
};

std::string stripExtension(const std::string& filename) {
    const auto dot = filename.find_last_of('.');
    return dot == std::string::npos ? filename : filename.substr(0, dot);
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
    return s;
}

// Underscores → spaces, collapse whitespace.
std::string normalizeLabel(std::string s) {
    std::replace(s.begin(), s.end(), '_', ' ');
    std::string out;
    bool prev_space = false;
    for (char c : s) {
        if (c == ' ') {
            if (!prev_space && !out.empty()) out += ' ';
            prev_space = true;
        } else {
            out += c;
            prev_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// TOSEC filename: "<Title> (<Year>)(<Publisher>)[flags...](Disk N of M)(...)"
// Year may be literal digits or "19xx" / "20xx" — we only set year if numeric.
bool parseTosec(const std::string& filename_no_ext,
                std::string& title, std::optional<int>& year,
                std::optional<std::string>& publisher) {
    static const std::regex re(R"(^(.+?)\s*\((\d{4}|19xx|20xx|\d{2}xx)\)\(([^)]+)\))",
                               std::regex::icase);
    std::smatch m;
    if (!std::regex_search(filename_no_ext, m, re)) return false;

    title = trim(m[1].str());
    const std::string y = m[2].str();
    if (!y.empty() && std::all_of(y.begin(), y.end(), [](char c){ return std::isdigit(static_cast<unsigned char>(c)); })) {
        year = std::stoi(y);
    }
    publisher = trim(m[3].str());
    return true;
}

// Extract "Disk N of M" → "multidisk-NofM" tag if present.
std::optional<std::string> parseDiskSet(const std::string& filename) {
    static const std::regex re(R"(\(Disk\s+(\d+)\s+of\s+(\d+)\))", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(filename, m, re)) return std::nullopt;
    return "multidisk-" + m[1].str() + "of" + m[2].str();
}

// TOSEC square-bracket flags: [cr], [t], [h], [a], [!] etc.
void parseFlags(const std::string& filename, std::vector<std::string>& tags) {
    static const std::regex re(R"(\[([a-zA-Z]{1,3}\d*)\])");
    for (auto it = std::sregex_iterator(filename.begin(), filename.end(), re);
         it != std::sregex_iterator(); ++it) {
        const std::string flag = (*it)[1].str();
        if (flag == "cr")        tags.emplace_back("cracked");
        else if (flag == "t")    tags.emplace_back("trained");
        else if (flag == "h")    tags.emplace_back("hacked");
        else if (flag == "a")    tags.emplace_back("alt");
        else if (flag == "!")    tags.emplace_back("verified");
        // unknown flags are ignored — they're noisy without being wrong
    }
}

// Add `tag` to `tags` if not already present.
void addTag(std::vector<std::string>& tags, std::string tag) {
    if (tag.empty()) return;
    if (std::find(tags.begin(), tags.end(), tag) == tags.end()) {
        tags.push_back(std::move(tag));
    }
}

} // namespace

struct Identifier::Impl {
    std::unordered_map<std::string, JsonEntry> by_hash;

    void loadJson(const std::filesystem::path& path) {
        QFile f(QString::fromStdString(path.string()));
        if (!f.open(QIODevice::ReadOnly)) return;
        const auto bytes = f.readAll();
        f.close();

        QJsonParseError err{};
        const auto doc = QJsonDocument::fromJson(bytes, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

        const auto obj = doc.object();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!it.value().isObject()) continue;
            const auto v = it.value().toObject();
            JsonEntry entry;
            entry.title = v.value("title").toString().toStdString();
            if (entry.title.empty()) continue;
            if (v.contains("publisher") && v.value("publisher").isString()) {
                entry.publisher = v.value("publisher").toString().toStdString();
            }
            if (v.contains("year") && v.value("year").isDouble()) {
                entry.year = static_cast<int>(v.value("year").toInt());
            }
            if (v.contains("tags") && v.value("tags").isArray()) {
                for (const auto& t : v.value("tags").toArray()) {
                    if (t.isString()) entry.tags.push_back(t.toString().toStdString());
                }
            }
            by_hash.emplace(it.key().toStdString(), std::move(entry));
        }
    }
};

Identifier::Identifier(std::optional<std::filesystem::path> tosec_json)
    : impl_(std::make_unique<Impl>()) {
    if (tosec_json && std::filesystem::exists(*tosec_json)) {
        impl_->loadJson(*tosec_json);
    }
}

Identifier::~Identifier() = default;

void Identifier::identify(DiskRecord& record) const {
    // Multidisk / flag tags are orthogonal to the title source — always apply.
    if (auto md = parseDiskSet(record.filename)) addTag(record.tags, *md);
    parseFlags(record.filename, record.tags);

    // --- Pass 1 · SHA1 lookup (most authoritative) ---------------------
    // Hash match wins over everything else — it's the one data source that
    // survives filename changes, cracker rebrands, and bogus volume labels.
    if (!impl_->by_hash.empty() && !record.image_hash.empty()) {
        auto it = impl_->by_hash.find(record.image_hash);
        if (it != impl_->by_hash.end()) {
            record.identified_title = it->second.title;
            if (it->second.publisher) record.publisher = it->second.publisher;
            if (it->second.year)      record.year      = it->second.year;
            for (const auto& t : it->second.tags) addTag(record.tags, t);
            return;
        }
    }

    // --- Pass 2 · TOSEC filename ---------------------------------------
    std::string title;
    std::optional<int>         year;
    std::optional<std::string> publisher;
    if (parseTosec(stripExtension(record.filename), title, year, publisher)) {
        record.identified_title = title;
        if (year)      record.year      = year;
        if (publisher) record.publisher = publisher;
        bool is_util = std::find(record.tags.begin(), record.tags.end(), "utility")
                       != record.tags.end();
        if (!is_util) addTag(record.tags, "game");
        return;
    }

    // --- Pass 3 · Heuristics -------------------------------------------
    const std::string label = normalizeLabel(record.volume_label);
    if (label.size() >= 3) {
        record.identified_title = label;
        return;
    }

    const std::string oem = trim(record.oem_name);
    if (oem.size() >= 3) {
        const bool printable = std::all_of(oem.begin(), oem.end(),
            [](char c){ return c >= 32 && c < 127; });
        if (printable) {
            record.identified_title = oem;
            return;
        }
    }

    for (const auto& f : record.files) {
        if (f.is_launcher) {
            record.identified_title = stripExtension(f.filename);
            return;
        }
    }
}

} // namespace manifest
