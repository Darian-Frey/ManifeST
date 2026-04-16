# ManifeST

A batch disk image cataloguer for Atari ST software collections.

Point `manifest` at a folder of `.ST`, `.MSA`, `.DIM`, or `.STX` images and it
walks the tree, mounts each image via a vendored subset of the Atari Disk
Engine, extracts metadata, identifies the title, and persists everything to a
local SQLite database. Browse with either a Qt 6 GUI (searchable table,
multi-disk sets, duplicates, tag sidebar, launch-in-Hatari button) or a
readline query shell.

**Status:** v0.1.0-dev — CLI and GUI both functional, 3/3 unit tests passing,
schema at v7, 30 MB AppImage ships for Linux x86_64. Windows port owned on a
separate branch.

Read-me priorities:

- **End-user usage:** [INSTRUCTIONS.md](INSTRUCTIONS.md)
- **Architecture / conventions:** [CLAUDE.md](CLAUDE.md) *(local development note, gitignored)*
- **Phase-by-phase progress log:** [ROADMAP.md](ROADMAP.md)
- **Known-bug history:** [BUGS.md](BUGS.md)

---

## Features

### Cataloguing

- Recursive walk of `.ST` / `.MSA` / `.DIM` / `.STX` images
- Boot-sector parsing, volume-label extraction (direct raw-sector scan), full root + subdir file listing
- SHA1 hashes for both the raw image (always) and every contained file (deep-scan mode)
- Launcher heuristic — flags the lone `.PRG` / `.APP` / `.TOS` in root
- MSA + STX decompression via the vendored engine
- Cracker-group detection on two layers:
  - **Engine-native** signatures (D-Bug, Pompey, Medway, Cynix, Elite, TRSI, Replicants, Copylock, Sleepwalker, …)
  - **Parallel in-ManifeST** signature detector covering 27 further groups (Automation, LSD, Empire, Fuzion, Vectronix, Was (Not Was), Spaced Out, Midland Boyz, etc.) — candidates for upstreaming back into the engine once stable
- **Scrolltext capture** — engine's boot-sector strings + the top 25 longest printable runs from the whole image, persisted as `text_fragments` rows for display in the GUI details dock

### Scan modes

- **Quick** (`--quick` / GUI `Scan ▸ Quick Scan…`) — image SHA1 + file listing + identification + menu-catalog match. Skips per-file hashing, byte-level game detection, scrolltext capture, cracker signature scan. **≈90× faster** on the test collection.
- **Deep** (default / `Scan ▸ Deep Scan…`) — everything Quick does plus the skipped passes. Produces the full knowledge graph.
- **Incremental** (`--incremental`) — skips images whose path + mtime + size match what's stored. 1500× speedup on an unchanged collection.

### Identification pipeline

Applied in priority order; the first pass to produce a non-empty title wins. Later passes still fill unrelated fields (genre, synopsis, …) without overwriting earlier ones.

1. **SHA1 hash lookup** in `data/tosec_titles.json` — imported via `manifest import-dat <tosec.dat>`
2. **TOSEC filename parse** — `Title (Year)(Publisher)[flags].ext`
3. **Heuristics** — volume label → OEM → lone-launcher filename
4. **ScreenScraper cache** in `data/screenscraper_cache.json` — fills genre, synopsis, developer, box-art / screenshot URLs. Built offline via `tools/screenscraper_sync.py`.

Orthogonal tags applied regardless of which pass identifies:
`multidisk-NofM`, `cracked`, `trained`, `hacked`, `alt`, `verified`,
cracker-group names, and `raw-loader`.

### Grouping

- **Duplicate detection** via image SHA1
- **Multi-disk set detection** — TOSEC `(Disk N of M)` filenames + volume-label prefix with contiguous-numbering validation (rejects false positives like `MEDWAY 98` + `MEDWAY 100`)
- **Persistent `disk_sets` table**, rebuilt after every scan

### Cracker-menu content catalog

- `data/menu_disk_contents.json` ships in-repo — **3,259 disks across 13 groups** (Medway Boys, Automation, D-Bug, Pompey Pirates, Vectronix, Fuzion, Superior, SuperGAU, Flame of Finland, Cynix, Krappy, Spaced Out, Sewer Doc Disk)
- Scraped once from spiny.org + 8bitchip.info via `manifest import-menu` (spiny + 8bitchip input formats supported)
- Cross-checked at scan time against byte-level strings (`detected_games` table) so search hits disks where the game is listed OR the bytes say so
- Menu game list appears both as a dedicated "Games on this disk" column in the main table and as a numbered section in the Details dock

### Search

- **SQLite FTS5** with the trigram tokenizer — substring match across title, filename, volume label, aggregated filenames, menu games, and detected games
- Schema-versioned migrations; current `user_version = 7`

### Per-disk user notes

- Every disk can carry a free-form multi-line note
- Preserved across rescans (scanner never touches the `notes` column)
- Editable via `Edit ▸ Edit Note…` (GUI, `Ctrl+E`), right-click context menu, or `note <id>` in the CLI shell
- Shown as a yellow callout in the Details dock and truncated preview in the main-table `Notes` column
- Exported in CSV and JSON

### GUI (Qt 6 Widgets)

- Sortable, live-filterable main table — 11 columns: `ID · Filename · Title · Publisher · Year · Format · Volume Label · Tags · Games on this disk · Identified? · Notes`
- Left sidebar: *All Disks · Duplicates · Tags · Multi-disk Sets* (click to filter)
- Bottom Details dock (rich HTML: path, SHA1, geometry, note callout, tags, catalog-listed games, byte-detected games, scrolltext/boot strings, file listing with ★ for launcher)
- Background scan on a `QThread` with progress bar + Cancel
- `Launch in Hatari` button + right-click context menu (Launch / Show in Files / Copy Path / Edit Note… / Remove from Catalog…)
- **File menu rounded out**: New · Open · Save As · Close · Delete · Quit
- **Help ▸ Instructions** (`F1`) opens a rendered-markdown viewer of INSTRUCTIONS.md with an escape-hatch "Open in External Viewer" button
- Persistent window state, last DB path, last scan folder, column visibility, scan-mode last-used

### CLI

- `manifest scan <folder> [--quick] [--incremental] [--tosec-json <path>]`
- `manifest query` — interactive `manifest>` shell with `find / list / info / launch / tags / dupes / sets / note`
- `manifest query --find <term>` — one-shot search
- `manifest launch <id>` — direct Hatari launcher
- `manifest inspect <path>` — single-disk diagnostic, no DB writes
- `manifest import-dat <file.dat>` — populate TOSEC hash → title JSON
- `manifest import-menu spiny <file>` / `import-menu 8bitchip <file>` — menu-disk content importers
- `manifest export --format csv|json|m3u` — catalogue dump; M3U groups multi-disk sets with `#EXTINF:0,Title · Disk N` entries
- readline + `~/.manifest_history` when `libreadline-dev` is installed; `std::getline` fallback otherwise

### Packaging

- **AppImage**: [packaging/build-appimage.sh](packaging/build-appimage.sh) produces `dist/manifest-x86_64.AppImage` (~30 MB, stripped, zstd-squashfs). Bundles Qt 6 Widgets/Core/Gui + xcb plugin + seed data (menu catalog, TOSEC JSON if present, INSTRUCTIONS.md / README.md / LICENSE). Runs on any modern x86_64 Linux with FUSE.
- **CMake install target**: `cmake --install build` puts `bin/manifest`, `share/manifest/{INSTRUCTIONS.md,README.md,LICENSE,data/*}`, and a Freedesktop `.desktop` file under the install prefix.

---

## Repository Layout

```
ManifeST/
├── CMakeLists.txt
├── CLAUDE.md                  # architecture & conventions (gitignored)
├── ROADMAP.md                 # phase-by-phase progress log
├── BUGS.md                    # known-bug history
├── INSTRUCTIONS.md            # end-user usage guide (installed with the binary)
├── LICENSE                    # MIT
├── data/
│   ├── menu_disk_contents.json   # 3,259-disk seed (tracked)
│   ├── tosec_titles.json         # hash → title (gitignored, optional)
│   └── screenscraper_cache.json  # genre/synopsis/box-art (gitignored, optional)
├── include/manifest/          # public headers (core + gui/)
├── src/                       # implementation (core + gui/ + cli/)
├── tests/                     # unit tests (database / metadata / identifier)
├── tools/
│   └── screenscraper_sync.py     # offline ScreenScraper fetcher (gitignored)
├── packaging/
│   ├── manifest.desktop
│   └── build-appimage.sh
└── third_party/
    ├── atari-engine/          # vendored Atari Disk Engine subset (MIT, same author)
    └── sqlite3/               # SQLite amalgamation drop-in (gitignored)
```

---

## Dependencies

- **C++20** compiler (GCC 11+ / Clang 14+)
- **CMake** 3.20 or newer
- **Qt 6** Core + Widgets
- **OpenSSL** (SHA1)
- **SQLite 3 amalgamation** — drop `sqlite3.c` + `sqlite3.h` into [third_party/sqlite3/](third_party/sqlite3/) (FTS5 enabled at build time via `SQLITE_ENABLE_FTS5`)
- **GNU Readline** (optional — enables history + line editing in the query shell)
- **Hatari** on `$PATH` (optional — required only to actually launch disks)

On Debian / Ubuntu / Mint:

```sh
sudo apt install build-essential cmake \
                 qt6-base-dev qt6-tools-dev \
                 libssl-dev libreadline-dev \
                 hatari
```

---

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

The `manifest` binary lands in `build/`.

### AppImage

```sh
packaging/build-appimage.sh
```

Produces `dist/manifest-x86_64.AppImage`. First run downloads
`linuxdeploy` + the Qt plugin into `packaging/.tools/` (cached for subsequent builds).

### Install to prefix

```sh
sudo cmake --install build
# installs /usr/local/bin/manifest + /usr/local/share/applications/manifest.desktop
# + /usr/local/share/manifest/{INSTRUCTIONS.md,README.md,LICENSE,data/*}
```

---

## Quick Start

```sh
# 1. Scan a folder into a fresh database (quick mode for a fast first pass)
./build/manifest scan ~/AtariCollection --db ~/manifest.db --quick

# 2. Later, upgrade to deep analysis:
./build/manifest scan ~/AtariCollection --db ~/manifest.db

# 3. Browse visually
./build/manifest --db ~/manifest.db

# 4. Or search from the shell
./build/manifest query --db ~/manifest.db --find "Dungeon Master"
```

See [INSTRUCTIONS.md](INSTRUCTIONS.md) for the full walk-through, including
TOSEC DAT import, menu-disk content cataloguing, ScreenScraper enrichment,
and the complete GUI keyboard-shortcut map.

---

## Testing

```sh
ctest --test-dir build --output-on-failure
```

Three suites: `test_database`, `test_metadata`, `test_identifier`.

---

## Reporting bugs

Check [BUGS.md](BUGS.md) first — a bug with a similar symptom may already be
catalogued. If new, file it there with symptom → root cause → fix.

---

## License

[MIT](LICENSE). The vendored Atari Disk Engine subset is authored by the same
copyright holder and distributed under the same terms.

---

## Author

Shane Hartley ([@Darian-Frey](https://github.com/Darian-Frey))
