# ManifeST

A batch disk image cataloguer for Atari ST software collections.

Point `manifest` at a folder of `.ST`, `.MSA`, `.DIM`, or `.STX` images and it walks the tree, mounts each image via a vendored subset of the Atari Disk Engine, extracts metadata, identifies the title, and persists everything to a local SQLite database. Browse with either a Qt 6 GUI (searchable table, multi-disk sets, duplicates, tag sidebar, launch-in-Hatari button) or a readline query shell.

**Status:** v0.1.0-dev — CLI and GUI both functional, all 3 unit tests passing. Linux-first; Windows port planned on a separate branch.

For step-by-step usage see [INSTRUCTIONS.md](INSTRUCTIONS.md).

---

## Features

### Cataloguing
- Recursive walk of `.ST` / `.MSA` / `.DIM` / `.STX` images
- Boot sector parsing, volume label extraction (direct raw-sector scan), full root + subdir file listing
- SHA1 hashes for both the raw image and every file it contains
- Launcher heuristic — flags the lone `.PRG` / `.APP` / `.TOS` in root
- MSA + STX decompression via the vendored engine
- Cracker-group detection (D-Bug, Pompey, Medway, Elite, TRSI, Replicants, Copylock, etc.)

### Identification
- **TOSEC filename parse** — `Title (Year)(Publisher)[flags]`
- **Heuristics** — volume label → OEM → lone-launcher filename
- **Hash lookup** — optional `data/tosec_titles.json`
- TOSEC flag tags: `cracked`, `trained`, `hacked`, `alt`, `verified`
- Multi-disk auto-tag: `multidisk-NofM`

### Grouping
- Duplicate detection via image SHA1
- Multi-disk set detection (TOSEC `(Disk N of M)` + volume-label prefix with contiguous-numbering validation)
- Persistent `disk_sets` table, rebuilt after every scan

### Search
- SQLite **FTS5 full-text index** with the trigram tokenizer — substring match across title, filename, volume label, and aggregated file names (arbitrary substring, like `kan` → Arkanoid)
- Schema-versioned migrations (current `user_version = 2`)

### GUI (Qt 6 Widgets)
- Sortable, live-filterable main table
- Left sidebar: All Disks · Duplicates · Tags · Multi-disk Sets (click any node to filter)
- Bottom Details dock (path, SHA1, geometry, tags, file listing with ★ for launcher)
- Background scan on a `QThread` with progress bar + Cancel
- Toggleable `Identified?` column
- `Launch in Hatari` button + right-click context menu (Launch / Show in Files / Copy Path / Remove)
- Persistent window state, last DB path, last scan folder, column visibility
- Keyboard shortcuts: `Ctrl+O` open DB · `Ctrl+S` scan · `F5` rescan · `Ctrl+F` focus search · `Ctrl+Q` quit

### CLI
- `manifest scan <folder>` — headless cataloguer
- `manifest query` — interactive `manifest>` shell with `find / list / info / launch / tags / dupes / sets`
- `manifest query --find <term>` — one-shot search (exit 0 on match, 1 otherwise)
- `manifest launch <id>` — fire-and-forget Hatari launcher
- `manifest inspect <path>` — single-disk diagnostic (no DB writes)
- readline + `~/.manifest_history` when `libreadline-dev` is installed; `std::getline` fallback otherwise

---

## Repository Layout

```
ManifeST/
├── CMakeLists.txt
├── CLAUDE.md                  # architecture & conventions
├── ROADMAP.md                 # phase-by-phase progress log
├── INSTRUCTIONS.md            # end-user usage guide
├── LICENSE                    # MIT
├── data/                      # optional tosec_titles.json lives here
├── include/manifest/          # public headers (core + gui/)
├── src/                       # implementation (core + gui/ + cli/)
├── tests/                     # unit tests (metadata / identifier / database)
├── packaging/                 # Linux .desktop file
└── third_party/
    ├── atari-engine/          # vendored Atari Disk Engine subset (MIT, same author)
    └── sqlite3/               # SQLite amalgamation (drop-in, gitignored)
```

---

## Dependencies

- **C++20** compiler (GCC 11+ / Clang 14+)
- **CMake** 3.20 or newer
- **Qt 6** Core + Widgets
- **OpenSSL** (SHA1)
- **SQLite 3 amalgamation** — `sqlite3.c` + `sqlite3.h` dropped into [third_party/sqlite3/](third_party/sqlite3/) (the zip at https://sqlite.org/download.html works directly)
- **GNU Readline** (optional — enables history + line editing in the query shell)
- **Hatari** on `$PATH` (optional — required only to actually launch disks from the GUI / CLI)

On Debian / Ubuntu:

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

```sh
# Optional: install to /usr/local
sudo cmake --install build
```

---

## Quick Start

```sh
# 1. Scan a folder into a fresh database
./build/manifest scan ~/AtariCollection --db ~/manifest.db

# 2. Launch the GUI
./build/manifest --db ~/manifest.db

# 3. Or use the query shell
./build/manifest query --db ~/manifest.db
```

See [INSTRUCTIONS.md](INSTRUCTIONS.md) for the full walk-through.

---

## Testing

```sh
ctest --test-dir build --output-on-failure
```

Three suites: `test_database`, `test_metadata`, `test_identifier`.

---

## License

[MIT](LICENSE). The vendored Atari Disk Engine subset is authored by the same copyright holder and distributed under the same terms.

---

## Author

Shane Hartley ([@Darian-Frey](https://github.com/Darian-Frey))
