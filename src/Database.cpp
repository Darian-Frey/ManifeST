#include "manifest/Database.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace manifest {

namespace {

constexpr int kSchemaVersion = 1;

constexpr const char* kSchemaSql = R"SQL(
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS disks (
    id                 INTEGER PRIMARY KEY,
    path               TEXT UNIQUE NOT NULL,
    filename           TEXT NOT NULL,
    image_hash         TEXT NOT NULL,
    format             TEXT NOT NULL,
    volume_label       TEXT,
    oem_name           TEXT,
    sides              INTEGER,
    tracks             INTEGER,
    sectors_per_track  INTEGER,
    bytes_per_sector   INTEGER,
    identified_title   TEXT,
    publisher          TEXT,
    year               INTEGER,
    notes              TEXT,
    scanned_at         DATETIME DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS files (
    id            INTEGER PRIMARY KEY,
    disk_id       INTEGER NOT NULL REFERENCES disks(id) ON DELETE CASCADE,
    filename      TEXT NOT NULL,
    extension     TEXT,
    size_bytes    INTEGER,
    start_cluster INTEGER,
    file_hash     TEXT,
    is_launcher   INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS tags (
    disk_id  INTEGER NOT NULL REFERENCES disks(id) ON DELETE CASCADE,
    tag      TEXT NOT NULL,
    PRIMARY KEY (disk_id, tag)
);

CREATE TABLE IF NOT EXISTS disk_sets (
    set_id   INTEGER NOT NULL,
    disk_id  INTEGER NOT NULL REFERENCES disks(id) ON DELETE CASCADE,
    disk_num INTEGER NOT NULL,
    PRIMARY KEY (set_id, disk_id)
);

CREATE INDEX IF NOT EXISTS idx_disks_hash  ON disks(image_hash);
CREATE INDEX IF NOT EXISTS idx_disks_title ON disks(identified_title);
CREATE INDEX IF NOT EXISTS idx_files_hash  ON files(file_hash);
CREATE INDEX IF NOT EXISTS idx_files_disk  ON files(disk_id);
)SQL";

[[noreturn]] void throwSqlite(sqlite3* db, const std::string& context) {
    const char* msg = db ? sqlite3_errmsg(db) : "sqlite3 is null";
    throw std::runtime_error(context + ": " + msg);
}

void exec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string message = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("sqlite3_exec failed: " + message);
    }
}

int readUserVersion(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) != SQLITE_OK) {
        throwSqlite(db, "prepare user_version");
    }
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

void writeUserVersion(sqlite3* db, int version) {
    const std::string sql = "PRAGMA user_version = " + std::to_string(version) + ";";
    exec(db, sql.c_str());
}

// --- bind helpers ----------------------------------------------------------

void bindText(sqlite3_stmt* s, int i, const std::string& v) {
    sqlite3_bind_text(s, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

void bindTextOrNull(sqlite3_stmt* s, int i, const std::string& v) {
    if (v.empty()) sqlite3_bind_null(s, i);
    else           bindText(s, i, v);
}

void bindOptText(sqlite3_stmt* s, int i, const std::optional<std::string>& v) {
    if (v) bindText(s, i, *v);
    else   sqlite3_bind_null(s, i);
}

void bindOptInt(sqlite3_stmt* s, int i, const std::optional<int>& v) {
    if (v) sqlite3_bind_int(s, i, *v);
    else   sqlite3_bind_null(s, i);
}

std::string colText(sqlite3_stmt* s, int i) {
    const unsigned char* txt = sqlite3_column_text(s, i);
    if (!txt) return {};
    const int bytes = sqlite3_column_bytes(s, i);
    return std::string(reinterpret_cast<const char*>(txt), static_cast<size_t>(bytes));
}

std::optional<std::string> colOptText(sqlite3_stmt* s, int i) {
    if (sqlite3_column_type(s, i) == SQLITE_NULL) return std::nullopt;
    return colText(s, i);
}

std::optional<int> colOptInt(sqlite3_stmt* s, int i) {
    if (sqlite3_column_type(s, i) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int(s, i);
}

} // namespace

// ---------------------------------------------------------------------------

struct Database::Impl {
    sqlite3* db{nullptr};
    int      tx_depth{0};

    sqlite3_stmt* upsert_disk{nullptr};
    sqlite3_stmt* delete_files{nullptr};
    sqlite3_stmt* insert_file{nullptr};
    sqlite3_stmt* delete_tags{nullptr};
    sqlite3_stmt* insert_tag{nullptr};
    sqlite3_stmt* select_by_hash{nullptr};
    sqlite3_stmt* select_files{nullptr};
    sqlite3_stmt* select_tags{nullptr};

    void prepare(sqlite3_stmt*& out, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &out, nullptr) != SQLITE_OK) {
            throwSqlite(db, std::string("prepare: ") + sql);
        }
    }

    void finalizeAll() {
        for (sqlite3_stmt** s : {&upsert_disk, &delete_files, &insert_file,
                                 &delete_tags, &insert_tag, &select_by_hash,
                                 &select_files, &select_tags}) {
            if (*s) { sqlite3_finalize(*s); *s = nullptr; }
        }
    }
};

Database::Database(const std::filesystem::path& db_path)
    : impl_(std::make_unique<Impl>()) {
    if (sqlite3_open(db_path.string().c_str(), &impl_->db) != SQLITE_OK) {
        const std::string msg = sqlite3_errmsg(impl_->db);
        sqlite3_close(impl_->db);
        impl_->db = nullptr;
        throw std::runtime_error("sqlite3_open failed: " + msg);
    }

    exec(impl_->db, "PRAGMA foreign_keys = ON;");

    const int existing = readUserVersion(impl_->db);
    if (existing < kSchemaVersion) {
        exec(impl_->db, kSchemaSql);
        writeUserVersion(impl_->db, kSchemaVersion);
    } else if (existing > kSchemaVersion) {
        throw std::runtime_error(
            "database has newer schema (user_version=" + std::to_string(existing) +
            ") than this build supports (" + std::to_string(kSchemaVersion) + ")");
    }

    impl_->prepare(impl_->upsert_disk, R"SQL(
        INSERT INTO disks (
            path, filename, image_hash, format, volume_label, oem_name,
            sides, tracks, sectors_per_track, bytes_per_sector,
            identified_title, publisher, year, notes
        ) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)
        ON CONFLICT(path) DO UPDATE SET
            filename          = excluded.filename,
            image_hash        = excluded.image_hash,
            format            = excluded.format,
            volume_label      = excluded.volume_label,
            oem_name          = excluded.oem_name,
            sides             = excluded.sides,
            tracks            = excluded.tracks,
            sectors_per_track = excluded.sectors_per_track,
            bytes_per_sector  = excluded.bytes_per_sector,
            identified_title  = excluded.identified_title,
            publisher         = excluded.publisher,
            year              = excluded.year,
            notes             = excluded.notes,
            scanned_at        = datetime('now')
        RETURNING id;
    )SQL");

    impl_->prepare(impl_->delete_files, "DELETE FROM files WHERE disk_id = ?1;");
    impl_->prepare(impl_->insert_file, R"SQL(
        INSERT INTO files (disk_id, filename, extension, size_bytes,
                           start_cluster, file_hash, is_launcher)
        VALUES (?1,?2,?3,?4,?5,?6,?7);
    )SQL");

    impl_->prepare(impl_->delete_tags, "DELETE FROM tags WHERE disk_id = ?1;");
    impl_->prepare(impl_->insert_tag,
                   "INSERT OR IGNORE INTO tags (disk_id, tag) VALUES (?1, ?2);");

    impl_->prepare(impl_->select_by_hash, R"SQL(
        SELECT id, path, filename, image_hash, format, volume_label, oem_name,
               sides, tracks, sectors_per_track, bytes_per_sector,
               identified_title, publisher, year, notes
        FROM disks WHERE image_hash = ?1 LIMIT 1;
    )SQL");

    impl_->prepare(impl_->select_files, R"SQL(
        SELECT filename, extension, size_bytes, start_cluster, file_hash, is_launcher
        FROM files WHERE disk_id = ?1 ORDER BY id;
    )SQL");

    impl_->prepare(impl_->select_tags,
                   "SELECT tag FROM tags WHERE disk_id = ?1 ORDER BY tag;");
}

Database::~Database() {
    if (!impl_) return;
    impl_->finalizeAll();
    if (impl_->db) sqlite3_close(impl_->db);
}

// --- Transaction -----------------------------------------------------------

Database::Transaction::Transaction(Database& db) : db_(db) {
    if (db_.impl_->tx_depth == 0) {
        exec(db_.impl_->db, "BEGIN;");
    }
    ++db_.impl_->tx_depth;
}

Database::Transaction::~Transaction() {
    if (!done_) {
        try { commit(); }
        catch (...) { /* dtor must not throw; the partial tx is already rolled back by SQLite on close */ }
    }
}

void Database::Transaction::commit() {
    if (done_) return;
    --db_.impl_->tx_depth;
    if (db_.impl_->tx_depth == 0) {
        exec(db_.impl_->db, "COMMIT;");
    }
    done_ = true;
}

void Database::Transaction::rollback() {
    if (done_) return;
    --db_.impl_->tx_depth;
    if (db_.impl_->tx_depth == 0) {
        exec(db_.impl_->db, "ROLLBACK;");
    }
    done_ = true;
}

// --- Writes ----------------------------------------------------------------

void Database::upsertDisk(DiskRecord& record) {
    Transaction tx(*this);

    auto* s = impl_->upsert_disk;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);

    bindText      (s, 1,  record.path);
    bindText      (s, 2,  record.filename);
    bindText      (s, 3,  record.image_hash);
    bindText      (s, 4,  record.format);
    bindTextOrNull(s, 5,  record.volume_label);
    bindTextOrNull(s, 6,  record.oem_name);
    sqlite3_bind_int(s, 7,  record.sides);
    sqlite3_bind_int(s, 8,  record.tracks);
    sqlite3_bind_int(s, 9,  record.sectors_per_track);
    sqlite3_bind_int(s, 10, record.bytes_per_sector);
    bindOptText(s, 11, record.identified_title);
    bindOptText(s, 12, record.publisher);
    bindOptInt (s, 13, record.year);
    bindOptText(s, 14, record.notes);

    if (sqlite3_step(s) != SQLITE_ROW) {
        throwSqlite(impl_->db, "upsertDisk step");
    }
    record.id = sqlite3_column_int64(s, 0);
    sqlite3_reset(s);

    tx.commit();
}

void Database::upsertFiles(const DiskRecord& record) {
    Transaction tx(*this);

    sqlite3_reset(impl_->delete_files);
    sqlite3_bind_int64(impl_->delete_files, 1, record.id);
    if (sqlite3_step(impl_->delete_files) != SQLITE_DONE) {
        throwSqlite(impl_->db, "delete files");
    }

    for (const auto& f : record.files) {
        auto* s = impl_->insert_file;
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        sqlite3_bind_int64(s, 1, record.id);
        bindText(s, 2, f.filename);
        bindTextOrNull(s, 3, f.extension);
        sqlite3_bind_int  (s, 4, static_cast<int>(f.size_bytes));
        sqlite3_bind_int  (s, 5, f.start_cluster);
        bindTextOrNull(s, 6, f.file_hash);
        sqlite3_bind_int  (s, 7, f.is_launcher ? 1 : 0);
        if (sqlite3_step(s) != SQLITE_DONE) {
            throwSqlite(impl_->db, "insert file");
        }
    }

    tx.commit();
}

void Database::upsertTags(const DiskRecord& record) {
    Transaction tx(*this);

    sqlite3_reset(impl_->delete_tags);
    sqlite3_bind_int64(impl_->delete_tags, 1, record.id);
    if (sqlite3_step(impl_->delete_tags) != SQLITE_DONE) {
        throwSqlite(impl_->db, "delete tags");
    }

    for (const auto& tag : record.tags) {
        auto* s = impl_->insert_tag;
        sqlite3_reset(s);
        sqlite3_bind_int64(s, 1, record.id);
        bindText(s, 2, tag);
        if (sqlite3_step(s) != SQLITE_DONE) {
            throwSqlite(impl_->db, "insert tag");
        }
    }

    tx.commit();
}

// --- Reads -----------------------------------------------------------------

namespace {

void loadFiles(sqlite3_stmt* stmt, DiskRecord& rec) {
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, rec.id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        FileRecord f;
        f.filename      = colText(stmt, 0);
        f.extension     = colText(stmt, 1);
        f.size_bytes    = static_cast<uint32_t>(sqlite3_column_int(stmt, 2));
        f.start_cluster = static_cast<uint16_t>(sqlite3_column_int(stmt, 3));
        f.file_hash     = colText(stmt, 4);
        f.is_launcher   = sqlite3_column_int(stmt, 5) != 0;
        rec.files.push_back(std::move(f));
    }
}

void loadTags(sqlite3_stmt* stmt, DiskRecord& rec) {
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, rec.id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rec.tags.push_back(colText(stmt, 0));
    }
}

DiskRecord readDiskRow(sqlite3_stmt* s) {
    DiskRecord r;
    r.id                = sqlite3_column_int64(s, 0);
    r.path              = colText(s, 1);
    r.filename          = colText(s, 2);
    r.image_hash        = colText(s, 3);
    r.format            = colText(s, 4);
    r.volume_label      = colText(s, 5);
    r.oem_name          = colText(s, 6);
    r.sides             = static_cast<uint8_t >(sqlite3_column_int(s, 7));
    r.tracks            = static_cast<uint16_t>(sqlite3_column_int(s, 8));
    r.sectors_per_track = static_cast<uint16_t>(sqlite3_column_int(s, 9));
    r.bytes_per_sector  = static_cast<uint16_t>(sqlite3_column_int(s, 10));
    r.identified_title  = colOptText(s, 11);
    r.publisher         = colOptText(s, 12);
    r.year              = colOptInt (s, 13);
    r.notes             = colOptText(s, 14);
    return r;
}

} // namespace

std::optional<DiskRecord> Database::queryByHash(const std::string& image_hash) const {
    auto* s = impl_->select_by_hash;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    bindText(s, 1, image_hash);

    if (sqlite3_step(s) != SQLITE_ROW) return std::nullopt;

    DiskRecord r = readDiskRow(s);
    sqlite3_reset(s);

    loadFiles(impl_->select_files, r);
    loadTags (impl_->select_tags,  r);
    return r;
}

std::vector<DiskRecord> Database::queryByTitle(const std::string& term) const {
    const std::string like = "%" + term + "%";

    sqlite3_stmt* s = nullptr;
    const char* sql = R"SQL(
        SELECT DISTINCT d.id, d.path, d.filename, d.image_hash, d.format,
               d.volume_label, d.oem_name, d.sides, d.tracks,
               d.sectors_per_track, d.bytes_per_sector,
               d.identified_title, d.publisher, d.year, d.notes
        FROM disks d
        LEFT JOIN files f ON f.disk_id = d.id
        WHERE d.identified_title LIKE ?1
           OR d.volume_label     LIKE ?1
           OR d.filename         LIKE ?1
           OR f.filename         LIKE ?1
        ORDER BY d.identified_title, d.filename;
    )SQL";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &s, nullptr) != SQLITE_OK) {
        throwSqlite(impl_->db, "prepare queryByTitle");
    }
    bindText(s, 1, like);

    std::vector<DiskRecord> out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        out.push_back(readDiskRow(s));
    }
    sqlite3_finalize(s);

    for (auto& r : out) {
        loadFiles(impl_->select_files, r);
        loadTags (impl_->select_tags,  r);
    }
    return out;
}

} // namespace manifest
