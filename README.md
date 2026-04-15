# ManifeST

A batch disk image cataloguer for Atari ST software collections.

Point `manifest` at a folder of `.ST`, `.MSA`, and `.DIM` images and it will walk the tree, mount each image, extract metadata, identify the title, and persist everything to a local SQLite database. A query CLI lets you find and launch any catalogued disk through Hatari.

**Status:** Scaffolding phase — headers and stubs are in place; implementation is pending.

---

## Features

- Recursive scan of disk image directories (`.ST` / `.MSA` / `.DIM`)
- Boot sector parsing, volume label extraction, full file listing
- SHA1 hashing of raw images and individual files
- Title identification in three passes:
  1. TOSEC filename convention (`Title (Year)(Publisher)[flags].st`)
  2. Volume label / OEM / launcher-filename heuristics
  3. Optional hash lookup against `data/tosec_titles.json`
- Duplicate detection via image SHA1
- Multi-disk set grouping (volume-label prefix + shared launcher hash)
- Interactive query shell with `find`, `info`, `launch`, `tags`, `dupes`

---

## Repository Layout

```
ManifeST/
├── CMakeLists.txt
├── CLAUDE.md                  # project instructions for Claude Code
├── atari-disk-engine/         # single-image library (read-only dependency)
├── data/                      # optional tosec_titles.json lives here
├── include/manifest/          # public headers
├── src/                       # implementation
├── tests/                     # unit tests
└── third_party/sqlite3/       # SQLite amalgamation (drop-in)
```

See [CLAUDE.md](CLAUDE.md) for the full architecture, data flow, and SQLite schema.

---

## Dependencies

- C++20 compiler (GCC 11+ / Clang 14+)
- CMake 3.20 or newer
- [atari-disk-engine](atari-disk-engine/) — vendored as a subdirectory
- OpenSSL (for SHA1)
- GNU Readline (for the interactive shell)
- SQLite 3 amalgamation — drop `sqlite3.c` and `sqlite3.h` into [third_party/sqlite3/](third_party/sqlite3/)

On Debian / Ubuntu:

```sh
sudo apt install build-essential cmake libssl-dev libreadline-dev
```

---

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

The `manifest` binary lands in `build/`.

---

## Usage

```sh
# Scan a folder and populate / update the database
manifest scan ~/AtariCollection/ --db ~/manifest.db

# Incremental rescan — skip images whose hash is already known
manifest scan ~/AtariCollection/ --db ~/manifest.db --incremental

# Interactive query shell
manifest query --db ~/manifest.db

# One-shot query
manifest query --db ~/manifest.db --find "Dungeon Master"

# Launch a catalogued disk via Hatari
manifest launch 42 --db ~/manifest.db
```

Interactive shell commands (`manifest>` prompt):

| Command        | Purpose                                                |
| -------------- | ------------------------------------------------------ |
| `find <term>`  | Full-text search across title, volume label, filenames |
| `list`         | Paginated dump of all catalogued disks                 |
| `info <id>`    | Full record for a disk, including file listing         |
| `launch <id>`  | Exec Hatari with the image path                        |
| `tags <tag>`   | Filter by tag (`game`, `demo`, `utility`, `multidisk`) |
| `dupes`        | List images with matching SHA1                         |
| `quit`         | Exit the shell                                         |

---

## Testing

```sh
ctest --test-dir build --output-on-failure
```

---

## License

[MIT](LICENSE). The vendored Atari Disk Engine subset is authored by the
same copyright holder and distributed under the same terms.

---

## Author

Shane Hartley ([@Darian-Frey](https://github.com/Darian-Frey))
