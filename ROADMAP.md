# ManifeST Roadmap

Single source of truth for "where we are / what's next". Update as work completes.
Status legend: ✅ done · 🔨 in progress · ⏳ todo · ⏸ blocked · ❓ needs decision

**Currently at:** Phase 1.2 ✅ complete → next is **Phase 1.3 · MetadataExtractor**.

---

## Phase 0 — Scaffolding & Decisions ✅

- [x] Repo skeleton (`include/manifest/`, `src/`, `tests/`, `third_party/`, `data/`)
- [x] Header stubs for all modules
- [x] `.gitignore` populated (build artefacts, DB files, upstream engine, amalgamation drops)
- [x] [README.md](README.md) landing page
- [x] Engine survey (identified the two-file vendor subset + gaps)
- [x] Decision: **Qt 6 Widgets** for GUI
- [x] Decision: **GUI + CLI** both shipped, shared core lib
- [x] Decision: vendor engine under `third_party/atari-engine/` (not `add_subdirectory`)
- [x] Decision: default DB at `~/manifest.db` (cross-platform)
- [x] Decision: no thumbnails
- [x] Decision: add toggleable `Identified?` column
- [x] Vendored [third_party/atari-engine/AtariDiskEngine.h](third_party/atari-engine/AtariDiskEngine.h) + `.cpp`
- [x] [CMakeLists.txt](CMakeLists.txt) rewritten for Qt6 + split libs
- [x] GUI / CLI layer directories + stubs created
- [x] [CLAUDE.md](CLAUDE.md) updated to reflect all decisions
- [x] This roadmap

---

## Phase 1 — Core Pipeline MVP (CLI) ⏳

Goal: `manifest scan <folder> --db <path>` walks a folder, catalogues images, writes SQLite.
No GUI, no multi-disk detection yet, no CLI query shell yet — just the pipeline end-to-end.

### 1.1 · `Database` ✅
- [x] Drop SQLite amalgamation into [third_party/sqlite3/](third_party/sqlite3/) (3.53.0, 2026-04-09)
- [x] RAII open/close of `sqlite3*` in `Database::Impl`
- [x] Schema bootstrap on first open (applies the `CREATE TABLE` block from [CLAUDE.md](CLAUDE.md#sqlite-schema))
- [x] `user_version` pragma check + "newer schema" rejection
- [x] Prepared statements: `upsertDisk`, `upsertFiles`, `upsertTags`, `queryByHash`, `queryByTitle`
- [x] RAII `Database::Transaction` scope (nest-safe via depth counter)
- [x] `std::runtime_error` with SQLite error string on failure
- [x] [tests/test_database.cpp](tests/test_database.cpp) — 4 cases: schema bootstrap, upsert round-trip, re-upsert preserves id + updates fields, query-by-title across title/label/filename, missing-hash returns nullopt
- [x] CMake: enabled C language, vendored-engine subdir layout restored, readline made optional

### 1.2 · `DiskReader` ✅
- [x] Confirm engine API surface against [third_party/atari-engine/include/AtariDiskEngine.h](third_party/atari-engine/include/AtariDiskEngine.h)
- [x] Construct `Atari::AtariDiskEngine`, call `loadImage()`, check `isLoaded()`
- [x] Map `BootSectorBpb` → geometry fields on `DiskRecord` (tracks derived from totalSectors / (spt × sides))
- [x] Walk root dir + subdirs recursively, build `std::vector<FileRecord>` (cycle guard via visited-cluster set)
- [x] Volume-label workaround: raw-byte scan of root-dir region for `attr & 0x08`, interior spaces collapsed
- [x] Format sniffing: magic bytes first (`0x0E 0x0F` → MSA, `RSY\0` → STX), then `.DIM` by extension, else `ST`
- [x] `manifest inspect <path>` diagnostic subcommand in [src/main.cpp](src/main.cpp)
- [x] Smoke-tested against 4 real disks: Arkanoid, Another World D1 (multi-disk), Medway Boys 098 (cracker-group), Atari Language 1040ST (360K single-side)
- [ ] Follow-up: engine's cracker-group detection returned empty for Medway Boys 098 — investigate in Phase 1.4 when wiring identification

### 1.3 · `MetadataExtractor` 🔨 **← next**
- [ ] SHA1 of raw image bytes via OpenSSL `EVP_Digest`
- [ ] SHA1 per file (use `DiskReader` to re-read bytes)
- [ ] `is_launcher` heuristic: single `.PRG` / `.APP` / `.TOS` in root → mark launcher
- [ ] [tests/test_metadata.cpp](tests/test_metadata.cpp) — hash of a known fixture matches

### 1.4 · `Identifier`
- [ ] Pass 1: TOSEC regex `^(.+?)\s*\((\d{4})\)\(([^)]+)\)`
- [ ] Pass 2: volume label (≥3 chars, strip trailing spaces, `_`→space) → OEM fallback → lone-PRG fallback
- [ ] Pass 3: optional `data/tosec_titles.json` SHA1 lookup
- [ ] Default tag `game` on TOSEC match; tag `utility` if `[util]` flag
- [ ] [tests/test_identifier.cpp](tests/test_identifier.cpp) — TOSEC strings, heuristic fallbacks, missing-JSON silent skip

### 1.5 · `Scanner` (sync mode)
- [ ] `std::filesystem::recursive_directory_iterator` filtered to `.st/.msa/.dim/.stx`
- [ ] Pipeline per image: `DiskReader` → `MetadataExtractor` → `Identifier` → `Database::upsertDisk` (single tx per image)
- [ ] WARN-and-continue on any image failure
- [ ] `--incremental` flag: skip images whose hash is already in DB
- [ ] Summary line: `N scanned, M added, K updated, J failed`

### 1.6 · `main.cpp` CLI routing
- [ ] Parse argv for `scan` / `query` / `launch` / `--gui` / `--db <path>` / `--incremental`
- [ ] `scan` subcommand wires Scanner → Database and prints the summary
- [ ] `--db` resolution: explicit path > `$MANIFEST_DB` env > `~/manifest.db`

### Phase 1 done-when
- [ ] `manifest scan <folder>` populates `manifest.db` without crashing on bad images
- [ ] Volume label, OEM name, file listing, geometry, image SHA1 stored for every readable image
- [ ] TOSEC filename parsing correctly identifies 20 known images
- [ ] `ctest` green

---

## Phase 2 — GUI MVP (Browse, Search, Launch) ⏳

Goal: GUI opens an existing DB and lets you find / inspect / launch disks.
No scanning from the GUI yet — Phase 3 adds that.

- [ ] `HatariLauncher::launch()` — `QProcess::startDetached("hatari", {path})`, `$PATH` check, error string when absent
- [ ] `gui::DiskTableModel` — load all rows from `Database`, `rowCount / columnCount / data / headerData`
- [ ] `QSortFilterProxyModel` wrapped around the model for live filter + column sort
- [ ] `gui::MainWindow`:
  - [ ] Menu bar (File / Scan / View / Help)
  - [ ] Toolbar with search box + clear button
  - [ ] Central `QTableView` bound to proxy
  - [ ] Detail dock (path, SHA1, OEM, geometry, file listing for selected row)
  - [ ] `[ Launch in Hatari ]` button wired to `HatariLauncher`
  - [ ] `[ Show in Files ]` button (`QDesktopServices::openUrl` on the containing folder)
  - [ ] `View → Show Identified column` toggle (persists via `QSettings`)
  - [ ] Right-click context menu on rows (Launch / Show Files / Copy path / Remove)
  - [ ] `File → Open Database…`
- [ ] Status-bar placeholder ("Ready")
- [ ] Smoke test: open a pre-populated DB, search, sort, launch

---

## Phase 3 — GUI Scan Integration ⏳

Goal: scan from inside the GUI with a live-updating table and progress bar.

- [ ] Make `Scanner` inherit `QObject`; add signals `progress(int scanned, int total, QString path)`, `imageDone(DiskRecord)`, `finished(Summary)`
- [ ] Move Scanner onto a `QThread` owned by `MainWindow`
- [ ] `Scan → Scan Folder…` opens `QFileDialog::getExistingDirectory`
- [ ] Status-bar shows running progress
- [ ] `imageDone` appends to `DiskTableModel` incrementally (no full reload)
- [ ] Cancel button in status bar (sets a `std::atomic<bool>` the Scanner checks between images)
- [ ] `Scan → Rescan` re-runs the last folder

---

## Phase 4 — Advanced Identification & Grouping ⏳

- [ ] Multi-disk detection pass (volume-label prefix + shared launcher SHA1)
- [ ] `disk_sets` table populated; `multidisk-NofM` tags applied
- [ ] GUI: `View → Multi-disk Sets` dock / tab
- [ ] GUI: `View → Duplicates` dock / tab (same image hash, different paths)
- [ ] GUI: tag filter sidebar (tree of `game / demo / utility / multidisk / ...`)

---

## Phase 5 — CLI Query Shell ⏳

- [ ] `cli::QueryCLI::run()` — readline loop, `manifest> ` prompt
- [ ] Commands: `find <term>`, `list`, `info <id>`, `launch <id>`, `tags <tag>`, `dupes`, `help`, `quit`
- [ ] `manifest query --find <term>` one-shot mode
- [ ] `manifest launch <id>` one-shot mode
- [ ] readline history persisted to `~/.manifest_history`

---

## Phase 6 — Polish, Packaging, Windows ⏳

- [ ] Logging: route WARN / ERR through a single sink (`std::cerr` in CLI, `QStatusBar` + log dock in GUI)
- [ ] `QSettings` for window geometry, last DB path, last scan folder, column visibility
- [ ] About dialog (version, engine attribution, license)
- [ ] Linux packaging: `.desktop` file, install target
- [ ] **Windows port** ❓
  - [ ] Replace `readline` with `wineditline` or a `std::getline` fallback
  - [ ] Confirm `QProcess::startDetached("hatari", ...)` works on Windows (path to `hatari.exe`)
  - [ ] `~/manifest.db` resolution on Windows (verify `%USERPROFILE%\manifest.db`)
  - [ ] MSVC / MinGW build verification
  - [ ] Installer (WiX or NSIS)
- [ ] Choose a license (TBD — currently placeholder in README)

---

## Parking Lot (nice-to-have, not scheduled)

- [ ] FTS5 virtual table for faster `find` across title + filenames
- [ ] Cover-art support if a good source ever surfaces
- [ ] Export catalog to CSV / JSON
- [ ] Import TOSEC DAT files directly (not just a pre-baked JSON)
- [ ] Disk-set manual editing in the GUI
- [ ] "Watch folder" mode: auto-scan on new file
- [ ] Dark theme switch

---

## Open Questions ❓

Track questions that need a decision before the relevant phase starts.

- **License** — required before first public commit.
- **Windows CLI input** — `wineditline` vs `std::getline` fallback.
- **DIM variants** — which DIM flavours does the vendored engine actually handle? Confirm during Phase 1.2 with a real `.DIM` fixture.
