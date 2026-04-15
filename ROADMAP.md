# ManifeST Roadmap

Single source of truth for "where we are / what's next". Update as work completes.
Status legend: ✅ done · 🔨 in progress · ⏳ todo · ⏸ blocked · ❓ needs decision

**Currently at:** Phase 6 + FTS5 full-text search ✅ complete. Windows port owned by Shane.

## Cracker / Menu-Disk Content Catalog ✅
- [x] Schema v2 → v3 migration adds `menu_contents(disk_id, position, game_name)` + `idx_menu_contents_game`
- [x] `DiskRecord::menu_games` vector, persisted via `Database::upsertMenuContents`, cascade-deletes with its disk
- [x] `MenuDiskCatalog` — loads `data/menu_disk_contents.json` shaped `{group_code: {disk_number: [games]}}`; resolves leading-zero variants (98 / 098 / 0098)
- [x] Disk detection — three strategies in order:
  1. Filename match (`Medway Boys Menu Disk 098 (...)`) → resolves group title → code
  2. Volume-label prefix (`MEDWAY 98`, `PP22`) → prefix-to-code map
  3. Engine cracker-group tag + trailing digits in label/filename
- [x] Scanner enriches records after `Identifier` runs; GUI + CLI both plumb the catalog automatically from `data/menu_disk_contents.json`
- [x] FTS5 integration — game names are concatenated into the indexed `files` column so `find Populous` finds the menu disk too
- [x] GUI detail pane now shows "Games on this menu (N)" as a numbered list
- [x] CLI `info <id>` prints the same menu-games list
- [x] **Two input-format importers** (new `manifest import-menu <format> <file>` subcommand):
  - `spiny` — tab-separated Medway list from https://www.spiny.org/medway/list.html (handles source's run-on-line bugs by injecting newlines before mid-line `CD<n>` tokens)
  - `8bitchip` — HTML-table game→disks inverted index from https://atari.8bitchip.info/MenuDG.html (covers MB/PP/DB/AU/CY/FZ/SU/VE/SO/FF/GG/PK/SD/TE). Proper `<tr><td>` parser with running "current game" across continuation rows.
- [x] Disk numbers canonicalized to stripped-leading-zeros (`032` merges with `32`)
- [x] Strategy 3 of detection (engine cracker-group tag + any digit) removed after surfacing false positive on `Zero 5 Loader.st`
- [x] **Seed data shipped in-repo**: `data/menu_disk_contents.json` (324 KB), 3,259 disks across 13 groups, scraped from spiny.org + 8bitchip.info with the project's own importers
- [x] Verified on the 44-disk collection: 16 real Medway menu disks (MB098–MB106 incl. all `[a]` alts and Part A/B) correctly enriched, **0 false positives**, Zero 5 Loader correctly gets 0 games

### Byte-level game detection (v4 schema) ✅
- [x] `GameStringScanner` — extracts printable ASCII runs from `DiskReader::rawImage()` bytes (min 4, max 120 chars, skips FAT12 dirent-shaped strings), intersects against catalog's known-game corpus with **word-boundary matching** and min game length 6 to eliminate `AGE`/`ORK`/`ELF`-style noise
- [x] `MenuDiskCatalog::allKnownGamesUpper()` — lazy-built corpus of ~4000 unique upper-cased game names drawn from every group in the catalog
- [x] Schema v3 → v4 migration: new `detected_games(disk_id, game_name, evidence)` table
- [x] `DiskRecord::detected_games` persisted + hydrated in `queryByHash`/`queryById`/`listAll`
- [x] Scanner runs byte scan after catalog enrich; detected names also fold into the FTS `files` column so `find <game>` matches disks where the game is only *detected* (not catalog-listed)
- [x] Detail pane shows two sections — **"Games on this menu — from catalog"** and **"Games detected on disk — from byte scan"** (with evidence column) — so catalog/detection agreement or discrepancy is visible
- [x] Real-world verification:
  - MB098 → catalog {LOTUS TURBO ESPRIT, JAMES POND}; detected {JAMES POND ✓, CORPORATION, EMPIRE} (two plausible false positives from cracktro text)
  - MB104 (no catalog entry) → 278 game hits extracted from the disk's internal database — genuine **discovery** on an un-catalogued menu disk
  - Arkanoid standalone → 4 hits including ARKANOID itself
  - Another World (binary-only, no text) → 0 hits, correctly
- [x] 3/3 unit tests still green
- [x] **Main-table Contents column** added — `DiskTableModel::Contents`, comma-joined game list, 1-based numbered tooltip for multi-line view. `FilterProxy` search box matches against it so typing a game name in the toolbar search filters the table to the menu disk(s) containing it.
- [x] `View → Show 'Games on this disk' column` toggle (parallel to Identified), persisted via `QSettings`, default on
- [x] `Database::listAll()` now hydrates `menu_games` alongside `tags` so the table populates without per-row queries
- [x] FTS5 search for games already worked at CLI: `find Lotus` → MB98, `find Mean Streets` → both parts of MB99

## TOSEC DAT Import ✅
- [x] `manifest::DatImporter` — ClrMamePro format parser (hand-rolled lexer, ~200 lines), no new deps
- [x] Tokenises `game (...)` / `rom (...)` blocks, extracts `name` + `sha1` pairs, skips everything else
- [x] Runs the TOSEC filename regex on each `rom.name` → writes SHA1 → `{title, publisher, year, tags}` entries into `data/tosec_titles.json`
- [x] Merges into existing JSON (preserves entries from other imported DATs)
- [x] New subcommand: `manifest import-dat <file.dat> [--json <output>]`
- [x] Scanner auto-picks-up `data/tosec_titles.json` if present (both CLI and GUI); `--tosec-json <path>` overrides
- [x] **Identifier pass reorder: SHA1 lookup now runs FIRST** — hash match is more authoritative than filename or volume-label guesses. Survives file renames, cracker rebrands, bogus labels
- [x] End-to-end verified: renamed `Another World.ST` → `mystery-disk.st`, imported fabricated DAT entry, scan correctly identified it as "Another World · Delphine · 1991" via SHA1 alone
- [x] Full 44-disk rescan: no regression (42/44 still identified)

## FTS5 Full-Text Search ✅
- [x] Schema version bumped 1 → 2, migration backfills existing rows into the new index
- [x] `disks_fts` virtual table with **trigram** tokenizer — preserves the old `LIKE '%term%'` substring behavior
- [x] Indexed columns: `title` · `filename` · `volume_label` · `files` (aggregated file names per disk)
- [x] Automatic sync in `upsertDisk` / `upsertFiles` / `removeDisk` — no triggers, no orphans
- [x] `queryByTitle` rewritten to `disks_fts MATCH ?` with phrase quoting so TOSEC punctuation can't be parsed as FTS operators; ordered by `rank`
- [x] CMake: `SQLITE_ENABLE_FTS5` compile-define on the `sqlite3` target
- [x] Verified: `find kan` → 2× Arkanoid (substring match), `find BANK03` → matches a file inside Another World (per-file index works), 3/3 tests still pass

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

### 1.3 · `MetadataExtractor` ✅
- [x] SHA1 of raw image bytes via OpenSSL `EVP_Digest` (SHA1 API deprecated in OpenSSL 3.x)
- [x] SHA1 per file, read via `DiskReader::readFileBytes(index)`
- [x] `is_launcher` heuristic: exactly one `.PRG` / `.APP` / `.TOS` in root
- [x] Refactored `DiskReader` to instance-based (pimpl owns engine) so MetadataExtractor can re-read file bytes without reloading
- [x] Added transient `FileRecord::in_root` flag (not persisted)
- [x] [tests/test_metadata.cpp](tests/test_metadata.cpp) — 3 known SHA1 vectors (empty, "abc", "fox") + vector overload
- [x] Smoke-verified against real disks: Arkanoid flags `ARKANOID.PRG`, Another World (no PRG) flags none

### 1.4 · `Identifier` ✅
- [x] Pass 1: TOSEC regex — handles `(19xx)` placeholder (leaves year unset), case-insensitive
- [x] Pass 2: volume label normalization (`_`→space, collapse), OEM with printable-ASCII guard, lone-launcher fallback
- [x] Pass 3: `data/tosec_titles.json` SHA1 lookup via `QJsonDocument` (no new deps), silent skip when missing
- [x] Orthogonal tags: `multidisk-NofM`, `cracked`, `trained`, `hacked`, `alt`, `verified`; default `game` on TOSEC match
- [x] [tests/test_identifier.cpp](tests/test_identifier.cpp) — 8 cases: TOSEC basic / with flags+multidisk / 19xx year / label fallback / OEM-junk rejection / lone launcher / JSON hash hit / JSON missing
- [x] Smoke-tested across 5 real disks: Arkanoid, Another World D1 (+multidisk tag), Artura (19xx), unidentified system disk, Medway Boys

### 1.5 · `Scanner` (sync mode) ✅
- [x] `std::filesystem::recursive_directory_iterator` filtered to `.st/.msa/.dim/.stx` (case-insensitive)
- [x] Pipeline per image: `DiskReader` → `MetadataExtractor` → `Identifier` → 3 upserts inside one `Database::Transaction`
- [x] WARN-and-continue on exceptions; `failed` count incremented
- [x] `--incremental` flag: skip by path if already present (path-based, not hash-based — see note below)
- [x] Optional `ProgressFn` callback (CLI prints `[i/N] path`, GUI will wire signals later)
- [x] Summary: `scanned / added / updated / skipped / failed`
- [ ] Follow-up: hash-based incremental (new column `raw_file_sha1`) deferred until a use case arises

### 1.6 · `main.cpp` CLI routing ✅
- [x] `manifest scan <folder> [--incremental] [--db <path>]`
- [x] `manifest inspect <path>` (diagnostic — shows full pipeline output for one image)
- [x] `--db` resolution: explicit flag > `$MANIFEST_DB` env > `$HOME/manifest.db` > `./manifest.db`
- [x] `query` / `launch` still stubbed for Phase 5

### Phase 1 done-when ✅
- [x] `manifest scan <folder>` populates `manifest.db` — 44/44 disks scanned, 0 failures, 1410 files + 62 tags stored
- [x] Volume label, OEM name, file listing, geometry, image SHA1 stored for every readable image
- [x] TOSEC filename parsing — 42/44 identified (2 non-TOSEC system disks correctly left unidentified)
- [x] `ctest` green — 3/3 passing

---

## Phase 2 — GUI MVP (Browse, Search, Launch) ✅

- [x] `HatariLauncher::launch()` — `QStandardPaths::findExecutable` + `QProcess::startDetached`
- [x] `Database` additions for the model: `listAll()`, `queryById()`, `removeDisk()`
- [x] `gui::DiskTableModel` — 8 columns (`Id / Title / Publisher / Year / Format / VolumeLabel / Tags / Identified`), ✓/✕ for Identified, tooltip rows, row helpers `idAtRow / pathAtRow`
- [x] `QSortFilterProxyModel` — case-insensitive filter over all columns, sortable headers
- [x] `gui::MainWindow`:
  - [x] Menu bar (File / View / Help) — Scan menu deferred to Phase 3
  - [x] Toolbar: Open DB · Launch in Hatari · Show in Files · search box (clear button)
  - [x] Central `QTableView` (sort, alternating rows, Title column stretched)
  - [x] Bottom Details dock (rich HTML: path / SHA1 / format / label / OEM / geometry / tags / file listing with ★ for launcher)
  - [x] `Launch in Hatari` wired to `HatariLauncher`, GUI popup on failure
  - [x] `Show in Files` opens the containing folder via `QDesktopServices`
  - [x] Right-click context menu: Launch / Show Files / Copy Path / Remove from Catalog (with confirm)
  - [x] `View → Show Identified column` toggle, persisted via `QSettings`
  - [x] Geometry + window state persisted via `QSettings`
  - [x] Status bar shows `filtered / total` counts
  - [ ] Follow-up: `File → Open Database…` currently stubbed — runtime DB reopen requires tearing down the model; deferred to Phase 6 polish
- [x] CMake fix: Q_OBJECT headers under `include/` added to `manifest_gui` source list so AUTOMOC finds them
- [x] Smoke test: `QT_QPA_PLATFORM=offscreen timeout 3 ./build/manifest --db /tmp/manifest-phase15.db` — starts, event loop runs, all 3 ctests still pass

---

## Phase 3 — GUI Scan Integration ✅

- [x] `Scanner` is now a `QObject` with signals `progress(int, int, QString)` / `imageDone(DiskRecord)` / `finished(Summary)`
- [x] `DiskRecord` + `Scanner::Summary` declared as Qt meta-types (`Q_DECLARE_METATYPE` + `qRegisterMetaType`) so they cross thread boundaries via queued signals
- [x] `MainWindow` owns a `QThread` + `Scanner`; scans are kicked off via `startScanRequested` signal (main → worker, queued)
- [x] `Scan → Scan Folder…` opens `QFileDialog::getExistingDirectory`, defaults to `$HOME` first time then the last folder
- [x] Status bar shows live progress: `[i/N] filename` label + `QProgressBar` + `Cancel` button (all hidden when idle)
- [x] `DiskTableModel::upsertRow` patches rows live — new rows `beginInsertRows` / `endInsertRows`, existing rows `dataChanged`
- [x] `Cancel` sets `std::atomic<bool>`; the Scanner loop exits at next iteration, `finished` still fires with partial summary
- [x] `Scan → Rescan` re-runs the last folder (path persisted via `QSettings`), full-scan mode (non-incremental)
- [x] CLI `scan` still works — now connects to the `progress` signal via a lambda
- [x] Destructor cancels + joins the worker thread cleanly

---

## Phase 4 — Advanced Identification & Grouping ✅

- [x] `MultiDiskDetector` — two strategies unioned: TOSEC title + `(Disk N of M)` filename pattern, and volume-label prefix with strict 1..N validation (≤9 disks) to avoid false positives like `MEDWAY 98` + `MEDWAY 100`
- [x] `disk_sets` table populated via `Database::rebuildDiskSets()` (full replace each pass; new `set_id` allocated from 1)
- [x] Detector runs automatically after every scan — both CLI (`runScan`) and GUI (`onScanFinished`)
- [x] New Database queries: `listDuplicates()`, `listDiskSets()`, `listAllTags()`, `idsWithTag()`
- [x] New types: `DiskSet`, `DuplicateGroup`, `TagCount`
- [x] `gui::FilterProxy` — replaces `QSortFilterProxyModel`; combines toolbar search text with an optional id whitelist
- [x] **Unified left sidebar** (`QTreeWidget`, replaces three separate docks for cleaner UX): `All Disks (N)` · `Duplicates (N)` · `Tags` (expandable, count per tag) · `Multi-disk Sets` (expandable, members per set). Click any node → table filters
- [x] Sidebar refreshes after scan and after manual remove
- [x] Verified against `disks/`: 4 correct sets (Another World, Arctic Moves, Medway 099, Medway 106), 0 false positives, 0 duplicates, 7 tags surfaced

---

## Phase 5 — CLI Query Shell ✅

- [x] `manifest::cli::QueryCLI` — interactive `manifest> ` prompt
- [x] Commands: `find <term>`, `list`, `info <id>`, `launch <id>`, `tags`, `tags <tag>`, `dupes`, `sets`, `help`, `quit`/`exit`/`q`
- [x] `manifest query --find <term> [--db <path>]` one-shot mode (exit 0 if matches found, 1 otherwise)
- [x] `manifest launch <id> [--db <path>]` one-shot mode
- [x] readline + history when `libreadline-dev` is present (`MANIFEST_HAVE_READLINE`); `std::getline` fallback otherwise — currently using fallback since readline isn't installed
- [x] History saved to `~/.manifest_history` on exit (when readline is enabled)
- [x] Smoke-tested: find/list/info/tags/sets/launch error path all work; 3/3 tests still pass

---

## Phase 6 — Polish, Packaging, Windows

### Linux polish ✅
- [x] `Version.hpp` with `kVersion` / `kProjectName` / `kAuthor` / `kEngineNote` constants
- [x] About dialog enhanced with version, engine attribution, author, license placeholder
- [x] `MainWindow` now owns the `Database` (via `unique_ptr`) — enables runtime reopen
- [x] `DiskTableModel::setDatabase` + `reload()` so the model can rebind to a new DB
- [x] File → Open Database… now actually works — tears down the worker thread, reopens DB, rebuilds model + sidebar, updates window title
- [x] Last DB path persisted via `QSettings`; next launch reopens it automatically (explicit `--db` still wins)
- [x] Keyboard shortcuts: Ctrl+O (open DB), Ctrl+Q (quit), Ctrl+S (scan folder), F5 (rescan), Ctrl+F (focus search), standard `QKeySequence::Find`
- [x] Empty-state nudge in status bar when the catalog is empty
- [x] Window title shows current DB basename
- [ ] Follow-up: unified log sink (route CLI `fprintf(stderr)` through a single facility so a future GUI log dock can also subscribe) — deferred

### Linux packaging ✅
- [x] `packaging/manifest.desktop` — Freedesktop entry, Utility;Archiving;Emulator
- [x] CMake `install` target: `bin/manifest` + `share/applications/manifest.desktop` via `GNUInstallDirs`
- [x] Verified: `cmake --install build --prefix /tmp/manifest-install` produces the right tree
- [ ] Icon file — not drawn yet; `Icon=manifest` in the .desktop resolves if one is later dropped into `share/icons/hicolor/256x256/apps/manifest.png`

### Windows port ⏸
Owned by Shane on a separate laptop. Not tracked here.

### License ✅
- [x] **MIT** — permissive, Qt-LGPL compatible, lowest-friction for a retro-hobby tool
- [x] [LICENSE](LICENSE) file at repo root with Shane Hartley / 2026 copyright
- [x] Note in LICENSE that the vendored engine subset is the same author, same terms
- [x] `Version.hpp::kLicense = "MIT"` — About dialog now reads "License: MIT" instead of "TBD"
- [x] [README.md](README.md) updated, links to LICENSE

---

## Parking Lot (nice-to-have, not scheduled)

- [x] FTS5 virtual table for faster `find` across title + filenames — **DONE**
- [x] TOSEC DAT import (hash-based identification survives file renames) — **DONE**
- [x] Cracker / menu-disk content catalog (Medway, Pompey, D-Bug, Automation, …) — **DONE** (see below)
- [ ] Cover-art support if a good source ever surfaces
- [ ] Export catalog to CSV / JSON
- [ ] Import TOSEC DAT files directly (not just a pre-baked JSON)
- [ ] Disk-set manual editing in the GUI
- [ ] "Watch folder" mode: auto-scan on new file
- [ ] Dark theme switch

---

## Open Questions ❓

- ~~**License**~~ — resolved: MIT.
- **Windows CLI input** — `wineditline` vs `std::getline` fallback. (Windows port is Shane's, not tracked here.)
- **DIM variants** — which DIM flavours does the vendored engine actually handle? Still unverified since `disks/` contains no `.DIM` fixtures. Will surface when one does.
