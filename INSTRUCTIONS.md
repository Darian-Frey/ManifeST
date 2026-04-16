# ManifeST — User Instructions

Everything you need to know to scan a folder of Atari ST disk images, find a title, and launch it in Hatari.

> The short version: **`manifest scan <folder>`** to catalogue, **`manifest`** to browse with the GUI, **`manifest query`** for the text shell. Keep reading for the details.

---

## Contents

- [0. Quick Reference](#0-quick-reference) — every command, flag, shortcut, and data file in one place
- [1. First-time Setup](#1-first-time-setup)
- [2. Database Location](#2-database-location)
- [3. Scanning a Folder](#3-scanning-a-folder)
- [4. Browsing: GUI](#4-browsing-gui)
- [5. Browsing: CLI Query Shell](#5-browsing-cli-query-shell)
- [6. Importing a TOSEC DAT](#6-importing-a-tosec-dat-hash-based-identification)
- [7. Importing Menu-Disk Contents](#7-importing-menu-disk-contents-medway-pompey-d-bug-)
- [8. ScreenScraper Enrichment](#8-screenscraper-enrichment-genre-synopsis-box-art)
- [9. Diagnostic Mode](#9-diagnostic-mode)
- [10. Notes (per-disk user comments)](#10-notes-per-disk-user-comments)
- [11. How Identification Works](#11-how-identification-works)
- [12. Multi-disk Set Detection](#12-multi-disk-set-detection)
- [13. Troubleshooting](#13-troubleshooting)
- [14. Moving or Backing Up the Catalogue](#14-moving-or-backing-up-the-catalogue)
- [15. Data Integrity Notes](#15-data-integrity-notes)
- [16. Getting Help](#16-getting-help)

---

## 0. Quick Reference

### CLI subcommands

| Command | What it does |
|---|---|
| `manifest` | Launch the GUI (picks up last-used DB from QSettings) |
| `manifest --gui` | Same, explicit |
| `manifest --db <path>` | Launch GUI against a specific DB |
| `manifest scan <folder>` | Deep scan — full pipeline |
| `manifest scan <folder> --quick` | Quick scan — image hash + file listing + identify only (≈90× faster) |
| `manifest scan <folder> --incremental` | Skip images whose path + mtime + size haven't changed |
| `manifest scan <folder> --tosec-json <path>` | Override the TOSEC hash DB location |
| `manifest inspect <image-path>` | Diagnostic dump of one disk (no DB writes) |
| `manifest query` | Interactive `manifest>` shell |
| `manifest query --find <term>` | One-shot text search; exit 0 on match, 1 on none |
| `manifest launch <id>` | Fire-and-forget Hatari launcher by disk ID |
| `manifest import-dat <file.dat>` | Parse a TOSEC ClrMamePro DAT into `data/tosec_titles.json` |
| `manifest import-menu spiny <file>` | Import spiny.org Medway Boys menu list |
| `manifest import-menu 8bitchip <file>` | Import 8bitchip.info multi-group menu index |
| `manifest export --format <csv/json/m3u>` | Dump the catalogue in the chosen format |

All subcommands accept `--db <path>` to target a specific database. Omit to use `$MANIFEST_DB` env var, or fall back to `$HOME/manifest.db`.

### Interactive shell commands (`manifest query`)

| Command | What it does |
|---|---|
| `find <term>` | FTS5 substring search across title / publisher / label / filenames / menu games / detected games |
| `list` | Every disk in the catalogue |
| `info <id>` | Full record for one disk |
| `launch <id>` | Exec Hatari with that disk |
| `tags` / `tags <tag>` | All tags with counts / list disks carrying a tag |
| `dupes` | Image-hash duplicate groups |
| `sets` | Multi-disk sets with members |
| `note <id>` | Edit (multi-line) or clear the note attached to a disk. Finish input with a single `.` on its own line; empty input clears. |
| `help` / `?` | Show the command list |
| `quit` / `exit` / `q` | Leave the shell |

### GUI actions

| Menu path | Shortcut | What it does |
|---|---|---|
| File ▸ New Database… | `Ctrl+N` | Create a fresh, empty catalogue at a chosen path (refuses to overwrite an existing file) |
| File ▸ Open Database… | `Ctrl+O` | Switch to a different `.db` file |
| File ▸ Save Database As… | `Ctrl+Shift+S` | Copy current DB to a new path, switch to it |
| File ▸ Close Database | `Ctrl+W` | Unload the current DB without opening another — scan/save actions disable, table empties |
| File ▸ Delete Database… | — | **Destructive**: permanently deletes the DB file (+ WAL/SHM sidecars) from disk. Requires typing the filename to confirm. |
| File ▸ Quit | `Ctrl+Q` | Exit |
| Edit ▸ Edit Note for Selected Disk… | `Ctrl+E` | Attach or edit a user-authored note on the selected disk |
| Scan ▸ Quick Scan… | `Ctrl+S` | Fast scan of a folder (image-level metadata only) |
| Scan ▸ Deep Scan… | `F6` | Full-pipeline scan of a folder |
| Scan ▸ Rescan | `F5` | Re-run the last folder in its last mode, non-incremental |
| View ▸ Show Identified column | — | Toggle the ✓/✕ column |
| View ▸ Show 'Games on this disk' column | — | Toggle the menu-contents column |
| Help ▸ Instructions | `F1` | Open this document in a reader window |
| Help ▸ About ManifeST | — | Version / license / attribution dialog |
| — (toolbar) | `Ctrl+F` | Focus the search box |

### Row actions (right-click on a table row)

- **Launch in Hatari** — starts the emulator with the selected image
- **Show in Files** — open the containing folder in your system file manager
- **Copy Path** — absolute image path to the clipboard
- **Edit Note…** — attach or edit a user-authored note (multi-line, preserved across rescans)
- **Remove from Catalog…** — delete the DB row (with confirmation). The `.st` file on disk is NOT deleted.

### Sidebar filters (left dock)

Click any node to filter the main table:

- **All Disks** — reset filter
- **Duplicates** — images sharing an image_hash with another disk
- **Tags** (expandable) — every tag with a disk count
- **Multi-disk Sets** (expandable) — auto-grouped sets, click a set to show its members

### Data files

| Path | Purpose | Format | Gitignored? |
|---|---|---|---|
| `data/tosec_titles.json` | TOSEC hash → title DB, built via `import-dat` | JSON, `{sha1: {title, publisher, year, tags}}` | Yes |
| `data/menu_disk_contents.json` | Cracker-menu contents, built via `import-menu` (seed ships in repo) | JSON, `{group_code: {disk_number: [games]}}` | No |
| `data/screenscraper_cache.json` | Optional ScreenScraper offline cache, built via `tools/screenscraper_sync.py` | JSON, `{version, games: {sha1: {...}}}` | Yes |
| `~/manifest.db` | Default SQLite catalogue — can override with `--db` or `$MANIFEST_DB` | SQLite 3, WAL, schema v7 | N/A — user data |

### Environment variables

| Name | Used by | Default |
|---|---|---|
| `MANIFEST_DB` | All subcommands | `$HOME/manifest.db` |
| `SS_DEV_ID` | `tools/screenscraper_sync.py` | (required — from screenscraper.fr) |
| `SS_DEV_PASSWORD` | `tools/screenscraper_sync.py` | (required) |
| `SS_USER` | `tools/screenscraper_sync.py` | (optional — raises rate limits) |
| `SS_USER_PASSWORD` | `tools/screenscraper_sync.py` | (optional) |
| `SS_SOFT_NAME` | `tools/screenscraper_sync.py` | `ManifeST` |
| `SS_DELAY_SEC` | `tools/screenscraper_sync.py` | `1.0` |
| `SS_SYSTEM_ID` | `tools/screenscraper_sync.py` | `42` (Atari ST on ScreenScraper) |

### Identification pipeline (priority order)

Applied in sequence; the first pass producing a non-empty title wins for that field. Later passes may still fill unrelated fields (genre, synopsis, etc.).

1. **SHA1 hash lookup** against `data/tosec_titles.json` — most authoritative
2. **TOSEC filename parse** — `Title (Year)(Publisher)[flags].ext`
3. **Heuristics** — volume label → OEM → lone-launcher filename
4. **ScreenScraper cache** — fills genre / synopsis / developer / box art (skipped if already set)

Orthogonal tags applied regardless of which pass identified:

- `multidisk-NofM`, `cracked`, `trained`, `hacked`, `alt`, `verified` — from TOSEC flags
- Cracker-group names (D-Bug, Medway Boys, Pompey Pirates, Vectronix, …) — via the parallel group detector
- `raw-loader` — when the engine reports `isRawLoaderDisk()`

### Schema versions

Current SQLite schema version: **7**. ManifeST auto-migrates older DBs forward on open; downgrades refuse with a clear error.

| Version | Added |
|---|---|
| 1 | Initial — disks, files, tags, disk_sets |
| 2 | FTS5 `disks_fts` virtual table with trigram tokenizer |
| 3 | `menu_contents` table (cracker-menu games) |
| 4 | `detected_games` table (byte-scan matches) |
| 5 | `file_mtime` + `file_size` columns on `disks` |
| 6 | `text_fragments` table (scrolltext / boot strings) |
| 7 | `genre / synopsis / developer / boxart_url / screenshot_url / screenscraper_id` on `disks` |

---

## 1. First-time Setup

### 1.1 Install the dependencies

Debian / Ubuntu / Mint:

```sh
sudo apt install build-essential cmake \
                 qt6-base-dev qt6-tools-dev \
                 libssl-dev libreadline-dev \
                 hatari
```

- `qt6-base-dev` pulls in Qt 6 Core + Widgets
- `libreadline-dev` is optional (no history / line editing without it, but the shell still works)
- `hatari` is optional but needed to actually launch a catalogued disk

### 1.2 Drop in the SQLite amalgamation

ManifeST bundles SQLite as a vendored amalgamation for portability. Download the amalgamation zip from <https://sqlite.org/download.html> (the `sqlite-amalgamation-*.zip` under "Source Code") and unpack `sqlite3.c` + `sqlite3.h` into:

```
third_party/sqlite3/sqlite3.c
third_party/sqlite3/sqlite3.h
```

Both files are gitignored — each developer drops their own.

### 1.3 Build

```sh
cmake -S . -B build
cmake --build build -j
```

The binary lands in `build/manifest`. Optional install:

```sh
sudo cmake --install build
```

Installs to `/usr/local/bin/manifest` and `/usr/local/share/applications/manifest.desktop` (so it shows up in your desktop application menu).

### 1.4 Verify

```sh
ctest --test-dir build
./build/manifest inspect path/to/any-image.st
```

Three tests should pass. `inspect` runs the full read pipeline on a single image without touching the database — a good smoke test.

---

## 2. Database Location

ManifeST stores everything in a single SQLite file. It resolves the path in this order:

1. Explicit `--db <path>` on the command line
2. `$MANIFEST_DB` environment variable
3. `$HOME/manifest.db` (default)

The GUI additionally remembers the last DB path via `QSettings`, so after the first launch with `--db` you don't need to pass it again.

---

## 3. Scanning a Folder

### 3.1 Scan modes: quick vs deep

`manifest scan` supports two depths:

| Mode | Flag | What it does | What it *skips* |
|------|------|--------------|-----------------|
| Quick | `--quick` | Image SHA1, file listing, TOSEC/menu-catalog identification, mtime tracking | Per-file hashes, byte-level game detection, scrolltext extraction, cracker-group signature scan |
| Deep  | (default, no flag) | Everything above + per-file SHA1s + game detection + scrolltext + cracker signatures | — |

**Use Quick when** you're cataloguing a fresh folder and just want titles/identification fast. Roughly **~90× faster** on a 44-disk test (0.18s vs 15.6s) — scales linearly to your full collection.

**Use Deep when** you want the full knowledge graph: per-file dedup, "find this game's name in the disk bytes", cracktro scrolltext, cracker-group tags.

You can always upgrade a Quick-scanned DB to Deep by re-running without `--quick` (non-incremental).

### 3.2 From the command line

```sh
# Quick scan — fast, minimum useful catalogue
manifest scan ~/AtariCollection --db ~/manifest.db --quick

# Deep scan — full pipeline (default)
manifest scan ~/AtariCollection --db ~/manifest.db

# Subsequent re-scans — skip images already in the DB if path+mtime+size match
manifest scan ~/AtariCollection --db ~/manifest.db --incremental
manifest scan ~/AtariCollection --db ~/manifest.db --incremental --quick
```

Each image prints `[i/N] path` as it's processed. On completion you get:

```
44 scanned, 44 added, 0 updated, 0 skipped, 0 failed
disk_sets: 4 groups
db: /home/you/manifest.db
```

- `added` / `updated` — new rows vs re-upsert of an existing path
- `skipped` — only non-zero in `--incremental` mode
- `failed` — images that threw (logged as `WARN` with the path + error)

The scanner also runs **multi-disk set detection** automatically after the scan finishes.

### 3.3 From the GUI

1. Launch `manifest` (with `--db` the first time, or let it reopen the remembered DB)
2. Pick a scan mode:
   - `Scan ▸ Quick Scan…` (or `Ctrl+S`) — fast; image hash + file listing + TOSEC/catalog match
   - `Scan ▸ Deep Scan…` (or `F6`) — full pipeline (per-file hashes, byte-level game detection, cracker signatures, scrolltext)
3. Pick the folder
4. Watch the table fill in live — status bar shows `[i/N] filename` with a progress bar
5. `Cancel` aborts after the current image finishes

`Scan ▸ Rescan` (or `F5`) re-runs the last folder, in whichever mode you last used, non-incrementally (picks up renames, edited TOSEC metadata, etc.).

---

## 4. Browsing: GUI

### 4.1 The main table

Columns (left to right): `ID` · `Filename` · `Title` · `Publisher` · `Year` · `Format` · `Volume Label` · `Tags` · `Games on this disk` · `Identified?` · `Notes`

The **Filename** column always has a value (the raw `.st` filename on disk) so unidentified rows still show something scannable. The **Notes** column shows a one-line preview of any attached note; hover for the full multi-line content, or select the row and `Ctrl+E` to edit.

- Click a header to sort
- Type in the search box (or `Ctrl+F`) to filter live — matches substring in any column
- `View ▸ Show Identified column` toggles the ✓/✕ indicator

### 4.2 The left sidebar

Clicking any node filters the main table:

- **All Disks (N)** — clear all filters
- **Duplicates (N)** — images with identical SHA1 at different paths
- **Tags ▸** — every tag that appears in the catalogue (`game`, `multidisk-1of2`, cracker-group names, etc.) with counts
- **Multi-disk Sets ▸** — auto-grouped sets (e.g. *Another World [2 disks]*). Click to filter the table to the set members

### 4.3 The details dock

Selecting a row populates the bottom dock with the full record: path, SHA1, format, volume label, OEM, geometry, tags, and the entire file listing (★ marks the launcher file).

### 4.4 Launching & managing

Three ways to launch:

- **Toolbar button** `Launch in Hatari`
- **Right-click** on a row → `Launch in Hatari`
- **Double-click** (not yet wired — use one of the above)

Right-click menu also offers:

- `Show in Files` — opens the containing folder in your system file manager
- `Copy Path` — copies the absolute image path
- `Remove from Catalog…` — deletes the DB row (file on disk is **not** deleted; confirmation dialog first)

### 4.5 Switching databases

`File ▸ Open Database…` (or `Ctrl+O`) — pops a file picker. The GUI reopens the chosen DB, rebuilds the model + sidebar, and updates the window title. The last-used path is remembered for the next launch.

### 4.6 Saving the database somewhere else

`File ▸ Save Database As…` (or `Ctrl+Shift+S`) — pops a Save dialog. Flushes the SQLite write-ahead log into the main `.db` file, copies it to the chosen path, and switches to the new file. The original stays put — this is a *copy-and-switch*, not a move. Useful for:

- Keeping a named "snapshot" of the catalogue before a big rescan
- Working copies vs long-term archive copies
- Migrating the DB to an external drive or syncing folder

If you pick the currently-open DB path, ManifeST says so and does nothing (no-op guard).

---

## 5. Browsing: CLI Query Shell

```sh
manifest query --db ~/manifest.db
```

Prompt: `manifest> `. Type `help` for the command list.

| Command          | What it does                                          |
| ---------------- | ----------------------------------------------------- |
| `find <term>`    | FTS5 substring search across title / publisher / label / filenames (including files inside each disk) |
| `list`           | Dump every disk in the catalogue as a table          |
| `info <id>`      | Full record for one disk (path, SHA1, files, tags)   |
| `launch <id>`    | Exec Hatari with that disk's image path              |
| `tags`           | List all tags with counts                             |
| `tags <tag>`     | List disks carrying that tag                          |
| `dupes`          | Group and print duplicate-hash sets                   |
| `sets`           | Print multi-disk sets with their members              |
| `help` / `?`     | Show the command list                                 |
| `quit` / `exit`  | Leave the shell                                       |

### One-shot mode

```sh
# Print matches and exit (exit code 0 on match, 1 on no match)
manifest query --db ~/manifest.db --find "Dungeon Master"

# Launch by id without opening the shell
manifest launch 42 --db ~/manifest.db
```

### History

When `libreadline-dev` is installed, commands are persisted to `~/.manifest_history` so arrow-key history works across sessions.

---

## 6. Importing a TOSEC DAT (hash-based identification)

TOSEC publishes ClrMamePro-format `.dat` catalogues containing the canonical
`Title (Year)(Publisher)[flags].st` name plus the SHA1 of the raw image for
every disk they've catalogued (~30,000+ Atari ST images). Feeding one into
ManifeST means any disk whose raw bytes match a TOSEC entry gets identified
by hash — even if the file has been renamed to nonsense or dumped yourself.

### Where to get a DAT

Download from <https://archive.org/details/tosec-main> (the `.dat` catalogue
files, not the ROMs) or the TOSECdev forum's Atari ST DAT section.
Look for files named like `Atari ST - Games - [ST] (TOSEC-v*.dat)`.

### Import

```sh
# Parses the DAT, merges into data/tosec_titles.json
manifest import-dat "Atari ST - Games.dat"

# Or point at a different JSON
manifest import-dat games.dat --json ~/my-tosec.json
```

Output is something like:

```
Parsing Atari ST - Games.dat
2934 rom entries, 2901 imported (0 overwrote existing), 33 skipped (no TOSEC name)
wrote data/tosec_titles.json
```

Running multiple DAT imports (Games + Utilities + Demos, say) merges them
into the same JSON — same SHA1s overwrite, new SHA1s append.

### Use it

Next time you scan:

```sh
manifest scan ~/AtariCollection --db ~/manifest.db
```

Any image whose raw SHA1 is in the catalogue will be identified via the
**SHA1 lookup pass**, which now runs *first* — ahead of the TOSEC filename
regex and volume-label heuristics. Hash match is the most authoritative
signal because it survives file renames, cracker rebrands, and junk volume
labels.

The GUI picks up the same JSON automatically if it's sitting at
`data/tosec_titles.json` relative to wherever you launched from.

### When to rebuild

- After importing a new DAT, rescan non-incrementally (`Scan ▸ Rescan` in
  the GUI, or drop `--incremental` on the CLI) so existing disks pick up
  the new identifications.
- The JSON file is portable — back it up alongside `manifest.db`.

---

## 7. Importing Menu-Disk Contents (Medway, Pompey, D-Bug, …)

Cracker-menu disks — the Medway Boys compilations, Pompey Pirates, D-Bug,
Automation, etc. — are single floppies that each contain several games.
The volume label will say something like `MEDWAY 98` but what's actually
playable on that disk (Populous, Xenon II, Rick Dangerous, …) isn't
recorded anywhere inside the image. A community-maintained catalog fills
that gap.

### The JSON schema

`data/menu_disk_contents.json`:

```json
{
  "MB": {
    "98":  ["Populous", "Xenon II", "Rick Dangerous"],
    "99":  ["Lemmings", "SimCity"]
  },
  "DB": {
    "001": ["Zak McKracken", "Maniac Mansion"]
  }
}
```

Group codes come from the 8bitchip index:

| Code | Group                |
| ---- | -------------------- |
| `AU` | Automation           |
| `CY` | Cynix                |
| `DB` | D-Bug                |
| `FF` | Flame of Finland     |
| `FZ` | Fuzion               |
| `GG` | SuperGAU             |
| `MB` | Medway Boys          |
| `PK` | Pompey Krappy        |
| `PP` | Pompey Pirates       |
| `SO` | Spaced Out           |
| `SU` | Superior             |
| `VE` | Vectronix            |

### Seed data ships with the repo

[`data/menu_disk_contents.json`](data/menu_disk_contents.json) is already
populated with **3,259 disks across 13 groups** — scraped once from
spiny.org + 8bitchip.info using ManifeST's own importers. You can use it
as-is. It covers:

| Code | Group              | Disks |
| ---- | ------------------ | ----- |
| `AU` | Automation         |  510  |
| `DB` | D-Bug              |  179  |
| `FF` | Flame of Finland   |   54  |
| `FZ` | Fuzion             |  198  |
| `GG` | SuperGAU           |  929  |
| `MB` | Medway Boys        |  217  |
| `PP` | Pompey Pirates     |  114  |
| `SU` | Superior           |  112  |
| `VE` | Vectronix          | 1000  |
| …    |                    |       |

### Refresh / import your own

If you want to refresh from source or add additional DATs, the two
importers work on raw downloads:

```sh
# Medway Boys — spiny.org tab-separated list
curl -o medway.html https://www.spiny.org/medway/list.html
manifest import-menu spiny medway.html

# 13-group game→disk inverted index
curl -o 8bitchip.html https://atari.8bitchip.info/MenuDG.html
manifest import-menu 8bitchip 8bitchip.html
```

Both importers **merge** into the existing JSON — run them back-to-back
to stack coverage. Order matters: the **8bitchip import preserves only
disk-position ordering from the source it came from**, while spiny.org
has the canonical Medway ordering. Run 8bitchip first, then spiny, so
spiny overwrites 8bitchip's MB entries with the clean order.

### Other community sources (not auto-imported)

- <https://www.gingerbeardman.com/archive/pompeypirates/> — per-disk Pompey pages (manual)
- <https://www.atarilegend.com/menusets> — Atari Legend (no API, polite scrape only)

### Use it

Next scan (CLI or GUI) automatically picks up `data/menu_disk_contents.json`
and enriches any matching disk. Evidence:

- **Detail pane** / `manifest query` → `info <id>` — a **"Games on this menu"**
  numbered list appears when the catalog matches the disk
- **FTS search** — `find Populous` returns both the stand-alone Populous
  release *and* every menu disk that contains it (the game names are
  folded into the search index)

Match happens via three routes, in priority order:

1. Filename matches `"<Group> Menu Disk NNN"` (TOSEC-named Medway / D-Bug disks)
2. Volume-label prefix + number (`MEDWAY 98`, `PP22`, `DB001`)
3. Engine-detected cracker group tag + any digit sequence in label/filename

Rescan non-incrementally after importing so existing DB rows get the new
games attached (`Scan ▸ Rescan` in the GUI, or drop `--incremental` on the
CLI).

---

## 8. ScreenScraper Enrichment (genre, synopsis, box art)

ScreenScraper ([screenscraper.fr](https://www.screenscraper.fr/)) is a
free, community-maintained retro gaming database that **matches by
file checksum** — works with ManifeST's SHA1 pipeline regardless of
filename. Coverage on Atari ST `.ST` / `.MSA` files is not guaranteed
but hits add genre, developer, synopsis, box-art URL, and screenshot
URL to your catalogue.

### Setup

1. Register a free account at <https://www.screenscraper.fr/> and
   request a **developer ID / password** (look for "API" on your
   account page — the community will typically approve within a day
   or two).
2. Set env vars:

```sh
export SS_DEV_ID=...
export SS_DEV_PASSWORD=...
# Optional but recommended — raises rate limits:
export SS_USER=your-screenscraper-username
export SS_USER_PASSWORD=your-user-password
```

### Sync

```sh
# Full sync — walks every disk in the catalogue, queries ScreenScraper,
# writes/appends data/screenscraper_cache.json. Polite 1s delay between
# requests; checkpoint every 25 rows; resumable if interrupted.
./tools/screenscraper_sync.py --db ~/manifest.db

# Try a small batch first to test credentials
./tools/screenscraper_sync.py --db ~/manifest.db --limit 20
```

The script prints per-disk `hit` / `miss` so you can see coverage
live. Misses are cached too (as `{}`) so re-runs skip them.

### Use it

Rescan (non-incremental) after syncing:

```sh
./build/manifest scan ~/AtariCollection --db ~/manifest.db
```

Hits appear in the GUI Details dock as new rows: **Genre**,
**Developer**, **Box art** (clickable), **Screenshot** (clickable),
and a full **Synopsis** paragraph. CLI `info <id>` also shows
these fields.

### Fill-only semantics

ScreenScraper is the **last** enrichment pass — it only fills fields
that are still empty. So if TOSEC already set publisher to "Imagine"
and ScreenScraper says "Taito", TOSEC wins. This protects the more
authoritative sources (TOSEC DAT hash match, filename parse) from
being overwritten by ScreenScraper's more general data.

### Don't have credentials?

ManifeST runs fine without ScreenScraper — the cache file is purely
optional. Missing genre/synopsis fields just stay empty.

---

## 9. Diagnostic Mode

```sh
manifest inspect path/to/image.st
```

Runs the full pipeline against one image **without touching the database** and prints everything ManifeST learned — format, geometry, volume label, OEM, SHA1s, identified title, file listing, tags. Useful for:

- Checking why an image didn't identify
- Comparing two copies of the same game
- Verifying a freshly downloaded disk before committing it to the catalogue

---

## 10. Notes (per-disk user comments)

Every disk can carry a free-form, multi-line note. Notes are stored in the
`notes` column on the `disks` table and are **never touched by the scanner**
— they survive full rescans, incremental rescans, and reidentification.

### Editing from the GUI

- Select a row → **Edit ▸ Edit Note for Selected Disk…** (or `Ctrl+E`), *or*
- Right-click a row → **Edit Note…**

Either pops a multi-line text dialog with the current note pre-loaded.
OK saves, Cancel discards. Entering empty text clears the note back to NULL.

Saved notes appear as a highlighted yellow box at the top of the Details
dock the next time you click the row — hard to miss.

### Editing from the CLI shell

```sh
manifest query --db ~/manifest.db
manifest> note 42
Current note for id=42 (Dungeon Master):
  (none)
Enter new note; single '.' on its own line finishes. Empty input clears the note.
Bought this at a car boot sale in 1991. Original box a bit dented
but disk reads cleanly. Still my favourite RPG.
.
Note saved.
```

Enter as many lines as you want; a single `.` on its own line closes
the note and persists it. Submit zero lines (just `.`) to clear.

`info <id>` always shows the current note indented under a `notes` header.

### Notes in exports

- **CSV**: last column (`notes`)
- **JSON**: `notes` field on every disk object (omitted if null)
- **M3U**: playlist format has no note semantics, so notes are skipped

### Why notes survive rescans

`upsertDisk` deliberately excludes `notes` from the `ON CONFLICT UPDATE`
clause. The scanner can overwrite every other field (title, publisher,
tags, menu games, scrolltext, etc.), but *never* touches the note. Your
comments are safe no matter how many times you re-scan the folder.

### Good uses

- "Disk edge slightly damaged; reads but consider imaging soon"
- "Got this from Alice's collection — her handwriting on the label"
- "Crashes in Hatari 2.5, try STeem"
- "This is the PAL 50Hz version, NOT the NTSC version"
- "Duplicate of id=123 but keeping the original label intact"

---

## 11. How Identification Works

Three passes run in order on every image; the first pass to produce a non-empty title wins.

1. **SHA1 hash lookup** (most authoritative) — if `data/tosec_titles.json` is present and the raw image's SHA1 matches an entry, we take the title / publisher / year / tags from it. This wins over everything else because hash identity is immune to filename games. Populate the JSON via `manifest import-dat` (Section 6). Missing JSON is silently skipped — the feature is strictly opt-in.

2. **TOSEC filename parse** — `Title (Year)(Publisher)[flags].ext` (with `(19xx)` accepted as "unknown year"). Sets `identified_title`, `publisher`, `year`. Default tag `game`.

3. **Heuristics** —
   - Volume label ≥ 3 chars (`_` → space, leading/trailing whitespace stripped) is taken as the title
   - OEM name from the boot sector is used as a fallback **only if it's plain ASCII** (junk OEM strings like `NNNNNNÁë` are rejected)
   - If no label and no OEM, and the root directory contains exactly one `.PRG` / `.APP` / `.TOS`, the launcher's filename (minus extension) becomes the title

JSON format consumed by Pass 1:

```json
{
  "sha1_hex_string": {
    "title": "Dungeon Master",
    "publisher": "FTL",
    "year": 1987,
    "tags": ["game", "rpg"]
  }
}
```

### Orthogonal tags

Regardless of which pass identifies the title, the following are tagged automatically:

- `multidisk-NofM` — from `(Disk N of M)` in the filename
- `cracked`, `trained`, `hacked`, `alt`, `verified` — from TOSEC bracket flags (`[cr]`, `[t]`, `[h]`, `[a]`, `[!]`)
- Cracker-group name — when the engine recognises a signature (D-Bug, Pompey, Medway, Elite, TRSI, Replicants, Copylock, …)
- `raw-loader` — when the engine reports `isRawLoaderDisk()` (boot-sector-only game with no filesystem)

---

## 12. Multi-disk Set Detection

After each scan, ManifeST runs a grouping pass with two strategies unioned:

1. **TOSEC title match** — disks with the same `identified_title` / `publisher` / `year` where the filename contains `(Disk N of M)`
2. **Volume-label prefix** — labels sharing a stem after stripping a trailing 1-9 digit (requires the resulting numbers to form a contiguous `1..N` sequence, which kills false positives like `MEDWAY 98` + `MEDWAY 100`)

Results go into the `disk_sets` table and surface in the sidebar (GUI) / `sets` command (CLI).

---

## 13. Troubleshooting

### "hatari not found on $PATH"

`sudo apt install hatari` — or install Hatari from your distro's package manager. Verify with `which hatari`. ManifeST uses `QProcess::startDetached`, which looks up the name on `$PATH`.

### An image shows `(unidentified)`

Run `manifest inspect <path>` to see what ManifeST found:

- If the filename isn't TOSEC-style, the identifier falls back to the volume label / OEM / launcher heuristic
- If none of those produce anything, populate `data/tosec_titles.json` with a SHA1 → metadata mapping and rescan
- If you rename the file to the TOSEC pattern, `manifest scan <folder>` (non-incremental) will re-identify it

### "no such module: fts5"

The SQLite amalgamation wasn't compiled with `SQLITE_ENABLE_FTS5`. ManifeST's CMakeLists adds that define automatically — if you're seeing this error, it means CMake didn't reconfigure after pulling updates. Run:

```sh
cmake -S . -B build
cmake --build build -j
```

### GUI shows "Empty catalog"

You haven't scanned a folder into this DB yet. `Scan ▸ Quick Scan…` (or `Ctrl+S`) for a fast first pass, or `Scan ▸ Deep Scan…` (F6) for the full pipeline.

### Scan fails on a specific image

The scanner logs `WARN: <path> — <reason> — skipping` and continues. Common causes:

- Unreadable / truncated image — confirm with `manifest inspect <path>`
- Unsupported DIM variant (the engine handles plain `.DIM` via magic-byte passthrough; Pasti `.STX` has its own magic)
- MSA with a corrupt compression track — the engine reports the specific failure

### `[DIAG] Standard BPB Detected` noise

That's the engine's internal diagnostic print, not ManifeST's. Harmless. A future release may silence it.

---

## 14. Moving or Backing Up the Catalogue

Everything lives in the single `manifest.db` file. Copy it anywhere — ManifeST is portable as long as the image paths stored inside are still reachable.

If you move the image collection, the recorded `path` column becomes stale. Two options:

- **Rescan from the new location** — `manifest scan <new-folder>` adds new rows for the new paths; `Remove from Catalog` the stale entries
- **SQL patch the paths** — power-user workaround:

```sh
sqlite3 ~/manifest.db \
  "UPDATE disks SET path = REPLACE(path, '/old/prefix/', '/new/prefix/');"
```

The `image_hash` doesn't change, so duplicate detection still works correctly after a path edit.

---

## 15. Data Integrity Notes

- `disks.path` is `UNIQUE` — re-scanning the same path updates in place
- `files` and `tags` cascade-delete on disk removal
- `disk_sets` is rebuilt from scratch after every scan — never stale
- FTS5 index is kept in sync automatically on every `upsertDisk` / `upsertFiles` / `removeDisk`
- Write-ahead logging (`PRAGMA journal_mode = WAL`) is enabled so reads never block writes
- Schema is version-stamped via `PRAGMA user_version` — upgrades apply incrementally on open; downgrades refuse to open with a clear error

---

## 16. Getting Help

- `manifest query` → `help` — command list
- Open a GitHub issue: <https://github.com/Darian-Frey> (the ManifeST repo once it's public)
- Read [CLAUDE.md](CLAUDE.md) for architecture, [ROADMAP.md](ROADMAP.md) for what's done / what's next
