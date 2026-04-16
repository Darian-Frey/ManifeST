# ManifeST — Improvements & Enhancement Proposals

Generated: 2026-04-16  
Based on: v0.1.0-dev README, repository structure, and AtariMania site analysis

---

## 1. AtariMania Integration

### 1.1 Why AtariMania Is the Best Enrichment Source for ST

AtariMania (atarimania.com) is the most comprehensive and accurate Atari ST games database
in existence. Unlike TOSEC — which encodes metadata in filenames only — AtariMania has
per-title records with fields that ManifeST currently cannot derive from the disk image itself:

| Field            | Currently in ManifeST? | AtariMania has it? |
|------------------|------------------------|---------------------|
| Title            | Heuristic / TOSEC      | ✓ authoritative     |
| Publisher        | TOSEC filename         | ✓ authoritative     |
| Year             | TOSEC filename         | ✓ authoritative     |
| Country          | ✗                      | ✓                   |
| Genre            | ✗                      | ✓ (detailed)        |
| Resolution       | ✗                      | ✓ Low/Med/High/VGA  |
| ST hardware type | ✗                      | ✓ ST/STe/TT/Falcon  |
| Dump status      | TOSEC flags only       | ✓ richer            |
| Screenshots      | ✗                      | ✓                   |
| Box art          | ✗                      | ✓                   |
| Distributor      | ✗                      | ✓                   |
| Game ID (unique) | ✗                      | ✓ (URL integer)     |

### 1.2 AtariMania Has No Public API

AtariMania does not offer an official API or data export. The data lives in HTML pages
structured as:

```
https://www.atarimania.com/list_games_atari-st-alphabetical_letter_A_S_G.html
https://www.atarimania.com/game-atari-st-arkanoid_10058.html
```

The list pages paginate 25/50/100/200 entries per page. The per-game detail page contains
all the rich metadata. The community has noted that an AtariMania API would be highly
desirable but does not currently exist.

### 1.3 Recommended Approach: Offline Scrape → Static JSON

**Do not scrape AtariMania at scan-time.** Network I/O per disk would slow a 4,500-image
scan to hours, and hammering the site repeatedly is hostile to a community resource.

Instead, build a **one-time offline scraper** that produces a static
`data/atarimania_st.json` bundle. ManifeST then loads this at startup and uses it as an
enrichment source during identification, the same way it already uses `data/tosec_titles.json`.

**Scraper design (Python, one-shot, polite):**

```python
import requests, time, json
from bs4 import BeautifulSoup

BASE = "https://www.atarimania.com"
LETTERS = list("ABCDEFGHIJKLMNOPQRSTUVWXYZ") + ["1"]
DELAY = 1.5  # seconds between requests — be polite

games = []

for letter in LETTERS:
    page = 1
    while True:
        url = (f"{BASE}/list_games_atari-st-p_total-page-step-letter"
               f"_XXX-{page}-100-{letter}_S_G.html")
        # fetch list page, parse game links + basic metadata
        # follow each game link for full detail
        time.sleep(DELAY)
        # ... break when no Next page
```

Each game detail page yields: title, publisher, distributor, year, country, genre,
resolution, ST type, dump status, AtariMania game ID (from URL integer).

**Output schema for `data/atarimania_st.json`:**

```json
{
  "version": 1,
  "generated": "2026-04-16",
  "games": [
    {
      "id": 10058,
      "title": "Arkanoid",
      "publisher": "Taito",
      "distributor": "Imagine",
      "year": 1987,
      "country": ["United Kingdom"],
      "genre": "Arcade - Breakout",
      "resolution": ["Low"],
      "st_type": ["ST", "STe"],
      "dump": "Good Dump",
      "title_normalised": "arkanoid"
    }
  ]
}
```

`title_normalised` is lowercase, punctuation-stripped — used for fuzzy matching.

### 1.4 Integration in ManifeST C++ Identifier

Add a new `AtariManiaDb` class alongside the existing `TosecTitleDb`:

```cpp
// include/manifest/atarimanidb.h
class AtariManiaDb {
public:
    explicit AtariManiaDb(const std::filesystem::path& json_path);

    struct Entry {
        int         id;
        std::string title;
        std::string publisher;
        int         year;
        std::string genre;
        std::string resolution;   // "Low" | "Medium" | "High" | "VGA"
        std::string st_type;      // "ST" | "STe" | "TT" | "Falcon"
        std::string country;
    };

    std::optional<Entry> lookup(const std::string& normalised_title) const;
    std::optional<Entry> lookup_fuzzy(const std::string& candidate,
                                      int threshold = 85) const;

private:
    std::unordered_map<std::string, Entry> index_;
};
```

The identifier tries AtariMania lookup after the TOSEC hash and heuristic passes,
using the already-identified title as the key. A fuzzy match (edit-distance based)
handles minor title spelling variations between TOSEC and AtariMania conventions.

### 1.5 Schema Extensions for AtariMania Fields

Add to the `disks` table (migration `user_version = 3`):

```sql
ALTER TABLE disks ADD COLUMN genre         TEXT;
ALTER TABLE disks ADD COLUMN resolution    TEXT;  -- Low/Medium/High/VGA
ALTER TABLE disks ADD COLUMN st_type       TEXT;  -- ST/STe/TT/Falcon030
ALTER TABLE disks ADD COLUMN country       TEXT;
ALTER TABLE disks ADD COLUMN atarimania_id INTEGER;
ALTER TABLE disks ADD COLUMN enrichment_source TEXT; -- "tosec"|"atarimania"|"heuristic"
```

Also extend the FTS5 index to include `genre` and `country` for filtering.

---

## 2. Performance at Scale (4,500+ Images)

### 2.1 Incremental Scanning — Critical at This Scale

Currently, every `manifest scan` re-processes every image from scratch. At 4,500 images
this is painful, especially if only a handful of new images were added.

**Add change detection before hashing:**

```cpp
// Before computing SHA1, check if the image is already catalogued
// and its mtime + file size match the stored values.
// If both match, skip re-hashing and re-parsing entirely.
struct DiskRecord {
    // ... existing fields ...
    std::time_t  scanned_mtime;
    std::uintmax_t scanned_size;
};
```

Add `scanned_mtime` and `scanned_size` to the `disks` table. During a rescan, stat the
file first. If mtime and size are unchanged, the record is still valid — skip it.

This should reduce a full rescan of a stable 4,500-image collection from minutes to seconds.

### 2.2 tosec_titles.json Load-Once

If the JSON enrichment file is parsed inside the per-image identification loop, it's
being parsed 4,500 times. Ensure `TosecTitleDb` (and `AtariManiaDb`) are constructed
once before the scan loop and passed by const reference into each `DiskIdentifier`
invocation.

### 2.3 MSA/STX Decompression Caching

MSA decompression is CPU-intensive. For rescans where the image hasn't changed (detected
via mtime/size above), the decompressed FAT12 data never needs to be re-derived. Consider
caching the decompressed image to a temp file keyed by SHA1, or simply relying on the
incremental scan skip to avoid re-decompressing unchanged images.

### 2.4 Parallel Scan with std::execution or a Thread Pool

Qt already uses a `QThread` for the background scan. Within that thread, the per-image
pipeline (stat → hash → mount → identify → upsert) is embarrassingly parallel up to the
SQLite write step.

Consider a small thread pool (e.g. `std::thread` × CPU count, or a simple
`std::async`-based fan-out) feeding results into a single writer thread that owns the
SQLite connection. At 4,500 images, parallelising the read/hash/identify phase alone
could cut scan time by 4–8× on a multi-core machine.

SQLite write serialisation is straightforward — WAL mode (`PRAGMA journal_mode=WAL`)
allows concurrent reads and a single-writer model without contention.

---

## 3. Cracker Group Detection Expansion

The current cracker list in the identifier covers: D-Bug, Pompey, Medway, Elite, TRSI,
Replicants, Copylock. Based on scene records, the following major groups are missing and
should be added:

```cpp
// Suggested additions to cracker_groups[] or equivalent table:
"Automation",      // 512 disks — the largest UK label
"LSD",             // predecessor to Automation (disks 1-50)
"Was (Not Was)",   // / Vapour — key early UK cracker, LSD era
"Cynix",           // Zippy's post-Medway group
"Fuzion",          // major French group, 200+ menus
"Empire",          // ~150 releases, top 3 by volume
"MCA",             // Dutch, The Magician — pro software cracks
"Hotline",         // Dutch, merged with MCA into Elite
"Blade Runners",   // 137 releases
"ICS",             // 99 releases, ran BBS HQ
"Delight",         // German, 57 releases
"Vectronix",       // German, active into late 90s
"Superior",        // Finnish (Flame of Finland)
"FOF",             // Flame of Finland alias
"Mad Vision",      // French
"POV",             // Point of View, French
"Bad Brew Crew",
"Dream Weavers",
"Fuzion",
"Boss",            // German, early scene
"42 Crew",         // German, Fastcopy author
"Delta Force",     // German, tool releases too
"TCB",             // The CareBears — primarily demo but cracked
"Sewer Software",  // Corporation member
"BBC",             // UK, Automation lineage
"Euroswap",
"FOFT",            // Federation of Free Traders
"Supremacy",
"Impact",
"Scottish Crackin Crew",
"Midland Boyz",    // Leicestershire, 63 disks
```

Detection should check: boot sector OEM string, scrolltext content (already parsed by the
menu scanner), intro executable filename patterns (e.g. `CYNIX.PRG`, `POMPEY.PRG`).

---

## 4. Menu Scanner Result Integration

The heuristic menu scanner is extracting game title lists from scrolltexts. These should
be persisted into the database rather than just used during identification.

**Suggested additions:**

```sql
-- New table to store menu content extracted by the scanner
CREATE TABLE menu_contents (
    id          INTEGER PRIMARY KEY,
    disk_id     INTEGER REFERENCES disks(id),
    item_index  INTEGER,          -- position on the menu (0-based)
    game_title  TEXT,             -- as extracted from the menu/scrolltext
    confidence  REAL,             -- 0.0–1.0 from heuristic
    source      TEXT              -- "menu_entry" | "scrolltext" | "filename"
);
```

This enables: searching by game title even when ManifeST can't fully identify the disk,
showing users the full contents of a menu disk in the Details dock, and cross-referencing
which menu disks contain a given game ("which disk has Bubble Bobble?").

The GUI Details dock should display `menu_contents` rows for menu-type disks — a simple
`QListWidget` below the file listing would suffice.

---

## 5. Atari Legend / Stonish Data Integration

Atari Legend (atarilegend.com) absorbed the Stonish project and has catalogued over 1,600
tested menu disks with per-disk content lists. This is complementary to AtariMania (which
covers commercial titles) — Atari Legend covers the cracker/menu scene specifically.

Atari Legend does not have a public API but its menu set pages are structured HTML.
A one-shot scraper producing `data/atarilegend_menus.json` would give ManifeST:

- Canonical group name per menu series (Automation, D-Bug, Pompey Pirates, Midland Boyz…)
- Menu number within the series
- Known game contents per disk
- Preservation status (complete / missing / damaged)

This would dramatically improve identification of menu disks, where TOSEC naming is
inconsistent and AtariMania only covers individual commercial titles.

---

## 6. Export Command

Add `manifest export` to the CLI:

```
manifest export --db ~/manifest.db --format csv > collection.csv
manifest export --db ~/manifest.db --format json > collection.json
manifest export --db ~/manifest.db --format m3u --group-sets
```

M3U export is particularly useful: it lets any emulator (Hatari, Steem, etc.) load
multi-disk sets as a playlist without manual disk-swapping configuration.

---

## 7. `manifest inspect` — Enriched Output

The current `manifest inspect <path>` does single-disk diagnostics. At minimum it should
also report:

- Whether the disk matches any known AtariMania record (once integrated)
- Whether it matches any known Atari Legend menu series entry
- Whether the menu scanner found parseable content and what it found
- A confidence score for the identification

---

## 8. SQLite FTS5 Tokenizer Note

The trigram tokenizer used by ManifeST (`tokenize="trigram"`) requires **SQLite 3.35.0
or later** (released 2021-03-12). On Ubuntu 22.04, the system SQLite is 3.37 — fine.
On Ubuntu 20.04, it's 3.31 — trigram is not available, and the FTS5 index creation will
silently fail or error.

Since ManifeST vendors the SQLite amalgamation via `third_party/sqlite3/`, this is not
actually a problem — the vendored version controls what's available. But the README and
`INSTRUCTIONS.md` should note that the system `libsqlite3-dev` is NOT used; the vendored
amalgamation must be present. Add a CMake check that errors clearly if the amalgamation
files are missing rather than failing at link time.

---

## 9. Minor GUI Improvements

- **Group column in main table** — once cracker-group detection is expanded, expose it
  as a sortable/filterable column alongside the existing tag sidebar node.
- **Genre column** — once AtariMania enrichment is in place, genre makes a useful
  filter axis alongside the existing tag sidebar.
- **ST hardware type indicator** — a small icon or badge on each row (ST / STe / Falcon)
  would help users quickly find hardware-specific images.
- **Preservation gap indicator** — for menu series where Atari Legend reports missing
  disks, show a warning icon in the sidebar set view.
- **Scrolltext viewer** — a read-only text area in the Details dock showing the raw
  scrolltext extracted by the menu scanner would be genuinely delightful for the
  nostalgia factor and useful for manual identification.

---

---

## 10. Additional Online Database Sources

Several further databases can improve disk recognition and metadata enrichment beyond
AtariMania and Atari Legend. They fall into three categories: hash-based matchers,
API-accessible databases, and scene-specific archives.

### 10.1 TOSEC DAT Files — Hash-Based Matching (Highest Priority)

ManifeST currently parses TOSEC *filenames* to extract title/year/publisher. The much
more reliable approach is to parse the TOSEC **DAT files** directly and match by hash.

The TOSEC DAT files are XML documents that record CRC32, MD5, and SHA1 for every known
image. A disk image renamed, re-packed, or sourced from a non-TOSEC archive will still
match if its hash is in the DAT. This eliminates the entire class of "correct image,
wrong filename" failures.

The latest TOSEC release is **2025-03-13**, which included a major Atari ST DAT overhaul.
The DAT files are freely downloadable from tosecdev.org.

**Integration approach:**

Build a `TosecDatDb` class that parses the Atari ST DAT XML files at startup and builds
an in-memory hash map keyed by SHA1 (and optionally CRC32 for speed):

```cpp
// include/manifest/tosecdatdb.h
class TosecDatDb {
public:
    // Load all *.dat files from a directory
    explicit TosecDatDb(const std::filesystem::path& dat_dir);

    struct Entry {
        std::string title;
        std::string publisher;
        int         year = 0;
        std::string flags;      // [cr], [a], [b], [t], [h] etc.
        std::string sha1;
        std::string md5;
        std::string crc32;
    };

    std::optional<Entry> lookup_sha1(const std::string& sha1) const;
    std::optional<Entry> lookup_crc32(const std::string& crc32) const;

private:
    std::unordered_map<std::string, Entry> by_sha1_;
    std::unordered_map<std::string, Entry> by_crc32_;
};
```

The identifier should try DAT hash lookup *first*, before filename parsing or heuristics,
as it is the most authoritative source. Add a `data/tosec_dats/` directory to the repo
layout for users to drop DAT files into.

The TOSEC flag set is also worth parsing properly into structured tags:

| TOSEC flag | ManifeST tag      |
|------------|-------------------|
| `[cr]`     | `cracked`         |
| `[cr GRP]` | `cracked` + group |
| `[a]`      | `alt`             |
| `[b]`      | `bad-dump`        |
| `[t]`      | `trained`         |
| `[h]`      | `hacked`          |
| `[!]`      | `verified`        |
| `[f]`      | `fixed`           |

### 10.2 ScreenScraper — Free Hash-Based API

ScreenScraper (screenscraper.fr) is a community-maintained retro gaming database that
**matches by file checksum** (CRC32/MD5/SHA1), making it ideal for ManifeST's pipeline.
It covers the Atari ST platform and returns: title, publisher, year, genre, synopsis,
screenshots, box art, and wheel images.

Key facts:
- **Free** after opening a developer account via their Web Service API
- Data redistributed under Creative Commons
- Matching is hash-based — works regardless of filename
- Supports batch requests; rate limit applies per account tier
- Already used by EmulationStation, RetroArch, Pegasus, LaunchBox, and others

**Integration approach:** One-shot bulk lookup script (Python) that takes ManifeST's
SQLite database, reads all SHA1 hashes for unidentified or partially-identified disks,
queries ScreenScraper's API, and writes results to a `data/screenscraper_cache.json`
bundle. ManifeST then loads this statically — no runtime network dependency.

The ScreenScraper API endpoint for a single game lookup by hash:
```
https://www.screenscraper.fr/api2/jeuInfos.php
    ?devid=YOUR_DEV_ID&devpassword=YOUR_DEV_PASS
    &softname=manifest&output=json
    &systemeid=42          # 42 = Atari ST
    &md5=<hash>
```

System ID 42 is Atari ST on ScreenScraper.

### 10.3 MobyGames API

MobyGames lists **2,506 Atari ST titles** with rich metadata: title, publisher, year,
genre, developer credits, descriptions, screenshots, and cover art. The API is
well-documented REST with JSON responses.

- **Platform ID** for Atari ST on MobyGames: `5`
- Rate limit: 720 requests/hour (non-commercial tier)
- **Cost:** API keys are no longer free as of September 2024; a paid hobbyist
  subscription is required

Recommended approach: one-shot offline scrape via the API to build
`data/mobygames_st.json`, then use statically. At 2,506 titles and 720 req/hour, a
complete pull takes under 4 hours. MobyGames is strongest on developer/composer credits
and long-form descriptions — fields AtariMania and TOSEC don't provide.

```python
import requests, time, json

API_KEY = "YOUR_KEY"
BASE    = "https://api.mobygames.com/v1"
PLATFORM_ST = 5

games = []
offset = 0
while True:
    r = requests.get(f"{BASE}/games",
                     params={"platform": PLATFORM_ST,
                             "api_key": API_KEY,
                             "offset": offset,
                             "limit": 100})
    batch = r.json().get("games", [])
    if not batch:
        break
    games.extend(batch)
    offset += len(batch)
    time.sleep(5)  # respect rate limit
```

### 10.4 Demozoo — Daily Database Dump (Scene / Demo Side)

Demozoo (demozoo.org) is the authoritative database for the demoscene — covering demos,
intros, cracktros, menu disk intros, and cracker group metadata. It has extensive Atari
ST coverage including group member lists, release dates, and production credits.

A **daily database dump** is available, making this zero-cost and offline-friendly.
Filter the dump for Atari ST productions (platform ID 9 on Demozoo) to build a local
index.

Demozoo covers the scene side that AtariMania and TOSEC don't focus on:

- Cracktro / intro identification by group
- Cracker group member lists (useful for group detection)
- Demo disk identification
- Cross-links between groups (e.g. Automation → D-Bug lineage)
- Production release dates from party archives

**Integration approach:** Download the daily dump, filter for ST productions, build
`data/demozoo_st.json`. In the identifier, after cracker-group detection fires, look up
the group in Demozoo to enrich the group record with founding year, country, and
notable members.

Demozoo REST API is also available for targeted lookups:
```
https://demozoo.org/api/v1/productions/?platform=9&supertype=production
```

### 10.5 Pouët

Pouët (pouet.net) is the other major demoscene database, complementary to Demozoo.
It covers some ST productions not on Demozoo, and its rating/comment data gives a sense
of community significance for demo disks. No formal download API, but the production
list pages are scrapeable. Lower priority than Demozoo but useful as a gap-filler for
the demo portion of the collection.

### 10.6 IGDB (via Twitch)

IGDB (igdb.com, owned by Twitch) has reasonable Atari ST coverage with a free API —
requires a Twitch developer account and client ID/secret, but no payment. Returns:
title, summary, genre, release date, cover art, and screenshots.

Known limitation: IGDB accuracy on older platforms is inconsistent (the community noted
Dungeon Master's release date was wrong by two years for the ST version). Use as a
tertiary fallback after AtariMania and MobyGames, not as a primary source.

```
https://api.igdb.com/v4/games
    ?search=<title>
    &fields=name,first_release_date,genres,summary,cover
    &platforms=63   # 63 = Atari ST/STE on IGDB
```

### 10.7 Fujiology Archive

Fujiology (compiled by Lotek Style / .tSCc.) is the largest Atari demoscene file
archive, containing over 11,000 ST files. It is not a queryable database but its
**file manifest** is valuable: cross-referencing ManifeST's SHA1 hashes against the
Fujiology manifest would identify demo disks that appear nowhere else.

The archive is available via Demozoo and scene.org mirrors. Parsing its directory
structure + filenames into a local lookup table (`data/fujiology_manifest.json`) would
complement the TOSEC DAT approach for demo/intro disks specifically.

### 10.8 Source Priority and Fallback Chain

With all sources integrated, the recommended identification pipeline is:

```
1. TOSEC DAT hash match (SHA1/CRC32)        → most authoritative, offline
2. ScreenScraper hash match (MD5/SHA1)      → free API, hash-based
3. AtariMania title lookup (normalised)     → best ST-specific metadata
4. Atari Legend menu series lookup          → best for menu disks
5. Demozoo production lookup               → best for demos/intros
6. Fujiology manifest hash match           → demo gap-filler
7. MobyGames title lookup                  → descriptions, credits
8. IGDB title lookup                       → last resort
9. Internal heuristics (volume/OEM/PRG)    → fallback when all else fails
```

Store `enrichment_source` per disk so users can see how confident each identification
is and which source provided it.

---

## 11. Priority Order (Suggested)

| Priority | Item |
|----------|------|
| 1 (High) | Incremental scan with mtime/size check — biggest QoL win at 4,500 images |
| 2 (High) | TOSEC DAT hash-based matching — replaces filename parsing, highest accuracy |
| 3 (High) | ScreenScraper integration — free, hash-based, covers art and metadata |
| 4 (High) | AtariMania offline scraper + JSON bundle + C++ integration |
| 5 (High) | Persist menu scanner results into `menu_contents` table |
| 6 (Medium) | Demozoo daily dump integration — demo/intro/group metadata |
| 7 (Medium) | Atari Legend menu series data integration |
| 8 (Medium) | Expanded cracker group detection list |
| 9 (Medium) | Parallel scan with thread pool + WAL mode |
| 10 (Medium) | MobyGames one-shot scrape — descriptions and credits |
| 11 (Low) | IGDB integration — tertiary fallback |
| 12 (Low) | Fujiology manifest hash index |
| 13 (Low) | Export command (CSV / JSON / M3U) |
| 14 (Low) | GUI enhancements (genre column, scrolltext viewer) |
