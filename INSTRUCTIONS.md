# ManifeST — User Instructions

Everything you need to know to scan a folder of Atari ST disk images, find a title, and launch it in Hatari.

> The short version: **`manifest scan <folder>`** to catalogue, **`manifest`** to browse with the GUI, **`manifest query`** for the text shell. Keep reading for the details.

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

### 3.1 From the command line

```sh
# First scan
manifest scan ~/AtariCollection --db ~/manifest.db

# Subsequent re-scans — skip images already in the DB by path
manifest scan ~/AtariCollection --db ~/manifest.db --incremental
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

### 3.2 From the GUI

1. Launch `manifest` (with `--db` the first time, or let it reopen the remembered DB)
2. `Scan ▸ Scan Folder…` (or `Ctrl+S`)
3. Pick the folder
4. Watch the table fill in live — status bar shows `[i/N] filename` with a progress bar
5. `Cancel` aborts after the current image finishes

`Scan ▸ Rescan` (or `F5`) re-runs the last folder in full (non-incremental) mode, which is what you want after editing TOSEC filenames.

---

## 4. Browsing: GUI

### 4.1 The main table

Columns: `ID` · `Title` · `Publisher` · `Year` · `Format` · `Volume Label` · `Tags` · `Identified?`

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

## 8. Diagnostic Mode

```sh
manifest inspect path/to/image.st
```

Runs the full pipeline against one image **without touching the database** and prints everything ManifeST learned — format, geometry, volume label, OEM, SHA1s, identified title, file listing, tags. Useful for:

- Checking why an image didn't identify
- Comparing two copies of the same game
- Verifying a freshly downloaded disk before committing it to the catalogue

---

## 9. How Identification Works

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

## 10. Multi-disk Set Detection

After each scan, ManifeST runs a grouping pass with two strategies unioned:

1. **TOSEC title match** — disks with the same `identified_title` / `publisher` / `year` where the filename contains `(Disk N of M)`
2. **Volume-label prefix** — labels sharing a stem after stripping a trailing 1-9 digit (requires the resulting numbers to form a contiguous `1..N` sequence, which kills false positives like `MEDWAY 98` + `MEDWAY 100`)

Results go into the `disk_sets` table and surface in the sidebar (GUI) / `sets` command (CLI).

---

## 11. Troubleshooting

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

You haven't scanned a folder into this DB yet. `Scan ▸ Scan Folder…` (or `Ctrl+S`).

### Scan fails on a specific image

The scanner logs `WARN: <path> — <reason> — skipping` and continues. Common causes:

- Unreadable / truncated image — confirm with `manifest inspect <path>`
- Unsupported DIM variant (the engine handles plain `.DIM` via magic-byte passthrough; Pasti `.STX` has its own magic)
- MSA with a corrupt compression track — the engine reports the specific failure

### `[DIAG] Standard BPB Detected` noise

That's the engine's internal diagnostic print, not ManifeST's. Harmless. A future release may silence it.

---

## 12. Moving or Backing Up the Catalogue

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

## 13. Data Integrity Notes

- `disks.path` is `UNIQUE` — re-scanning the same path updates in place
- `files` and `tags` cascade-delete on disk removal
- `disk_sets` is rebuilt from scratch after every scan — never stale
- FTS5 index is kept in sync automatically on every `upsertDisk` / `upsertFiles` / `removeDisk`
- Write-ahead logging (`PRAGMA journal_mode = WAL`) is enabled so reads never block writes
- Schema is version-stamped via `PRAGMA user_version` — upgrades apply incrementally on open; downgrades refuse to open with a clear error

---

## 14. Getting Help

- `manifest query` → `help` — command list
- Open a GitHub issue: <https://github.com/Darian-Frey> (the ManifeST repo once it's public)
- Read [CLAUDE.md](CLAUDE.md) for architecture, [ROADMAP.md](ROADMAP.md) for what's done / what's next
