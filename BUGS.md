# ManifeST — Bug Log

A running record of bugs discovered during development, what caused them, and
how they were fixed. Primarily a reference for future debugging — if a similar
symptom reappears, the fix pattern is usually already here.

Status legend: ✅ fixed · 🔨 in progress · ⏳ known / not yet scheduled · ❌ won't fix

---

## Build-system / environment

### B001 · Vendored engine `.cpp` had a relative include ✅
- **Symptom:** `#include "../include/AtariDiskEngine.h" — No such file or directory` on first CMake build.
- **Root cause:** We flattened the vendored engine into `third_party/atari-engine/` (just two files side-by-side) but the upstream `AtariDiskEngine.cpp` has a hard-coded `#include "../include/AtariDiskEngine.h"` relative path. Moving the file changed its relative parent.
- **Fix:** Restored the original `include/` and `src/` subdirectory layout inside `third_party/atari-engine/` and pointed `target_include_directories` at the new subpath. Keeps the vendored file byte-identical to upstream (per [CLAUDE.md](CLAUDE.md)'s "vendor verbatim" rule).

### B002 · SQLite target had no link language ✅
- **Symptom:** `Cannot determine link language for target "sqlite3"`.
- **Root cause:** `project(ManifeST LANGUAGES CXX)` didn't enable C, and the SQLite amalgamation is a single `.c` file.
- **Fix:** `project(ManifeST LANGUAGES CXX C)`.

### B003 · FTS5 "no such module" at runtime ✅
- **Symptom:** `sqlite3_exec failed: no such module: fts5` during schema migration.
- **Root cause:** SQLite's amalgamation doesn't compile FTS5 by default.
- **Fix:** `target_compile_definitions(sqlite3 PUBLIC SQLITE_ENABLE_FTS5)` in [CMakeLists.txt](CMakeLists.txt).

### B004 · MOC missed Q_OBJECT headers under `include/` ✅
- **Symptom:** `undefined reference to vtable for manifest::gui::MainWindow` at link time; same for every Q_OBJECT class.
- **Root cause:** AUTOMOC only scans files listed on the target. `MainWindow.hpp` was under `include/manifest/gui/` which AUTOMOC doesn't pick up implicitly when only `src/gui/MainWindow.cpp` is listed.
- **Fix:** List the Q_OBJECT `.hpp` files explicitly in the `add_library(...)` source list (still compiles correctly; AUTOMOC now finds them).

### B005 · `libreadline` hard-required the build green ✅
- **Symptom:** `/usr/bin/ld: cannot find -lreadline` on a box without `libreadline-dev`.
- **Root cause:** CMake unconditionally linked readline into `manifest_cli`, but readline is only used at Phase-5 QueryCLI runtime.
- **Fix:** `find_library(READLINE_LIB readline)` guard; when present, link + define `MANIFEST_HAVE_READLINE`; when absent, the CLI uses `std::getline` fallback.

---

## Database / schema

### B010 · Private `Impl` inaccessible from anonymous-namespace helpers ✅
- **Symptom:** `'struct manifest::Database::Impl' is private within this context` on helper functions that took `Database::Impl&`.
- **Root cause:** `Database::Impl` is a private nested type; free helpers in an anon namespace aren't friends.
- **Fix:** Helpers (`loadFiles`, `loadTags`, `loadMenuGames`, etc.) were rewritten to take the specific `sqlite3_stmt*` pointers they need, rather than an `Impl&`.

### B011 · User notes wiped on rescan ✅
- **Symptom:** After adding a note and re-scanning the folder, the note was gone.
- **Root cause:** `upsertDisk`'s `ON CONFLICT(path) DO UPDATE SET` clause included `notes = excluded.notes`, so a re-insert (which has an empty note in the record) nulled the stored note.
- **Fix:** Removed `notes` from the UPDATE branch. Notes are user-authored and now survive all scans. `Database::setNotes(id, text)` is the only API that modifies them.

---

## Identifier / detection logic

### B020 · `Zero 5 Loader.st` picked up Medway Boys Disk 5's games ✅
- **Symptom:** A stand-alone cracker-boot disk was tagged with games from a completely unrelated Medway menu disk.
- **Root cause:** The third detection strategy in `MenuDiskCatalog::detectMenuDisk` combined an engine-supplied cracker-group tag with *any digit sequence in the filename*. The filename `Zero 5 Loader.st` has a `5` → looked up Medway `5` → attached four wrong games.
- **Fix:** Dropped strategy 3 entirely. Only strategies 1 (filename "<Group> Menu Disk NNN") and 2 (volume-label prefix + trailing digit) remain — both produce 0 false positives on the 44-disk test collection.

### B021 · `AGE` / `ELF` / `ORK` false matches in byte-level game detection ✅
- **Symptom:** Every disk with any English text had dozens of spurious game hits — games with very short titles matched arbitrary words.
- **Root cause:** `GameStringScanner` used plain substring matching against the full known-games corpus, which contains short titles like `AGE` and `ELF`. "WORKER" matched "ORK".
- **Fix:** Added `containsAsWord()` with word-boundary checks (`!isalnum` before and after the match position) + raised `min_game_len` from 3 to 6. Cut false-positive rate to near zero on the test collection.

### B022 · Multi-disk detector glued `MEDWAY 98` and `MEDWAY 100` into one set ✅
- **Symptom:** Multi-disk grouping reported a false "set 3" containing `Medway Boys Menu Disk 098` + `Medway Boys Menu Disk 100` — unrelated disks from the same numbered series.
- **Root cause:** Volume-label prefix detection stripped trailing digits (`MEDWAY 98` → `MEDWAY`, `MEDWAY 100` → `MEDWAY`) and grouped any disks sharing that prefix, regardless of the resulting disk-number distribution.
- **Fix:** Require the resulting disk numbers to form a contiguous `1..N` sequence with `N ≤ 9`. `MEDWAY 98` would need `MEDWAY 1..97` to also exist to be grouped — doesn't happen on any real collection.

### B023 · Seed JSON had `032` and `32` as separate keys ✅
- **Symptom:** Both `"MB": { "032": [...], "32": [...] }` coexisting in the shipped seed file after running 8bitchip + spiny importers back-to-back.
- **Root cause:** spiny.org uses bare numbers (`CD32`), 8bitchip uses zero-padded (`MB032`). The importers stored them verbatim.
- **Fix:** `canonicalDiskNumber()` helper in `MenuImporter.cpp` strips leading zeros on ingest. `MenuDiskCatalog::lookup` already tried both forms for robustness, but dedup at import time shrinks the JSON (3,356 → 3,259 disks) and prevents duplicate game lists.

### B024 · spiny.org source has mashed-together disk lines ✅
- **Symptom:** Importer skipped ~3 Medway disks (CD12, CD14, CD25).
- **Root cause:** Source `.html` has rendering bugs where one row runs into the next with no newline: `STUNTTEAMCD12\tLAST TROOPER...`.
- **Fix:** Pre-processed the input with a regex that injects a newline before any mid-line `CD<digits>` token. Gap count: 3 → 0.

---

## GUI

### B030 · `CrackerGroupDetector` segfaulted during first scan ✅
- **Symptom:** Scanner crashed with SIGSEGV mid-pipeline the first time the cracker-group detector ran.
- **Root cause:** The signature table used `std::initializer_list<const char*>` as a struct field (`GroupSig::patterns`). `std::initializer_list` only references a temporary backing array that lives until the end of the full-expression — by the time the static `kSignatures` vector was used, the arrays were gone. Classic C++ footgun.
- **Fix:** Changed `GroupSig::patterns` to `std::vector<const char*>`. Allocates its own storage; lives as long as the outer vector.

### B031 · Main-table columns "fought" the user on resize ✅
- **Symptom:** Dragging a column divider made the opposite side of the table move (e.g. widening Publisher shrank Title, shifting the right edge).
- **Root cause:** `Title` column was set to `QHeaderView::Stretch` mode, which makes it absorb leftover width whenever other columns resize. Combined with `setStretchLastSection(false)` this produced counter-intuitive behavior.
- **Fix:** All columns now `QHeaderView::Interactive` with sensible default widths. `Title` is no longer special. Each column resizes independently; any leftover space just appears as whitespace on the right.

### B032 · Help ▸ Instructions truncated at random section ✅
- **Symptom:** Dialog rendered only part of `INSTRUCTIONS.md` — sometimes stopping at §3.2, sometimes at §7, never showing the full document.
- **Root cause:** Three markdown-syntax issues confused Qt's cmark port:
  1. `` `manifest> ` `` had a trailing space inside backticks — parser treated the code span as unclosed, consumed text until the next backtick elsewhere.
  2. `` `manifest export --format csv \| json \| m3u` `` — backslash-escaped pipes inside a code span inside a table cell. Ambiguous to most parsers; Qt's table tokeniser chose badly.
  3. A duplicate `### 3.2` subsection heading created colliding fragment IDs.
- **Fix:** Sanitized all three constructs. Additionally switched the dialog to `QTextDocument::MarkdownDialectGitHub` for better table handling, and added an "Open in External Viewer" button as an escape hatch.

### B033 · Open Database button said "Save" ✅
- **Symptom:** File ▸ Open Database… popped a file picker with the action button labelled "Save" instead of "Open".
- **Root cause:** `onOpenDatabase` used `QFileDialog::getSaveFileName` because early on we wanted "Open or Create" behaviour from a single dialog. After File ▸ New Database… became a separate entry, the getSave choice became wrong.
- **Fix:** Switched to `QFileDialog::getOpenFileName`. Open Database now *only* opens existing files — creating goes through New Database.

### B034 · New/Save-As didn't append `.db` extension ✅
- **Symptom:** User typed `my-collection` in the filename box; the file saved as `my-collection` (no extension). Subsequent Open Database didn't show it because the default filter is `*.db`.
- **Root cause:** The static `QFileDialog::getSaveFileName` overload doesn't expose `setDefaultSuffix()`.
- **Fix:** Rewrote New Database and Save As to use a `QFileDialog` object with `setDefaultSuffix("db")`. Filenames without an explicit extension now save as `<name>.db`.

---

## Notes / tooling

### N001 · Clangd "phantom" errors on fresh projects
- **Symptom:** IntelliSense reports `std::string is aka int`, `std::optional missing member`, and similar obvious lies.
- **Root cause:** clangd hasn't seen a `compile_commands.json` yet — no include paths, defaults to wrong assumptions.
- **Workaround:** Run `cmake -S . -B build` once to generate the compile database. clangd picks it up on next reindex. Real compile is always authoritative; warnings that don't reproduce under `cmake --build` can be ignored.

---

## Process

When you find a new bug:

1. Reproduce it and capture the exact symptom (error message, unexpected output, stack trace if crash).
2. Bisect to root cause (what change or input triggers it).
3. Add a new entry here with `B0NN` id, the symptom-then-cause structure above, and the fix reference (file + commit).
4. When the fix lands in a commit, flip the status to ✅.

Keep entries terse — one-screen read. Large post-mortems can live in CLAUDE.md or a PR description; BUGS.md is the ten-second lookup.
