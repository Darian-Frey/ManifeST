#include "manifest/Database.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace manifest {

namespace {

constexpr int kSchemaVersion = 7;

// Full-text-search index applied at user_version 2. Trigram tokenizer gives
// substring matching, so `find ark` still hits "Arkanoid" the way the
// old LIKE '%ark%' query did.
constexpr const char* kFtsSchemaSql = R"SQL(
CREATE VIRTUAL TABLE IF NOT EXISTS disks_fts USING fts5(
    title, filename, volume_label, files,
    tokenize = 'trigram'
);
)SQL";

// Applied on migration 1 → 2 to populate the new FTS index from existing rows.
constexpr const char* kFtsBackfillSql = R"SQL(
INSERT INTO disks_fts (rowid, title, filename, volume_label, files)
SELECT d.id,
       COALESCE(d.identified_title, ''),
       d.filename,
       COALESCE(d.volume_label, ''),
       COALESCE((SELECT GROUP_CONCAT(f.filename, ' ')
                 FROM files f WHERE f.disk_id = d.id), '')
FROM disks d;
)SQL";

// Applied on migration 2 → 3. Adds the menu-disk contents table used by
// MenuDiskCatalog to record the games sitting on a Medway / Pompey / D-Bug
// / Automation menu disk.
constexpr const char* kMenuContentsSql = R"SQL(
CREATE TABLE IF NOT EXISTS menu_contents (
    disk_id   INTEGER NOT NULL REFERENCES disks(id) ON DELETE CASCADE,
    position  INTEGER NOT NULL,
    game_name TEXT    NOT NULL,
    PRIMARY KEY (disk_id, position)
);
CREATE INDEX IF NOT EXISTS idx_menu_contents_game ON menu_contents(game_name);
)SQL";

// Applied on migration 3 → 4: games detected by byte-level string
// scanning against the known-games corpus. Separate from menu_contents
// because the source is empirical (fuzzy) rather than authoritative.
constexpr const char* kDetectedGamesSql = R"SQL(
CREATE TABLE IF NOT EXISTS detected_games (
    disk_id   INTEGER NOT NULL REFERENCES disks(id) ON DELETE CASCADE,
    game_name TEXT    NOT NULL,
    evidence  TEXT,
    PRIMARY KEY (disk_id, game_name)
);
CREATE INDEX IF NOT EXISTS idx_detected_games_name ON detected_games(game_name);
)SQL";

// Applied on migration 4 → 5: file stat (mtime + size) captured at scan
// time so incremental mode can skip unchanged images without re-hashing.
constexpr const char* kFileStatSql = R"SQL(
ALTER TABLE disks ADD COLUMN file_mtime INTEGER;
ALTER TABLE disks ADD COLUMN file_size  INTEGER;
)SQL";

// Applied on migration 5 → 6: scrolltext / boot-sector-strings fragments.
// Stored as rows rather than on the disks table so a single disk can
// carry many fragments without column bloat.
constexpr const char* kTextFragmentsSql = R"SQL(
CREATE TABLE IF NOT EXISTS text_fragments (
    disk_id  INTEGER NOT NULL REFERENCES disks(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    source   TEXT    NOT NULL,   -- "boot" | "deep"
    text     TEXT    NOT NULL,
    PRIMARY KEY (disk_id, position)
);
)SQL";

// Applied on migration 6 → 7: external-enrichment fields populated from
// ScreenScraper / similar third-party hash databases. All nullable — a
// disk can be catalogued without any of these.
constexpr const char* kEnrichmentSql = R"SQL(
ALTER TABLE disks ADD COLUMN genre            TEXT;
ALTER TABLE disks ADD COLUMN synopsis         TEXT;
ALTER TABLE disks ADD COLUMN developer        TEXT;
ALTER TABLE disks ADD COLUMN boxart_url       TEXT;
ALTER TABLE disks ADD COLUMN screenshot_url   TEXT;
ALTER TABLE disks ADD COLUMN screenscraper_id INTEGER;
)SQL";

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
    sqlite3_stmt* select_path_exists{nullptr};
    sqlite3_stmt* fts_delete{nullptr};
    sqlite3_stmt* fts_insert{nullptr};
    sqlite3_stmt* fts_update_files{nullptr};
    sqlite3_stmt* menu_delete{nullptr};
    sqlite3_stmt* menu_insert{nullptr};
    sqlite3_stmt* menu_select{nullptr};
    sqlite3_stmt* detected_delete{nullptr};
    sqlite3_stmt* detected_insert{nullptr};
    sqlite3_stmt* detected_select{nullptr};
    sqlite3_stmt* select_file_stat{nullptr};
    sqlite3_stmt* text_delete{nullptr};
    sqlite3_stmt* text_insert{nullptr};
    sqlite3_stmt* text_select{nullptr};

    void prepare(sqlite3_stmt*& out, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &out, nullptr) != SQLITE_OK) {
            throwSqlite(db, std::string("prepare: ") + sql);
        }
    }

    void finalizeAll() {
        for (sqlite3_stmt** s : {&upsert_disk, &delete_files, &insert_file,
                                 &delete_tags, &insert_tag, &select_by_hash,
                                 &select_files, &select_tags, &select_path_exists,
                                 &fts_delete, &fts_insert, &fts_update_files,
                                 &menu_delete, &menu_insert, &menu_select,
                                 &detected_delete, &detected_insert, &detected_select,
                                 &select_file_stat,
                                 &text_delete, &text_insert, &text_select}) {
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
    if (existing > kSchemaVersion) {
        throw std::runtime_error(
            "database has newer schema (user_version=" + std::to_string(existing) +
            ") than this build supports (" + std::to_string(kSchemaVersion) + ")");
    }

    // v0 → v1: initial schema.
    if (existing < 1) {
        exec(impl_->db, kSchemaSql);
    }
    // v1 → v2: FTS5 index + backfill.
    if (existing < 2) {
        exec(impl_->db, kFtsSchemaSql);
        if (existing >= 1) {
            exec(impl_->db, kFtsBackfillSql);   // pre-existing disks get indexed
        }
    }
    // v2 → v3: menu_contents table for cracker/menu disk game listings.
    if (existing < 3) {
        exec(impl_->db, kMenuContentsSql);
    }
    // v3 → v4: detected_games table populated by GameStringScanner.
    if (existing < 4) {
        exec(impl_->db, kDetectedGamesSql);
    }
    // v4 → v5: mtime + size for fast incremental skip.
    if (existing < 5) {
        exec(impl_->db, kFileStatSql);
    }
    // v5 → v6: scrolltext / boot-sector text fragments.
    if (existing < 6) {
        exec(impl_->db, kTextFragmentsSql);
    }
    // v6 → v7: external-enrichment columns for ScreenScraper-style data.
    if (existing < 7) {
        exec(impl_->db, kEnrichmentSql);
    }
    if (existing < kSchemaVersion) {
        writeUserVersion(impl_->db, kSchemaVersion);
    }

    impl_->prepare(impl_->upsert_disk, R"SQL(
        INSERT INTO disks (
            path, filename, image_hash, format, volume_label, oem_name,
            sides, tracks, sectors_per_track, bytes_per_sector,
            identified_title, publisher, year, notes,
            file_mtime, file_size,
            genre, synopsis, developer, boxart_url, screenshot_url, screenscraper_id
        ) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,
                  ?17,?18,?19,?20,?21,?22)
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
            -- notes: preserved across rescans. Users author these; the
            -- scanner never sets them, so we deliberately leave the stored
            -- value alone during ON CONFLICT updates.
            file_mtime        = excluded.file_mtime,
            file_size         = excluded.file_size,
            genre             = excluded.genre,
            synopsis          = excluded.synopsis,
            developer         = excluded.developer,
            boxart_url        = excluded.boxart_url,
            screenshot_url    = excluded.screenshot_url,
            screenscraper_id  = excluded.screenscraper_id,
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
               identified_title, publisher, year, notes,
               genre, synopsis, developer, boxart_url, screenshot_url, screenscraper_id
        FROM disks WHERE image_hash = ?1 LIMIT 1;
    )SQL");

    impl_->prepare(impl_->select_files, R"SQL(
        SELECT filename, extension, size_bytes, start_cluster, file_hash, is_launcher
        FROM files WHERE disk_id = ?1 ORDER BY id;
    )SQL");

    impl_->prepare(impl_->select_tags,
                   "SELECT tag FROM tags WHERE disk_id = ?1 ORDER BY tag;");

    impl_->prepare(impl_->select_path_exists,
                   "SELECT 1 FROM disks WHERE path = ?1 LIMIT 1;");

    impl_->prepare(impl_->fts_delete, "DELETE FROM disks_fts WHERE rowid = ?1;");
    impl_->prepare(impl_->fts_insert, R"SQL(
        INSERT INTO disks_fts (rowid, title, filename, volume_label, files)
        VALUES (?1, ?2, ?3, ?4, '');
    )SQL");
    impl_->prepare(impl_->fts_update_files,
                   "UPDATE disks_fts SET files = ?1 WHERE rowid = ?2;");

    impl_->prepare(impl_->menu_delete, "DELETE FROM menu_contents WHERE disk_id = ?1;");
    impl_->prepare(impl_->menu_insert, R"SQL(
        INSERT INTO menu_contents (disk_id, position, game_name) VALUES (?1, ?2, ?3);
    )SQL");
    impl_->prepare(impl_->menu_select, R"SQL(
        SELECT position, game_name FROM menu_contents
        WHERE disk_id = ?1 ORDER BY position;
    )SQL");

    impl_->prepare(impl_->detected_delete,
                   "DELETE FROM detected_games WHERE disk_id = ?1;");
    impl_->prepare(impl_->detected_insert, R"SQL(
        INSERT INTO detected_games (disk_id, game_name, evidence) VALUES (?1, ?2, ?3);
    )SQL");
    impl_->prepare(impl_->detected_select, R"SQL(
        SELECT game_name, evidence FROM detected_games
        WHERE disk_id = ?1 ORDER BY game_name;
    )SQL");

    impl_->prepare(impl_->select_file_stat, R"SQL(
        SELECT file_mtime, file_size FROM disks WHERE path = ?1 LIMIT 1;
    )SQL");

    impl_->prepare(impl_->text_delete,
                   "DELETE FROM text_fragments WHERE disk_id = ?1;");
    impl_->prepare(impl_->text_insert, R"SQL(
        INSERT INTO text_fragments (disk_id, position, source, text)
        VALUES (?1, ?2, ?3, ?4);
    )SQL");
    impl_->prepare(impl_->text_select, R"SQL(
        SELECT source, text FROM text_fragments
        WHERE disk_id = ?1 ORDER BY position;
    )SQL");
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
    sqlite3_bind_int64(s, 15, record.file_mtime);
    sqlite3_bind_int64(s, 16, record.file_size);
    bindOptText(s, 17, record.genre);
    bindOptText(s, 18, record.synopsis);
    bindOptText(s, 19, record.developer);
    bindOptText(s, 20, record.boxart_url);
    bindOptText(s, 21, record.screenshot_url);
    bindOptInt (s, 22, record.screenscraper_id);

    if (sqlite3_step(s) != SQLITE_ROW) {
        throwSqlite(impl_->db, "upsertDisk step");
    }
    record.id = sqlite3_column_int64(s, 0);
    sqlite3_reset(s);

    // Sync FTS row — delete + insert; files column is populated later in
    // upsertFiles(). Virtual tables don't support ON CONFLICT.
    sqlite3_reset(impl_->fts_delete);
    sqlite3_bind_int64(impl_->fts_delete, 1, record.id);
    if (sqlite3_step(impl_->fts_delete) != SQLITE_DONE) {
        throwSqlite(impl_->db, "fts_delete step");
    }

    sqlite3_reset(impl_->fts_insert);
    sqlite3_clear_bindings(impl_->fts_insert);
    sqlite3_bind_int64(impl_->fts_insert, 1, record.id);
    bindText(impl_->fts_insert, 2, record.identified_title.value_or(""));
    bindText(impl_->fts_insert, 3, record.filename);
    bindText(impl_->fts_insert, 4, record.volume_label);
    if (sqlite3_step(impl_->fts_insert) != SQLITE_DONE) {
        throwSqlite(impl_->db, "fts_insert step");
    }

    tx.commit();
}

void Database::upsertFiles(const DiskRecord& record) {
    Transaction tx(*this);

    sqlite3_reset(impl_->delete_files);
    sqlite3_bind_int64(impl_->delete_files, 1, record.id);
    if (sqlite3_step(impl_->delete_files) != SQLITE_DONE) {
        throwSqlite(impl_->db, "delete files");
    }

    std::string fts_files;
    fts_files.reserve(record.files.size() * 16
                    + record.menu_games.size() * 16
                    + record.detected_games.size() * 16);
    // Menu-disk game titles are indexed alongside raw filenames so
    // `find Populous` hits any menu disk known to contain Populous
    // (via either the curated catalog OR the byte-level string scan).
    for (const auto& g : record.menu_games) {
        if (!fts_files.empty()) fts_files += ' ';
        fts_files += g.name;
    }
    for (const auto& g : record.detected_games) {
        if (!fts_files.empty()) fts_files += ' ';
        fts_files += g.name;
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

        if (!fts_files.empty()) fts_files += ' ';
        fts_files += f.filename;
    }

    // Patch the `files` column of the FTS row with the aggregated names.
    sqlite3_reset(impl_->fts_update_files);
    sqlite3_clear_bindings(impl_->fts_update_files);
    bindText(impl_->fts_update_files, 1, fts_files);
    sqlite3_bind_int64(impl_->fts_update_files, 2, record.id);
    if (sqlite3_step(impl_->fts_update_files) != SQLITE_DONE) {
        throwSqlite(impl_->db, "fts_update_files step");
    }

    tx.commit();
}

void Database::upsertMenuContents(const DiskRecord& record) {
    Transaction tx(*this);

    sqlite3_reset(impl_->menu_delete);
    sqlite3_bind_int64(impl_->menu_delete, 1, record.id);
    if (sqlite3_step(impl_->menu_delete) != SQLITE_DONE) {
        throwSqlite(impl_->db, "delete menu_contents");
    }

    for (const auto& g : record.menu_games) {
        auto* s = impl_->menu_insert;
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        sqlite3_bind_int64(s, 1, record.id);
        sqlite3_bind_int  (s, 2, g.position);
        bindText          (s, 3, g.name);
        if (sqlite3_step(s) != SQLITE_DONE) {
            throwSqlite(impl_->db, "insert menu_contents");
        }
    }
    tx.commit();
}

void Database::upsertTextFragments(const DiskRecord& record) {
    Transaction tx(*this);

    sqlite3_reset(impl_->text_delete);
    sqlite3_bind_int64(impl_->text_delete, 1, record.id);
    if (sqlite3_step(impl_->text_delete) != SQLITE_DONE) {
        throwSqlite(impl_->db, "delete text_fragments");
    }

    for (std::size_t i = 0; i < record.text_fragments.size(); ++i) {
        auto& f = record.text_fragments[i];
        auto* s = impl_->text_insert;
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        sqlite3_bind_int64(s, 1, record.id);
        sqlite3_bind_int  (s, 2, static_cast<int>(i));
        bindText          (s, 3, f.source);
        bindText          (s, 4, f.text);
        if (sqlite3_step(s) != SQLITE_DONE) {
            throwSqlite(impl_->db, "insert text_fragment");
        }
    }
    tx.commit();
}

void Database::upsertDetectedGames(const DiskRecord& record) {
    Transaction tx(*this);

    sqlite3_reset(impl_->detected_delete);
    sqlite3_bind_int64(impl_->detected_delete, 1, record.id);
    if (sqlite3_step(impl_->detected_delete) != SQLITE_DONE) {
        throwSqlite(impl_->db, "delete detected_games");
    }

    for (const auto& d : record.detected_games) {
        auto* s = impl_->detected_insert;
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        sqlite3_bind_int64(s, 1, record.id);
        bindText          (s, 2, d.name);
        bindTextOrNull    (s, 3, d.evidence);
        if (sqlite3_step(s) != SQLITE_DONE) {
            throwSqlite(impl_->db, "insert detected_game");
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

void loadMenuGames(sqlite3_stmt* stmt, DiskRecord& rec) {
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, rec.id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MenuGame g;
        g.position = sqlite3_column_int(stmt, 0);
        g.name     = colText(stmt, 1);
        rec.menu_games.push_back(std::move(g));
    }
}

void loadDetectedGames(sqlite3_stmt* stmt, DiskRecord& rec) {
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, rec.id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DetectedGame d;
        d.name     = colText(stmt, 0);
        d.evidence = colText(stmt, 1);
        rec.detected_games.push_back(std::move(d));
    }
}

void loadTextFragments(sqlite3_stmt* stmt, DiskRecord& rec) {
    sqlite3_reset(stmt);
    sqlite3_bind_int64(stmt, 1, rec.id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DiskRecord::TextFragment f;
        f.source = colText(stmt, 0);
        f.text   = colText(stmt, 1);
        rec.text_fragments.push_back(std::move(f));
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
    r.genre             = colOptText(s, 15);
    r.synopsis          = colOptText(s, 16);
    r.developer         = colOptText(s, 17);
    r.boxart_url        = colOptText(s, 18);
    r.screenshot_url    = colOptText(s, 19);
    r.screenscraper_id  = colOptInt (s, 20);
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

    loadFiles        (impl_->select_files,     r);
    loadTags         (impl_->select_tags,      r);
    loadMenuGames    (impl_->menu_select,      r);
    loadDetectedGames(impl_->detected_select,  r);
    loadTextFragments(impl_->text_select,      r);
    return r;
}

std::optional<DiskRecord> Database::queryById(int64_t id) const {
    sqlite3_stmt* s = nullptr;
    const char* sql = R"SQL(
        SELECT id, path, filename, image_hash, format, volume_label, oem_name,
               sides, tracks, sectors_per_track, bytes_per_sector,
               identified_title, publisher, year, notes,
               genre, synopsis, developer, boxart_url, screenshot_url, screenscraper_id
        FROM disks WHERE id = ?1 LIMIT 1;
    )SQL";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &s, nullptr) != SQLITE_OK) {
        throwSqlite(impl_->db, "prepare queryById");
    }
    sqlite3_bind_int64(s, 1, id);

    std::optional<DiskRecord> out;
    if (sqlite3_step(s) == SQLITE_ROW) {
        out = readDiskRow(s);
    }
    sqlite3_finalize(s);

    if (out) {
        loadFiles        (impl_->select_files,    *out);
        loadTags         (impl_->select_tags,     *out);
        loadMenuGames    (impl_->menu_select,     *out);
        loadDetectedGames(impl_->detected_select, *out);
        loadTextFragments(impl_->text_select,     *out);
    }
    return out;
}

std::vector<DiskRecord> Database::listAll() const {
    sqlite3_stmt* s = nullptr;
    const char* sql = R"SQL(
        SELECT id, path, filename, image_hash, format, volume_label, oem_name,
               sides, tracks, sectors_per_track, bytes_per_sector,
               identified_title, publisher, year, notes,
               genre, synopsis, developer, boxart_url, screenshot_url, screenscraper_id
        FROM disks ORDER BY id;
    )SQL";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &s, nullptr) != SQLITE_OK) {
        throwSqlite(impl_->db, "prepare listAll");
    }
    std::vector<DiskRecord> out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        out.push_back(readDiskRow(s));
    }
    sqlite3_finalize(s);
    // Tags, menu games, and detected games are all shown in the GUI.
    for (auto& r : out) {
        loadTags         (impl_->select_tags,     r);
        loadMenuGames    (impl_->menu_select,     r);
        loadDetectedGames(impl_->detected_select, r);
    }
    return out;
}

void Database::removeDisk(int64_t id) {
    Transaction tx(*this);

    sqlite3_reset(impl_->fts_delete);
    sqlite3_bind_int64(impl_->fts_delete, 1, id);
    if (sqlite3_step(impl_->fts_delete) != SQLITE_DONE) {
        throwSqlite(impl_->db, "removeDisk fts_delete");
    }

    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(impl_->db, "DELETE FROM disks WHERE id = ?1;", -1,
                           &s, nullptr) != SQLITE_OK) {
        throwSqlite(impl_->db, "prepare removeDisk");
    }
    sqlite3_bind_int64(s, 1, id);
    if (sqlite3_step(s) != SQLITE_DONE) {
        sqlite3_finalize(s);
        throwSqlite(impl_->db, "removeDisk step");
    }
    sqlite3_finalize(s);
    tx.commit();
}

std::vector<DuplicateGroup> Database::listDuplicates() const {
    sqlite3_stmt* s = nullptr;
    const char* sql = R"SQL(
        SELECT id, path, filename, image_hash, format, volume_label, oem_name,
               sides, tracks, sectors_per_track, bytes_per_sector,
               identified_title, publisher, year, notes,
               genre, synopsis, developer, boxart_url, screenshot_url, screenscraper_id
        FROM disks
        WHERE image_hash IN (
            SELECT image_hash FROM disks GROUP BY image_hash HAVING COUNT(*) > 1
        )
        ORDER BY image_hash, filename;
    )SQL";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &s, nullptr) != SQLITE_OK) {
        throwSqlite(impl_->db, "prepare listDuplicates");
    }

    std::vector<DuplicateGroup> groups;
    while (sqlite3_step(s) == SQLITE_ROW) {
        DiskRecord r = readDiskRow(s);
        if (groups.empty() || groups.back().image_hash != r.image_hash) {
            groups.push_back({r.image_hash, {}});
        }
        groups.back().disks.push_back(std::move(r));
    }
    sqlite3_finalize(s);
    return groups;
}

std::vector<DiskSet> Database::listDiskSets() const {
    sqlite3_stmt* s = nullptr;
    const char* sql = R"SQL(
        SELECT s.set_id, s.disk_id, s.disk_num,
               COALESCE(d.identified_title, d.filename)
        FROM disk_sets s
        JOIN disks d ON d.id = s.disk_id
        ORDER BY s.set_id, s.disk_num;
    )SQL";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &s, nullptr) != SQLITE_OK) {
        throwSqlite(impl_->db, "prepare listDiskSets");
    }

    std::vector<DiskSet> out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const int64_t set_id  = sqlite3_column_int64(s, 0);
        const int64_t disk_id = sqlite3_column_int64(s, 1);
        const int     num     = sqlite3_column_int  (s, 2);
        const std::string title = colText(s, 3);

        if (out.empty() || out.back().set_id != set_id) {
            out.push_back({set_id, title, {}});
        }
        out.back().members.emplace_back(disk_id, num);
    }
    sqlite3_finalize(s);
    return out;
}

std::vector<TagCount> Database::listAllTags() const {
    sqlite3_stmt* s = nullptr;
    const char* sql = R"SQL(
        SELECT tag, COUNT(DISTINCT disk_id)
        FROM tags GROUP BY tag ORDER BY tag;
    )SQL";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &s, nullptr) != SQLITE_OK) {
        throwSqlite(impl_->db, "prepare listAllTags");
    }

    std::vector<TagCount> out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        TagCount tc;
        tc.tag   = colText(s, 0);
        tc.count = static_cast<std::size_t>(sqlite3_column_int64(s, 1));
        out.push_back(std::move(tc));
    }
    sqlite3_finalize(s);
    return out;
}

std::vector<int64_t> Database::idsWithTag(const std::string& tag) const {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
            "SELECT disk_id FROM tags WHERE tag = ?1;", -1, &s, nullptr) != SQLITE_OK) {
        throwSqlite(impl_->db, "prepare idsWithTag");
    }
    bindText(s, 1, tag);
    std::vector<int64_t> out;
    while (sqlite3_step(s) == SQLITE_ROW) {
        out.push_back(sqlite3_column_int64(s, 0));
    }
    sqlite3_finalize(s);
    return out;
}

void Database::rebuildDiskSets(const std::vector<DiskSet>& sets) {
    Transaction tx(*this);
    exec(impl_->db, "DELETE FROM disk_sets;");

    sqlite3_stmt* ins = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
            "INSERT INTO disk_sets (set_id, disk_id, disk_num) VALUES (?1, ?2, ?3);",
            -1, &ins, nullptr) != SQLITE_OK) {
        throwSqlite(impl_->db, "prepare rebuildDiskSets");
    }

    int64_t next_id = 1;
    for (const auto& set : sets) {
        if (set.members.size() < 2) continue;   // don't persist singletons
        for (const auto& [disk_id, num] : set.members) {
            sqlite3_reset(ins);
            sqlite3_clear_bindings(ins);
            sqlite3_bind_int64(ins, 1, next_id);
            sqlite3_bind_int64(ins, 2, disk_id);
            sqlite3_bind_int  (ins, 3, num);
            if (sqlite3_step(ins) != SQLITE_DONE) {
                sqlite3_finalize(ins);
                throwSqlite(impl_->db, "insert disk_set member");
            }
        }
        ++next_id;
    }
    sqlite3_finalize(ins);

    tx.commit();
}

void Database::setNotes(int64_t id, const std::string& notes) {
    Transaction tx(*this);
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(impl_->db,
            "UPDATE disks SET notes = ?1 WHERE id = ?2;", -1,
            &s, nullptr) != SQLITE_OK) {
        throwSqlite(impl_->db, "prepare setNotes");
    }
    if (notes.empty()) sqlite3_bind_null(s, 1);
    else               bindText(s, 1, notes);
    sqlite3_bind_int64(s, 2, id);
    if (sqlite3_step(s) != SQLITE_DONE) {
        sqlite3_finalize(s);
        throwSqlite(impl_->db, "setNotes step");
    }
    sqlite3_finalize(s);
    tx.commit();
}

void Database::checkpoint() {
    exec(impl_->db, "PRAGMA wal_checkpoint(TRUNCATE);");
}

bool Database::fileStatMatches(const std::string& path,
                               int64_t mtime, int64_t size) const {
    auto* s = impl_->select_file_stat;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    bindText(s, 1, path);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_reset(s);
        return false;
    }
    const int64_t stored_mtime = sqlite3_column_int64(s, 0);
    const int64_t stored_size  = sqlite3_column_int64(s, 1);
    sqlite3_reset(s);
    return stored_mtime == mtime && stored_size == size && stored_size > 0;
}

bool Database::pathExists(const std::string& path) const {
    auto* s = impl_->select_path_exists;
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    bindText(s, 1, path);
    const bool found = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_reset(s);
    return found;
}

std::vector<DiskRecord> Database::queryByTitle(const std::string& term) const {
    // Wrap the user's term in double quotes so FTS5 treats it as a literal
    // phrase — this keeps special characters (parentheses, colons, punctuation
    // that TOSEC filenames contain) from being interpreted as query operators.
    // Embedded double quotes get escaped by doubling, per FTS5 syntax rules.
    std::string fts_term = "\"";
    for (char c : term) {
        if (c == '"') fts_term += '"';
        fts_term += c;
    }
    fts_term += '"';

    sqlite3_stmt* s = nullptr;
    const char* sql = R"SQL(
        SELECT d.id, d.path, d.filename, d.image_hash, d.format,
               d.volume_label, d.oem_name, d.sides, d.tracks,
               d.sectors_per_track, d.bytes_per_sector,
               d.identified_title, d.publisher, d.year, d.notes,
               d.genre, d.synopsis, d.developer, d.boxart_url, d.screenshot_url, d.screenscraper_id
        FROM disks_fts f
        JOIN disks d ON d.id = f.rowid
        WHERE disks_fts MATCH ?1
        ORDER BY rank;
    )SQL";
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &s, nullptr) != SQLITE_OK) {
        throwSqlite(impl_->db, "prepare queryByTitle");
    }
    bindText(s, 1, fts_term);

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
