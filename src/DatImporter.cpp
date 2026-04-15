#include "manifest/DatImporter.hpp"

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
#include <string_view>
#include <unordered_map>

namespace manifest {

namespace {

// --- ClrMamePro lexer -----------------------------------------------------

struct Token {
    enum Kind { Word, String, LParen, RParen, End };
    Kind        kind{End};
    std::string text;
};

class Lexer {
public:
    explicit Lexer(std::string_view src) : src_(src) {}

    Token next() {
        skipWsAndComments();
        if (pos_ >= src_.size()) return {Token::End, {}};

        const char c = src_[pos_];
        if (c == '(') { ++pos_; return {Token::LParen, "("}; }
        if (c == ')') { ++pos_; return {Token::RParen, ")"}; }

        if (c == '"') return readString();
        return readWord();
    }

private:
    void skipWsAndComments() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(c))) { ++pos_; continue; }
            // Line comment: `#` or `//` — tolerant of either.
            if (c == '#' ||
                (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/')) {
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
    }

    Token readString() {
        ++pos_;                       // opening quote
        std::string out;
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') return {Token::String, std::move(out)};
            out += c;
        }
        throw std::runtime_error("unterminated string in DAT");
    }

    Token readWord() {
        const std::size_t start = pos_;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == '"') break;
            ++pos_;
        }
        return {Token::Word, std::string(src_.substr(start, pos_ - start))};
    }

    std::string_view src_;
    std::size_t      pos_{0};
};

// --- helpers --------------------------------------------------------------

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
    return s;
}

std::string stripExtension(const std::string& filename) {
    const auto dot = filename.find_last_of('.');
    return dot == std::string::npos ? filename : filename.substr(0, dot);
}

// Duplicated from Identifier.cpp — small enough that sharing via a helper
// module isn't worth the new file. Returns true + fills out-params on match.
bool parseTosec(const std::string& filename_no_ext,
                std::string& title, std::optional<int>& year,
                std::optional<std::string>& publisher) {
    static const std::regex re(R"(^(.+?)\s*\((\d{4}|19xx|20xx|\d{2}xx)\)\(([^)]+)\))",
                               std::regex::icase);
    std::smatch m;
    if (!std::regex_search(filename_no_ext, m, re)) return false;

    title = trim(m[1].str());
    const std::string y = m[2].str();
    if (!y.empty() && std::all_of(y.begin(), y.end(), [](char c){
            return std::isdigit(static_cast<unsigned char>(c)); })) {
        year = std::stoi(y);
    }
    publisher = trim(m[3].str());
    return true;
}

std::vector<std::string> parseFlagTags(const std::string& filename) {
    static const std::regex re(R"(\[([a-zA-Z]{1,3}\d*)\])");
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(filename.begin(), filename.end(), re);
         it != std::sregex_iterator(); ++it) {
        const std::string flag = (*it)[1].str();
        if (flag == "cr")        out.emplace_back("cracked");
        else if (flag == "t")    out.emplace_back("trained");
        else if (flag == "h")    out.emplace_back("hacked");
        else if (flag == "a")    out.emplace_back("alt");
        else if (flag == "!")    out.emplace_back("verified");
    }
    return out;
}

// Skip a `(...)` block, consuming tokens up to and including the matching
// close-paren. Caller has already consumed the opening `(`.
void skipBlock(Lexer& lx) {
    int depth = 1;
    while (depth > 0) {
        Token t = lx.next();
        if (t.kind == Token::End)    throw std::runtime_error("unexpected EOF inside block");
        if (t.kind == Token::LParen) ++depth;
        if (t.kind == Token::RParen) --depth;
    }
}

// Reads a `key value` pair where value is a word or quoted string.
// Returns false if the next token closes the current block.
bool readField(Lexer& lx, std::string& key, std::string& value) {
    Token k = lx.next();
    if (k.kind == Token::RParen) return false;
    if (k.kind != Token::Word) {
        throw std::runtime_error("expected field name, got '" + k.text + "'");
    }
    key = k.text;

    Token v = lx.next();
    if (v.kind == Token::LParen) {
        // Nested block (e.g. `rom ( ... )`); caller handles these explicitly.
        // Push a marker back by... actually we can't un-read. Signal via a
        // sentinel value.
        value = "(";
        return true;
    }
    if (v.kind != Token::Word && v.kind != Token::String) {
        throw std::runtime_error("expected value for '" + key + "'");
    }
    value = v.text;
    return true;
}

struct RomEntry {
    std::string name;
    std::string sha1;   // stored lowercase
};

void parseRomBlock(Lexer& lx, RomEntry& rom) {
    // Opening `(` already consumed.
    while (true) {
        std::string k, v;
        if (!readField(lx, k, v)) break;
        if (v == "(") { skipBlock(lx); continue; }
        if (k == "name") rom.name = v;
        else if (k == "sha1") rom.sha1 = lower(v);
    }
}

void parseGameBlock(Lexer& lx, std::vector<RomEntry>& roms) {
    while (true) {
        std::string k, v;
        if (!readField(lx, k, v)) break;
        if (v == "(") {
            if (k == "rom") {
                RomEntry rom;
                parseRomBlock(lx, rom);
                if (!rom.sha1.empty() && !rom.name.empty()) roms.push_back(std::move(rom));
            } else {
                skipBlock(lx);
            }
        }
        // other simple key/value pairs — ignore
    }
}

std::vector<RomEntry> parseDat(std::string_view src) {
    Lexer lx(src);
    std::vector<RomEntry> all;

    while (true) {
        Token t = lx.next();
        if (t.kind == Token::End) break;

        if (t.kind == Token::Word) {
            Token open = lx.next();
            if (open.kind != Token::LParen) {
                throw std::runtime_error("expected '(' after '" + t.text + "'");
            }
            if (t.text == "game" || t.text == "machine") {
                parseGameBlock(lx, all);
            } else {
                // clrmamepro / header / resource blocks — skip.
                skipBlock(lx);
            }
        } else {
            throw std::runtime_error("unexpected token '" + t.text + "' at top level");
        }
    }
    return all;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open DAT: " + path.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

QJsonObject loadExistingJson(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const auto bytes = f.readAll();
    f.close();

    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return doc.object();
}

void writeJson(const std::filesystem::path& path, const QJsonObject& obj) {
    std::filesystem::create_directories(path.parent_path());
    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("cannot write JSON: " + path.string());
    }
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    f.close();
}

} // namespace

DatImporter::Summary
DatImporter::importDat(const std::filesystem::path& dat_path,
                       const std::filesystem::path& json_path) {
    Summary s;
    const std::string src = readFile(dat_path);
    const auto roms = parseDat(src);

    QJsonObject merged = loadExistingJson(json_path);

    for (const auto& rom : roms) {
        ++s.rom_entries;

        std::string title;
        std::optional<int>         year;
        std::optional<std::string> publisher;
        const std::string name_no_ext = stripExtension(rom.name);
        if (!parseTosec(name_no_ext, title, year, publisher)) {
            ++s.skipped;
            continue;
        }

        QJsonObject entry;
        entry["title"] = QString::fromStdString(title);
        if (publisher) entry["publisher"] = QString::fromStdString(*publisher);
        if (year)      entry["year"]      = *year;

        QJsonArray tags;
        tags.append("game");
        for (const auto& t : parseFlagTags(rom.name)) {
            tags.append(QString::fromStdString(t));
        }
        entry["tags"] = tags;

        const QString key = QString::fromStdString(rom.sha1);
        if (merged.contains(key)) ++s.overwritten;
        merged[key] = entry;
        ++s.imported;
    }

    writeJson(json_path, merged);
    return s;
}

} // namespace manifest
