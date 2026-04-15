# CLAUDE.md — ManifeST

## Project Identity

**Project:** `ManifeST`  
**Binary:** `manifest`  
**Language:** C++20  
**Build system:** CMake 3.20+  
**Platform:** Linux-first (ThinkPad P15 Gen 2i), Windows planned  
**Author:** Shane Hartley (@Darian-Frey)  
**Status:** Scaffolding phase — no source files exist yet

---

## Purpose

ManifeST is a batch disk image cataloguer for Atari ST software collections.  
Point it at a folder of `.ST`, `.MSA`, and `.DIM` images, and it:

1. Recursively walks the directory tree
2. Mounts each image via the Atari disk engine
3. Extracts metadata (volume label, OEM string, file listing, hashes)
4. Identifies the disk (title, publisher, year) using heuristics and optional TOSEC data
5. Persists everything to a local SQLite database (`manifest.db`)
6. Provides a query/launch CLI to find and boot any catalogued disk

This project is **intentionally separate** from the Atari disk engine. The engine is a single-image library. ManifeST is the batch orchestration and persistence layer on top of it.

---

## Dependency: Atari Disk Engine

The Atari disk engine is located at `atari_disk-engine/` within this repository. Claude Code may read headers and source files there directly to understand the API. Do not modify the engine — it is a standalone library consumed as a dependency.

The engine handles all low-level disk I/O:

- FAT12 parsing and cluster chain traversal
- Root directory and subdirectory enumeration
- Boot sector parsing (OEM name, geometry fields, media byte)
- Raw sector I/O for `.ST`, `.MSA`, `.DIM` formats

ManifeST consumes the engine's public API only. It never touches sectors directly.

**Assumed interface contract** — read `atari_disk-engine/include/` to confirm actual types and function signatures, then adapt `DiskReader` accordingly:

```cpp
// Mount an image from a file path
DiskImage openImage(const std::filesystem::path& path);

// Iterate root directory entries
std::vector<DirectoryEntry> listRootDirectory(const DiskImage&);

// Recurse into subdirectories
std::vector<DirectoryEntry> listDirectory(const DiskImage&, uint16_t startCluster);

// Read a file's raw bytes (for hashing)
std::vector<uint8_t> readFile(const DiskImage&, const DirectoryEntry&);

// Boot sector fields
struct BootSector {
    char     oemName[9];     // bytes 3–10, null-terminated
    uint16_t bytesPerSector;
    uint8_t  sectorsPerCluster;
    uint16_t reservedSectors;
    uint8_t  numFATs;
    uint16_t rootEntryCount;
    uint16_t totalSectors16;
    uint8_t  mediaType;
    uint16_t sectorsPerFAT;
    uint16_t sectorsPerTrack;
    uint16_t numHeads;
};

BootSector readBootSector(const DiskImage&);
std::string readVolumeLabel(const DiskImage&);  // from root directory or boot sector
```

If the actual engine API differs from the above, adapt the `DiskReader` adapter class (see Architecture) before touching anything else. `DiskReader` is the single point of contact with the engine — if the API changes, only `DiskReader` changes.

---

## Repository Layout

```
ManifeST/
├── CLAUDE.md                  ← this file
├── CMakeLists.txt
├── README.md
├── atari-disk-engine/         ← Atari disk engine (read-only dependency)
│   ├── include/               ← read these to confirm API before implementing DiskReader
│   └── src/
├── data/
│   └── tosec_titles.json      ← optional TOSEC name lookup (offline)
├── include/
│   └── manifest/
│       ├── DiskRecord.hpp     ← plain data struct, no engine dependency
│       ├── DiskReader.hpp     ← thin adapter over the disk engine
│       ├── MetadataExtractor.hpp
│       ├── Identifier.hpp     ← heuristic + TOSEC title matching
│       ├── Database.hpp       ← SQLite RAII wrapper
│       ├── Scanner.hpp        ← directory walker / batch orchestrator
│       └── QueryCLI.hpp       ← readline-based query interface
├── src/
│   ├── DiskReader.cpp
│   ├── MetadataExtractor.cpp
│   ├── Identifier.cpp
│   ├── Database.cpp
│   ├── Scanner.cpp
│   ├── QueryCLI.cpp
│   └── main.cpp
├── tests/
│   ├── test_metadata.cpp
│   ├── test_identifier.cpp
│   └── test_database.cpp
└── third_party/
    └── sqlite3/               ← amalgamation (sqlite3.h + sqlite3.c)
```

---

## Architecture

### Data Flow

```
Folder path
    │
    ▼
Scanner
  └─ std::filesystem::recursive_directory_iterator
  └─ filter: .ST / .MSA / .DIM extensions
       │
       ▼ per image
  DiskReader (adapter)
  └─ calls disk engine: openImage(), readBootSector(), listRootDirectory(), etc.
       │
       ▼
  MetadataExtractor
  └─ collects: volume label, OEM name, file listing, geometry
  └─ computes: SHA1 of raw image bytes, SHA1 per file
       │
       ▼
  Identifier
  └─ TOSEC pass: parse image filename against TOSEC convention
  └─ heuristic pass: volume label, boot sector strings, launcher filename
  └─ hash pass: lookup image SHA1 in tosec_titles.json (if present)
       │
       ▼
  DiskRecord (populated struct)
       │
       ▼
  Database
  └─ upserts disk record (keyed on image SHA1, path as secondary key)
  └─ inserts file records
  └─ inserts tags
```

### Key Classes

**`DiskRecord`** — plain data struct, no methods. Passed between all pipeline stages. Fields map 1:1 to the `disks` table. Lives in `include/manifest/DiskRecord.hpp`.

**`DiskReader`** — thin adapter. Its only job is to translate the engine's types into `DiskRecord` fields and `FileRecord` vectors. Read `atari_disk-engine/include/` before implementing this class. If the engine API changes, only this class changes.

**`MetadataExtractor`** — stateless. Takes a mounted `DiskImage&` (via `DiskReader`) and returns a partially-filled `DiskRecord`. Does not identify the title — just raw facts.

**`Identifier`** — stateless. Takes a partially-filled `DiskRecord` and returns it with `identified_title`, `publisher`, `year`, and `tags` populated. Three passes: TOSEC filename parse → volume label heuristics → hash lookup. First pass that yields a result wins.

**`Database`** — RAII wrapper around `sqlite3*`. Owns the connection. Provides `upsertDisk()`, `upsertFiles()`, `queryByTitle()`, `queryByHash()`. Uses prepared statements throughout — no string interpolation into SQL.

**`Scanner`** — owns the walk. Constructs a pipeline per image: `DiskReader → MetadataExtractor → Identifier → Database`. Logs progress to stdout. Skips images that fail to open (logs error, continues). Reports summary on completion: `N images scanned, M new, K updated, J failed`.

**`QueryCLI`** — readline loop. Prompt: `manifest> `. Commands:
- `find <term>` — full-text search across title, volume label, filenames
- `list` — paginated dump of all catalogued disks
- `info <id>` — full record for a disk, including file listing
- `launch <id>` — exec Hatari with the image path
- `tags <tag>` — filter by tag (game, demo, utility, multidisk)
- `dupes` — list images with matching SHA1 (duplicates)
- `quit`

---

## SQLite Schema

Apply this verbatim on first run. Use `user_version` pragma for migrations.

```sql
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
PRAGMA user_version = 1;

CREATE TABLE IF NOT EXISTS disks (
    id                 INTEGER PRIMARY KEY,
    path               TEXT UNIQUE NOT NULL,
    filename           TEXT NOT NULL,
    image_hash         TEXT NOT NULL,        -- SHA1 hex of raw image
    format             TEXT NOT NULL,        -- "ST", "MSA", "DIM"
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
    file_hash     TEXT,                      -- SHA1 hex of file data
    is_launcher   INTEGER NOT NULL DEFAULT 0 -- 1 if heuristically identified as main executable
);

CREATE TABLE IF NOT EXISTS tags (
    disk_id  INTEGER NOT NULL REFERENCES disks(id) ON DELETE CASCADE,
    tag      TEXT NOT NULL,
    PRIMARY KEY (disk_id, tag)
);

CREATE TABLE IF NOT EXISTS disk_sets (
    set_id   INTEGER NOT NULL,
    disk_id  INTEGER NOT NULL REFERENCES disks(id) ON DELETE CASCADE,
    disk_num INTEGER NOT NULL,               -- 1-based index within set
    PRIMARY KEY (set_id, disk_id)
);

CREATE INDEX IF NOT EXISTS idx_disks_hash    ON disks(image_hash);
CREATE INDEX IF NOT EXISTS idx_disks_title   ON disks(identified_title);
CREATE INDEX IF NOT EXISTS idx_files_hash    ON files(file_hash);
CREATE INDEX IF NOT EXISTS idx_files_disk    ON files(disk_id);
```

---

## Title Identification Strategy

Three passes in order; first to yield a non-empty `identified_title` wins.

### Pass 1 — TOSEC Filename Parse

Many collections are TOSEC-named. Parse the image filename against:

```
<Title> (<Year>)(<Publisher>)[flags].st
```

Example: `Dungeon Master (1987)(FTL)[cr].st`  
Regex: `^(.+?)\s*\((\d{4})\)\(([^)]+)\)`

Extract `Title`, `Year`, `Publisher` directly. Set tag `game` (assume if no utility flag present).

### Pass 2 — Volume Label Heuristics

- Volume label present and ≥ 3 chars → use as `identified_title` candidate
- Strip trailing spaces, convert underscores to spaces
- OEM name (boot sector bytes 3–10) as fallback if volume label absent
- Root directory contains a single `.PRG`/`.APP`/`.TOS` → that filename (sans extension) becomes the title candidate

### Pass 3 — Hash Lookup

If `data/tosec_titles.json` is present, look up `image_hash` in it.

JSON format:
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

This file is optional. If absent, skip silently.

---

## Multi-Disk Detection

Run after all images in a folder are scanned. Two signals:

1. **Volume label prefix match** — `GAME_D1`, `GAME_D2` → same set  
2. **Common launcher file hash** — images sharing a file SHA1 (loader or intro) are likely from the same set

Group matched images into `disk_sets` records. Assign `set_id` as `MAX(set_id)+1`.

Tag each disk: `multidisk-1of2`, `multidisk-2of2`, etc.

---

## CMakeLists.txt Skeleton

```cmake
cmake_minimum_required(VERSION 3.20)
project(ManifeST LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Atari disk engine ---
# Read atari_disk-engine/CMakeLists.txt to confirm the exported target name
# and adjust target_link_libraries below if it differs from atari_disk_engine.
add_subdirectory(atari_disk-engine engine)

# --- SQLite amalgamation ---
add_library(sqlite3 STATIC third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite3)

# --- OpenSSL (for SHA1) ---
find_package(OpenSSL REQUIRED)

# --- ManifeST library ---
add_library(manifest_lib STATIC
    src/DiskReader.cpp
    src/MetadataExtractor.cpp
    src/Identifier.cpp
    src/Database.cpp
    src/Scanner.cpp
    src/QueryCLI.cpp
)
target_include_directories(manifest_lib PUBLIC include)
target_link_libraries(manifest_lib
    PRIVATE atari_disk_engine   # confirm target name from atari_disk-engine/CMakeLists.txt
    PRIVATE sqlite3
    PRIVATE OpenSSL::Crypto
    PRIVATE readline
)

# --- manifest binary ---
add_executable(manifest src/main.cpp)
target_link_libraries(manifest PRIVATE manifest_lib)

# --- Tests ---
enable_testing()
add_subdirectory(tests)
```

---

## CLI Usage (target UX)

```
# Scan a folder and populate / update the database
manifest scan ~/AtariCollection/ --db ~/manifest.db

# Re-scan only new images (skip known hashes)
manifest scan ~/AtariCollection/ --db ~/manifest.db --incremental

# Launch interactive query shell
manifest query --db ~/manifest.db

# One-shot query from shell
manifest query --db ~/manifest.db --find "Dungeon Master"

# Launch a found disk directly (calls hatari)
manifest launch 42 --db ~/manifest.db
```

The interactive shell prompt is `manifest> `.

---

## Implementation Order

Work in this sequence to keep the build green at every step:

1. **Read the engine first** — inspect `atari_disk-engine/include/` and confirm actual API types and function signatures before writing any ManifeST code.
2. **SQLite wrapper** (`Database.cpp`) — schema creation, `upsertDisk()`, basic query. No engine dependency yet. Test with a hand-crafted `DiskRecord`.
3. **DiskRecord / DiskReader** — wire the engine adapter using the confirmed API. Verify volume label and file list extraction against a single known image.
4. **MetadataExtractor** — SHA1 hashing via `openssl/sha.h`, geometry fields, file listing population.
5. **Identifier** — TOSEC filename parse first (easiest, highest yield). Heuristics second. Hash lookup last.
6. **Scanner** — directory walk, pipeline wiring, error handling, progress output.
7. **Multi-disk detection** — post-scan grouping pass.
8. **QueryCLI** — readline loop, `find`, `info`, `launch`. Add `list`, `tags`, `dupes` after basics work.

---

## Error Handling Policy

- Images that fail to open: log `WARN: could not open <path> — skipping` and continue. Do not abort the scan.
- Images with no identifiable title: store with `identified_title = NULL`. They are still catalogued by hash and file listing.
- SQLite errors: throw `std::runtime_error` with the SQLite error string. Let `Scanner` catch, log, and continue to next image.
- Filesystem permission errors: catch `std::filesystem::filesystem_error`, log, skip.

---

## Known Constraints / Open Questions

- **Engine target name** — read `atari_disk-engine/CMakeLists.txt` to confirm the CMake target name before building. Update `target_link_libraries` in ManifeST's `CMakeLists.txt` to match.
- **MSA decompression** — MSA images are compressed. Confirm whether the engine handles decompression transparently before `listRootDirectory()` is called, or add a decompression step in `DiskReader` if not.
- **DIM format** — verify which DIM variant(s) the engine supports (Pasti `.STX` is different from plain `.DIM`).
- **TOSEC JSON** — optional at runtime. ManifeST must start and run correctly without it.
- **Hatari path** — the `launch` command must check `$PATH` for `hatari` and emit a clear error if not found rather than silently failing.

---

## Definition of Done (Phase 1)

- [ ] `manifest scan <folder>` walks recursively and populates `manifest.db` without crashing on bad images
- [ ] Volume label, OEM name, file listing, geometry, and image SHA1 stored for every readable image
- [ ] TOSEC filename parsing correctly extracts title/year/publisher for a test set of 20 known images
- [ ] `manifest query --find <term>` returns correct results
- [ ] Duplicate images (same SHA1, different path) are detected and flagged
- [ ] Multi-disk sets for at least one known 2-disk game are correctly grouped
- [ ] `manifest launch <id>` invokes Hatari with the correct image path
- [ ] Interactive `manifest> ` shell accepts all documented commands
