#include "manifest/Exporter.hpp"

#include "manifest/Database.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>

namespace manifest {

namespace {

// RFC-4180: fields containing " , or newline get wrapped in double quotes
// with embedded quotes doubled. Apply this to every cell to be safe.
std::string csvCell(const std::string& s) {
    bool needs_quotes = s.find_first_of(",\"\r\n") != std::string::npos;
    if (!needs_quotes) return s;
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::string joinTags(const std::vector<std::string>& tags) {
    std::string out;
    for (const auto& t : tags) {
        if (!out.empty()) out += '|';
        out += t;
    }
    return out;
}

std::string joinMenu(const std::vector<MenuGame>& g) {
    std::string out;
    for (const auto& x : g) {
        if (!out.empty()) out += '|';
        out += x.name;
    }
    return out;
}

// --- CSV -----------------------------------------------------------------

void writeCsv(const Database& db, std::ostream& out) {
    out << "id,path,filename,image_hash,format,volume_label,oem_name,"
           "sides,tracks,sectors_per_track,bytes_per_sector,"
           "identified_title,publisher,year,tags,menu_games,notes\n";

    for (const auto& r : db.listAll()) {
        // listAll hydrates tags + menu_games; re-query by id for a full
        // hydrate if we ever need files here (we don't).
        out << r.id << ','
            << csvCell(r.path) << ','
            << csvCell(r.filename) << ','
            << csvCell(r.image_hash) << ','
            << csvCell(r.format) << ','
            << csvCell(r.volume_label) << ','
            << csvCell(r.oem_name) << ','
            << int(r.sides) << ','
            << r.tracks << ','
            << r.sectors_per_track << ','
            << r.bytes_per_sector << ','
            << csvCell(r.identified_title.value_or("")) << ','
            << csvCell(r.publisher.value_or("")) << ','
            << (r.year ? std::to_string(*r.year) : "") << ','
            << csvCell(joinTags(r.tags)) << ','
            << csvCell(joinMenu(r.menu_games)) << ','
            << csvCell(r.notes.value_or("")) << '\n';
    }
}

// --- JSON ----------------------------------------------------------------

QJsonObject recordToJson(const DiskRecord& r) {
    QJsonObject o;
    o["id"]                = double(r.id);
    o["path"]              = QString::fromStdString(r.path);
    o["filename"]          = QString::fromStdString(r.filename);
    o["image_hash"]        = QString::fromStdString(r.image_hash);
    o["format"]            = QString::fromStdString(r.format);
    o["volume_label"]      = QString::fromStdString(r.volume_label);
    o["oem_name"]          = QString::fromStdString(r.oem_name);
    o["sides"]             = int(r.sides);
    o["tracks"]            = r.tracks;
    o["sectors_per_track"] = r.sectors_per_track;
    o["bytes_per_sector"]  = r.bytes_per_sector;
    if (r.identified_title) o["identified_title"] = QString::fromStdString(*r.identified_title);
    if (r.publisher)        o["publisher"]        = QString::fromStdString(*r.publisher);
    if (r.year)             o["year"]             = *r.year;
    if (r.notes)            o["notes"]            = QString::fromStdString(*r.notes);

    QJsonArray tags;
    for (const auto& t : r.tags) tags.append(QString::fromStdString(t));
    o["tags"] = tags;

    QJsonArray files;
    for (const auto& f : r.files) {
        QJsonObject fo;
        fo["filename"]    = QString::fromStdString(f.filename);
        fo["extension"]   = QString::fromStdString(f.extension);
        fo["size_bytes"]  = double(f.size_bytes);
        fo["file_hash"]   = QString::fromStdString(f.file_hash);
        fo["is_launcher"] = f.is_launcher;
        files.append(fo);
    }
    o["files"] = files;

    QJsonArray menu;
    for (const auto& g : r.menu_games) menu.append(QString::fromStdString(g.name));
    o["menu_games"] = menu;

    QJsonArray detected;
    for (const auto& g : r.detected_games) detected.append(QString::fromStdString(g.name));
    o["detected_games"] = detected;

    return o;
}

void writeJson(const Database& db, std::ostream& out) {
    QJsonArray arr;
    // listAll doesn't hydrate files or detected_games — do per-id lookups
    // so the JSON export is complete.
    for (const auto& lite : db.listAll()) {
        auto full = db.queryById(lite.id);
        if (full) arr.append(recordToJson(*full));
    }
    QJsonDocument doc(arr);
    const auto bytes = doc.toJson(QJsonDocument::Indented);
    out.write(bytes.constData(), bytes.size());
}

// --- M3U -----------------------------------------------------------------

void writeM3u(const Database& db, std::ostream& out) {
    out << "#EXTM3U\n";

    const auto all  = db.listAll();
    const auto sets = db.listDiskSets();

    // Build disk_id → set membership for fast lookup.
    std::unordered_map<int64_t, std::pair<int64_t /*set_id*/, int /*disk_num*/>> in_set;
    for (const auto& s : sets) {
        for (const auto& [disk_id, num] : s.members) {
            in_set[disk_id] = {s.set_id, num};
        }
    }

    // Emit grouped multi-disk sets first (in set order, disk_num order).
    for (const auto& s : sets) {
        out << "\n# --- " << s.title << " (" << s.members.size() << " disks) ---\n";
        for (const auto& [disk_id, num] : s.members) {
            auto rec = db.queryById(disk_id);
            if (!rec) continue;
            out << "#EXTINF:0," << s.title << " · Disk " << num << "\n";
            out << rec->path << '\n';
        }
    }

    // Then everything else (standalone).
    bool header_emitted = false;
    for (const auto& r : all) {
        if (in_set.count(r.id)) continue;
        if (!header_emitted) {
            out << "\n# --- Standalone disks ---\n";
            header_emitted = true;
        }
        const std::string title = r.identified_title.value_or(r.filename);
        out << "#EXTINF:0," << title << "\n";
        out << r.path << '\n';
    }
}

} // namespace

void Exporter::exportTo(const Database& db, Format fmt, std::ostream& out) {
    switch (fmt) {
        case Format::Csv:  writeCsv (db, out); break;
        case Format::Json: writeJson(db, out); break;
        case Format::M3u:  writeM3u (db, out); break;
    }
}

} // namespace manifest
