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

## 6. Diagnostic Mode

```sh
manifest inspect path/to/image.st
```

Runs the full pipeline against one image **without touching the database** and prints everything ManifeST learned — format, geometry, volume label, OEM, SHA1s, identified title, file listing, tags. Useful for:

- Checking why an image didn't identify
- Comparing two copies of the same game
- Verifying a freshly downloaded disk before committing it to the catalogue

---

## 7. How Identification Works

Three passes run in order on every image; the first pass to produce a non-empty title wins.

1. **TOSEC filename parse** — `Title (Year)(Publisher)[flags].ext` (with `(19xx)` accepted as "unknown year"). Sets `identified_title`, `publisher`, `year`. Default tag `game`.
2. **Heuristics** —
   - Volume label ≥ 3 chars (`_` → space, leading/trailing whitespace stripped) is taken as the title
   - OEM name from the boot sector is used as a fallback **only if it's plain ASCII** (junk OEM strings like `NNNNNNÁë` are rejected)
   - If no label and no OEM, and the root directory contains exactly one `.PRG` / `.APP` / `.TOS`, the launcher's filename (minus extension) becomes the title
3. **Hash lookup** — if `data/tosec_titles.json` exists, the image's SHA1 is looked up against it. Format:

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

Missing file is silently skipped — the feature is strictly opt-in.

### Orthogonal tags

Regardless of which pass identifies the title, the following are tagged automatically:

- `multidisk-NofM` — from `(Disk N of M)` in the filename
- `cracked`, `trained`, `hacked`, `alt`, `verified` — from TOSEC bracket flags (`[cr]`, `[t]`, `[h]`, `[a]`, `[!]`)
- Cracker-group name — when the engine recognises a signature (D-Bug, Pompey, Medway, Elite, TRSI, Replicants, Copylock, …)
- `raw-loader` — when the engine reports `isRawLoaderDisk()` (boot-sector-only game with no filesystem)

---

## 8. Multi-disk Set Detection

After each scan, ManifeST runs a grouping pass with two strategies unioned:

1. **TOSEC title match** — disks with the same `identified_title` / `publisher` / `year` where the filename contains `(Disk N of M)`
2. **Volume-label prefix** — labels sharing a stem after stripping a trailing 1-9 digit (requires the resulting numbers to form a contiguous `1..N` sequence, which kills false positives like `MEDWAY 98` + `MEDWAY 100`)

Results go into the `disk_sets` table and surface in the sidebar (GUI) / `sets` command (CLI).

---

## 9. Troubleshooting

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

## 10. Moving or Backing Up the Catalogue

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

## 11. Data Integrity Notes

- `disks.path` is `UNIQUE` — re-scanning the same path updates in place
- `files` and `tags` cascade-delete on disk removal
- `disk_sets` is rebuilt from scratch after every scan — never stale
- FTS5 index is kept in sync automatically on every `upsertDisk` / `upsertFiles` / `removeDisk`
- Write-ahead logging (`PRAGMA journal_mode = WAL`) is enabled so reads never block writes
- Schema is version-stamped via `PRAGMA user_version` — upgrades apply incrementally on open; downgrades refuse to open with a clear error

---

## 12. Getting Help

- `manifest query` → `help` — command list
- Open a GitHub issue: <https://github.com/Darian-Frey> (the ManifeST repo once it's public)
- Read [CLAUDE.md](CLAUDE.md) for architecture, [ROADMAP.md](ROADMAP.md) for what's done / what's next
