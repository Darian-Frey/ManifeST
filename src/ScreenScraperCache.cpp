#include "manifest/ScreenScraperCache.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <cctype>

namespace manifest {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<std::string> optStr(const QJsonObject& obj, const char* key) {
    if (!obj.contains(key)) return std::nullopt;
    const auto v = obj.value(key);
    if (!v.isString()) return std::nullopt;
    const auto s = v.toString();
    if (s.isEmpty()) return std::nullopt;
    return s.toStdString();
}

std::optional<int> optInt(const QJsonObject& obj, const char* key) {
    if (!obj.contains(key)) return std::nullopt;
    const auto v = obj.value(key);
    if (!v.isDouble()) return std::nullopt;
    return static_cast<int>(v.toInt());
}

} // namespace

ScreenScraperCache::ScreenScraperCache(const std::filesystem::path& json_path) {
    QFile f(QString::fromStdString(json_path.string()));
    if (!f.open(QIODevice::ReadOnly)) return;
    const auto bytes = f.readAll();
    f.close();

    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    const auto root  = doc.object();
    const auto games = root.value("games").toObject();
    if (games.isEmpty()) return;

    for (auto it = games.begin(); it != games.end(); ++it) {
        if (!it.value().isObject()) continue;
        const auto obj = it.value().toObject();
        Entry e;
        e.title            = optStr(obj, "title");
        e.publisher        = optStr(obj, "publisher");
        e.developer        = optStr(obj, "developer");
        e.year             = optInt(obj, "year");
        e.genre            = optStr(obj, "genre");
        e.synopsis         = optStr(obj, "synopsis");
        e.boxart_url       = optStr(obj, "boxart_url");
        e.screenshot_url   = optStr(obj, "screenshot_url");
        e.screenscraper_id = optInt(obj, "screenscraper_id");

        by_sha1_.emplace(lower(it.key().toStdString()), std::move(e));
    }
    loaded_ = !by_sha1_.empty();
}

std::size_t ScreenScraperCache::size() const {
    return by_sha1_.size();
}

bool ScreenScraperCache::enrich(DiskRecord& record) const {
    if (by_sha1_.empty() || record.image_hash.empty()) return false;

    auto it = by_sha1_.find(record.image_hash);
    if (it == by_sha1_.end()) return false;
    const auto& e = it->second;

    // Earlier-pass data wins: only fill fields that are currently empty.
    auto fillStr = [](std::optional<std::string>& target,
                      const std::optional<std::string>& src){
        if (!target.has_value() && src.has_value()) target = src;
    };
    auto fillInt = [](std::optional<int>& target,
                      const std::optional<int>& src){
        if (!target.has_value() && src.has_value()) target = src;
    };

    fillStr(record.identified_title, e.title);
    fillStr(record.publisher,        e.publisher);
    fillStr(record.developer,        e.developer);
    fillInt(record.year,             e.year);
    fillStr(record.genre,            e.genre);
    fillStr(record.synopsis,         e.synopsis);
    fillStr(record.boxart_url,       e.boxart_url);
    fillStr(record.screenshot_url,   e.screenshot_url);
    fillInt(record.screenscraper_id, e.screenscraper_id);

    return true;
}

} // namespace manifest
