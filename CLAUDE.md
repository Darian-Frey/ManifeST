# CLAUDE.md — ManifeST

## Project Identity

**Project:** `ManifeST`
**Binary:** `manifest`
**Language:** C++20
**Build system:** CMake 3.20+
**Platform:** Linux-first (ThinkPad P15 Gen 2i), Windows planned
**GUI toolkit:** Qt 6 Widgets
**Author:** Shane Hartley (@Darian-Frey)
**Status:** Scaffolding phase — headers + stubs in place, no implementations yet

---

## Purpose

ManifeST is a batch disk image cataloguer for Atari ST software collections.
Point it at a folder of `.ST`, `.MSA`, and `.DIM` images, and it:

1. Recursively walks the directory tree
2. Mounts each image via the vendored Atari disk engine
3. Extracts metadata (volume label, OEM string, file listing, hashes)
4. Identifies the disk (title, publisher, year) using heuristics and optional TOSEC data
5. Persists everything to a local SQLite database (`manifest.db`)
6. Provides **both** a Qt Widgets GUI (searchable database table) and a readline CLI to browse and launch catalogued disks in Hatari

ManifeST is a **standalone tool**, unrelated at runtime to the original Atari disk engine application. A curated two-file subset of the engine is vendored under `third_party/atari-engine/`.

---

## Dependency: Vendored Atari Disk Engine

The engine is vendored — copied verbatim, unmodified — into
`third_party/atari-engine/`:

- `AtariDiskEngine.h`
- `AtariDiskEngine.cpp`

These two files are the entire engine subset ManifeST needs. They link against `Qt6::Core` only (QByteArray / QString / QStringList / QVector). No other engine headers or sources are pulled in.

The upstream engine source tree lives locally in `atari-disk-engine/` for reference and is **gitignored** — it is *not* part of ManifeST's build or release.

### Actual engine API (read these, don't guess)

Namespace `Atari`. Core class:

```cpp
class AtariDiskEngine {
public:
    bool                    loadImage(const QString& path);   // sniffs raw / MSA / STX
    void                    load(const std::vector<uint8_t>& data);
    bool                    isLoaded() const;
    bool                    isReadOnly() const;               // true after STX load

    BootSectorInfo          checkBootSector() const;          // OEM + checksum validity
    BootSectorBpb           getBpb() const;                   // geometry
    DiskStats               getDiskStats() const;             // note: .label is NOT populated
    std::vector<DirEntry>   readRootDirectory(std::vector<uint32_t>* offsets = nullptr) const;
    std::vector<DirEntry>   readSubDirectory(uint16_t cluster, std::vector<uint32_t>* offsets = nullptr) const;
    std::vector<uint8_t>    readFile(const DirEntry& entry) const;
    const std::vector<uint8_t>& getRawImageData() const;

    QString                 getGroupName() const;             // cracker/menu-disk group
    QStringList             getBootSectorStrings() const;     // printable runs
    bool                    isHighBitEncoded() const;         // D-Bug filenames
    bool                    isRawLoaderDisk() const;          // no FAT — boot-sector game
};
```

`DirEntry` has `name[8]`, `ext[3]`, `attr`, `getStartCluster()`, `getFileSize()`, `getFilename()`, `isDirectory()`. `BootSectorBpb` holds `oemName` (QString), `bytesPerSector`, `sectorsPerCluster`, `reservedSectors`, `fatCount`, `rootEntries`, `totalSectors`, `mediaDescriptor`, `sectorsPerFat`, `sectorsPerTrack`, `sides`, `hiddenSectors`.

**`DiskReader` is the only ManifeST translation unit that includes the engine header.** Qt types stop at that boundary — the rest of ManifeST speaks `std::string` / `std::vector<uint8_t>`.

### Known engine gap: volume label

`readRootDirectory()` actively skips entries with `attr & 0x08` (volume label), and `DiskStats::label` is never populated. `DiskReader` works around this by parsing the root-directory region of `getRawImageData()` itself — scan 32-byte dirents, keep the first one with `attr & 0x08`, strip trailing spaces, convert underscores to spaces. Do **not** modify the vendored engine file to fix this.

### Hatari launch — not in the engine

The engine has no Hatari-launch code. ManifeST implements this itself in `HatariLauncher` (`QProcess::startDetached("hatari", {imagePath})`, with a `$PATH` check and a clear error if missing).

---

## Repository Layout

```
ManifeST/
├── CLAUDE.md                       ← this file
├── CMakeLists.txt
├── README.md
├── .gitignore
├── atari-disk-engine/              ← upstream engine (gitignored, reference only)
├── data/
│   └── tosec_titles.json           ← optional TOSEC hash lookup (gitignored, optional)
├── include/
│   └── manifest/
│       ├── DiskRecord.hpp          ← plain data struct, no engine dependency
│       ├── DiskReader.hpp          ← thin adapter over the vendored engine
│       ├── MetadataExtractor.hpp
│       ├── Identifier.hpp          ← TOSEC parse + heuristics + hash lookup
│       ├── Database.hpp            ← SQLite RAII wrapper
│       ├── Scanner.hpp             ← directory walker; runs on a QThread in GUI mode
│       ├── HatariLauncher.hpp      ← QProcess::startDetached wrapper
│       └── gui/
│           ├── MainWindow.hpp
│           └── DiskTableModel.hpp
├── src/
│   ├── DiskReader.cpp              ← only TU that includes AtariDiskEngine.h
│   ├── MetadataExtractor.cpp
│   ├── Identifier.cpp
│   ├── Database.cpp
│   ├── Scanner.cpp
│   ├── HatariLauncher.cpp
│   ├── main.cpp                    ← routes GUI vs CLI
│   ├── gui/
│   │   ├── MainWindow.cpp
│   │   └── DiskTableModel.cpp
│   └── cli/
│       └── QueryCLI.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── test_metadata.cpp
│   ├── test_identifier.cpp
│   └── test_database.cpp
└── third_party/
    ├── atari-engine/               ← vendored subset (tracked)
    │   ├── AtariDiskEngine.h
    │   └── AtariDiskEngine.cpp
    └── sqlite3/                    ← drop sqlite3.c / sqlite3.h here (gitignored)
```

---

## Architecture

### Data Flow

```
Folder path
    │
    ▼
Scanner  ──  std::filesystem::recursive_directory_iterator
             filter: .ST / .MSA / .DIM extensions
    │
    ▼  per image
DiskReader  ──  constructs Atari::AtariDiskEngine, calls loadImage(),
                extracts BPB + root dir + file list + volume label
    │
    ▼
MetadataExtractor  ──  SHA1 of raw image bytes, SHA1 per file,
                       launcher heuristic flag
    │
    ▼
Identifier  ──  (1) TOSEC filename parse
                (2) volume label / OEM / launcher filename heuristics
                (3) SHA1 lookup in tosec_titles.json (if present)
    │
    ▼
DiskRecord (populated)
    │
    ▼
Database  ──  upsert keyed on image SHA1, path as secondary key
```

### Key Classes

**`DiskRecord`** — plain data struct, no methods, no Qt. Passed between pipeline stages. Fields map 1:1 to the `disks` table.

**`DiskReader`** — the single adapter over the vendored engine. Owns the `Atari::AtariDiskEngine` instance for one image, translates its types into `DiskRecord` fields and `FileRecord` vectors. Also implements the volume-label workaround (parse root-dir region of raw image). If the vendored engine is ever updated, only this class changes.

**`MetadataExtractor`** — stateless. Takes a `DiskRecord&` and `DiskReader&`, fills in SHA1 hashes and the `is_launcher` flag.

**`Identifier`** — stateless. Three-pass title identification (see below).

**`Database`** — RAII wrapper around `sqlite3*`. Prepared statements only, no string interpolation. Provides `upsertDisk()`, `upsertFiles()`, `queryByTitle()`, `queryByHash()`, and the query helpers the GUI table model needs.

**`Scanner`** — owns the walk. In CLI mode runs synchronously and logs to stdout. In GUI mode runs on a `QThread` and emits Qt signals (`progress(scanned, total, currentPath)`, `imageDone(DiskRecord)`, `finished(Summary)`) so the table updates live. Skips bad images with a WARN; does not abort.

**`HatariLauncher`** — static helper. `QProcess::startDetached("hatari", {imagePath})`. If `hatari` is not on `$PATH`, returns a `Result` with a descriptive error string. Used by both the GUI ("Launch in Hatari" button) and the CLI (`launch` command).

**`gui::MainWindow`** — `QMainWindow`. Owns the `DiskTableModel`, the filter proxy, the toolbar (Scan Folder / Rescan / search box), the detail dock, and the status-bar progress widget. Connects Scanner signals to model refresh and status updates.

**`gui::DiskTableModel`** — `QAbstractTableModel`. Read-only. Columns defined in an enum: `Id / Title / Publisher / Year / Format / VolumeLabel / Tags / Identified`. Wrapped in a `QSortFilterProxyModel` for live search + column sorting. The `Identified` column is toggleable from the View menu and shows a checkmark (or red ✕) based on whether `identified_title` is non-null.

**`cli::QueryCLI`** — readline loop (`manifest> `). Commands: `find / list / info / launch / tags / dupes / quit`. Shares `Database` + `HatariLauncher` with the GUI.

---

## SQLite Schema

Apply verbatim on first run. Use `user_version` pragma for migrations.

```sql
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
PRAGMA user_version = 1;

CREATE TABLE IF NOT EXISTS disks (
    id                 INTEGER PRIMARY KEY,
    path               TEXT UNIQUE NOT NULL,
    filename           TEXT NOT NULL,
    image_hash         TEXT NOT NULL,        -- SHA1 hex of raw image
    format             TEXT NOT NULL,        -- "ST", "MSA", "DIM", "STX"
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

CREATE INDEX IF NOT EXISTS idx_disks_hash    ON disks(image_hash);
CREATE INDEX IF NOT EXISTS idx_disks_title   ON disks(identified_title);
CREATE INDEX IF NOT EXISTS idx_files_hash    ON files(file_hash);
CREATE INDEX IF NOT EXISTS idx_files_disk    ON files(disk_id);
```

---

## Title Identification Strategy

Three passes; first non-empty `identified_title` wins.

### Pass 1 — TOSEC Filename Parse

`<Title> (<Year>)(<Publisher>)[flags].st`
Regex: `^(.+?)\s*\((\d{4})\)\(([^)]+)\)`
Extract Title, Year, Publisher. Tag `game` by default.

### Pass 2 — Heuristics

- Volume label ≥ 3 chars (strip trailing spaces, `_` → space)
- OEM name (BPB) as fallback if no volume label
- Root directory with a single `.PRG` / `.APP` / `.TOS` → that filename (sans extension) is the title candidate

### Pass 3 — Hash Lookup

Optional `data/tosec_titles.json`:

```json
{
  "sha1_hex_string": {
    "title": "Dungeon Master",
    "publisher": "FTL",
    "year": 1987,
    "tags": ["game"]
  }
}
```

Silently skipped if the file is missing.

---

## Multi-Disk Detection

Post-scan pass over the folder's disks. Two signals:

1. Volume label prefix match (`GAME_D1`, `GAME_D2` → same set)
2. Shared file SHA1 (loader/intro common between images)

Group into `disk_sets`. Tag each disk `multidisk-1of2`, `multidisk-2of2`, etc.

---

## GUI Layout (Qt Widgets)

```
┌─────────────────────────────────────────────────────────────────────┐
│  File   Scan   View   Help                                          │  menu bar
├─────────────────────────────────────────────────────────────────────┤
│ [ Scan Folder… ] [ Rescan ] 🔍 [search________________________] [x] │  toolbar
├─────────────────────────────────────────────────────────────────────┤
│ ID │ Title          │ Publisher │ Year │ Format │ Label │ Tags │ ✓? │  QTableView
│ 42 │ Dungeon Master │ FTL       │ 1987 │ ST     │ DM1   │ game │ ✓  │  + QSortFilterProxyModel
│ …  │                │           │      │        │       │      │    │
├─────────────────────────────────────────────────────────────────────┤
│ Selected: Dungeon Master    [ Launch in Hatari ] [ Show in Files ] │  detail
│ Path / SHA1 / OEM / Geometry / file listing                         │  dock
├─────────────────────────────────────────────────────────────────────┤
│ Scanning: 341 / 2048  /mnt/atari/games/d/dragons.st  [=======    ]  │  status bar
└─────────────────────────────────────────────────────────────────────┘
```

- Columns in `DiskTableModel::Column`: `Id, Title, Publisher, Year, Format, VolumeLabel, Tags, Identified`.
- **`Identified`** column is **toggleable** from the `View` menu (`View → Show Identified column`). Shows ✓ when `identified_title` is non-null, ✕ when null — makes failed identifications visible at a glance.
- Right-click row → Launch / Show Files / Copy path / Open containing folder / Remove from catalog.
- `Scanner` runs on a `QThread`; `MainWindow` is the slot-owner for its signals.
- No thumbnails / cover art. Table is the primary surface.

---

## CLI Usage

```sh
manifest                                         # launch GUI
manifest --gui                                   # launch GUI (explicit)

manifest scan ~/AtariCollection/ --db ~/manifest.db
manifest scan ~/AtariCollection/ --db ~/manifest.db --incremental

manifest query --db ~/manifest.db                # interactive readline shell
manifest query --db ~/manifest.db --find "Dungeon Master"

manifest launch 42 --db ~/manifest.db            # one-shot Hatari launch
```

Interactive shell prompt: `manifest> `.
Commands: `find <term>`, `list`, `info <id>`, `launch <id>`, `tags <tag>`, `dupes`, `quit`.

---

## Database Location

Default: `~/manifest.db`. Platform-agnostic on purpose — the project will be cross-compiled to Windows later, and `~/manifest.db` resolves sanely on both Linux (`$HOME`) and Windows (`%USERPROFILE%`). Override via `--db` on the CLI or `File → Open Database…` in the GUI.

---

## CMakeLists.txt Skeleton

See [CMakeLists.txt](CMakeLists.txt). Summary:

- `find_package(Qt6 REQUIRED COMPONENTS Core Widgets)`
- `AUTOMOC` enabled (needed for `QObject` subclasses in the GUI layer)
- `atari_engine` — static lib from the two vendored files, links `Qt6::Core`
- `sqlite3` — static lib from `third_party/sqlite3/sqlite3.c` (drop it in)
- `manifest_lib` — core library (DiskReader / MetadataExtractor / Identifier / Database / Scanner / HatariLauncher)
- `manifest_gui` — Qt Widgets frontend, links `Qt6::Widgets`
- `manifest_cli` — readline query shell
- `manifest` binary — links GUI + CLI; `main.cpp` routes between them

---

## Implementation Order

1. **`Database`** — schema bootstrap, `upsertDisk()`, `upsertFiles()`, `queryByTitle()`, `queryByHash()`. Test with hand-crafted `DiskRecord`. Zero engine / Qt-GUI dependency.
2. **`DiskReader`** — wire the vendored engine. Include `AtariDiskEngine.h` only here. Implement volume-label workaround. Verify against one known `.ST` image.
3. **`MetadataExtractor`** — OpenSSL SHA1 over raw image + each file, launcher heuristic.
4. **`Identifier`** — TOSEC regex pass, heuristics, optional JSON hash lookup.
5. **`Scanner`** — directory walk, pipeline wiring, synchronous first.
6. **`HatariLauncher`** — `QProcess::startDetached` + `$PATH` check.
7. **`gui::DiskTableModel`** and **`gui::MainWindow`** — table + filter proxy + detail dock + toolbar. Wire to `Database`.
8. **Scanner GUI integration** — move Scanner onto a QThread, wire `progress` / `imageDone` / `finished` signals to `MainWindow`.
9. **Multi-disk detection** — post-scan grouping pass.
10. **`cli::QueryCLI`** — readline shell with `find / list / info / launch / tags / dupes`.

Keep the build green at every step.

---

## Error Handling Policy

- Images that fail to open: log `WARN: could not open <path> — skipping` and continue. Never abort a scan.
- Images with no identifiable title: store with `identified_title = NULL`. Still catalogued by hash and file listing. GUI shows ✕ in the Identified column.
- SQLite errors: throw `std::runtime_error` with the SQLite error string. `Scanner` catches, logs, and continues.
- Filesystem errors: catch `std::filesystem::filesystem_error`, log, skip.
- Hatari not on `$PATH`: `HatariLauncher::Result{ launched: false, error: "hatari not found on $PATH" }`. GUI surfaces via `QMessageBox`; CLI prints to stderr.

---

## Constraints / Open Questions

- **MSA / STX decompression** — handled transparently by the vendored engine in `loadImage()`. `.DIM` is treated as raw passthrough if the magic doesn't match MSA or STX; confirm against real `.DIM` images during step 2.
- **Read-only images** — STX loads set `isReadOnly() == true`. ManifeST never writes, so this is informational only; record `format = "STX"`.
- **TOSEC JSON** — optional. ManifeST must start and scan correctly without it.
- **Windows build** — planned. Avoid POSIX-only APIs. `readline` is Linux/Mac; the Windows CLI will either use a minimal `std::getline` fallback or `wineditline`. Decide before the Windows port.

---

## Definition of Done (Phase 1)

- [ ] `manifest scan <folder>` populates `manifest.db` without crashing on bad images
- [ ] Volume label, OEM name, file listing, geometry, image SHA1 stored for every readable image
- [ ] TOSEC filename parsing correctly identifies 20 known images
- [ ] Duplicate images (same SHA1) detected and flagged
- [ ] Multi-disk sets grouped for at least one known 2-disk game
- [ ] `manifest launch <id>` invokes Hatari
- [ ] GUI opens, table populates, live search works, sort works, Identified column toggles
- [ ] "Launch in Hatari" button works from the GUI
- [ ] `manifest query --find <term>` returns correct results
